// ratelimit.cpp — Token-bucket arithmetic + per-IP table.
//
// The pure half (tokenBucket*) is testable under the native env;
// the per-IP table requires only stdint and is ifdef-free, so the
// host build links it too. The lookup table size (16) is sized for
// 4 simultaneous AP clients × 4 endpoints with one slack slot — an
// AP that hosts more clients than the typical 1–2 still has a
// well-defined LRU eviction policy.

#include "ratelimit.h"

#include <string.h>

// ---------------------------------------------------------------------------
// Pure token-bucket helpers
// ---------------------------------------------------------------------------

void tokenBucketInit(TokenBucket& b,
                     uint32_t capacity,
                     uint32_t msPerToken,
                     uint32_t nowMs) {
    if (capacity == 0)   capacity   = 1;
    if (msPerToken == 0) msPerToken = 1;
    b.capacity     = capacity;
    b.msPerToken   = msPerToken;
    b.tokens       = capacity;     // start full so first request is allowed
    b.pendingMs    = 0;
    b.lastRefillMs = nowMs;
}

// Compute elapsed ms since `lastRefillMs`, treating a backwards
// timestamp (negative signed diff) as zero. Mirrors the rollover-
// safe pattern from relays.cpp: `(int32_t)(now - last)` yields the
// relative offset under a millis() wrap, but at the very first
// refill after init we want a clean 0 instead of a huge value if
// the caller seeded `lastRefillMs` from a future-looking source.
static uint32_t elapsedSince(uint32_t lastMs, uint32_t nowMs) {
    int32_t diff = (int32_t)(nowMs - lastMs);
    if (diff < 0) return 0;
    return (uint32_t)diff;
}

void tokenBucketRefill(TokenBucket& b, uint32_t nowMs) {
    if (b.msPerToken == 0) b.msPerToken = 1;  // defensive
    uint32_t elapsed = elapsedSince(b.lastRefillMs, nowMs);
    b.lastRefillMs = nowMs;

    // Fold leftover sub-token time from the previous refill before
    // dividing — otherwise tiny ticks never accumulate to a token
    // and a slow refill rate effectively never replenishes.
    uint64_t total = (uint64_t)b.pendingMs + (uint64_t)elapsed;
    uint64_t newTokens = total / (uint64_t)b.msPerToken;
    uint64_t remainder = total % (uint64_t)b.msPerToken;

    // Saturate at capacity; the modulo carry is preserved across
    // saturation so a long idle period does not "donate" overflow
    // to a future refill.
    if (newTokens > 0) {
        if (newTokens > (uint64_t)(b.capacity - b.tokens)) {
            b.tokens = b.capacity;
        } else {
            b.tokens += (uint32_t)newTokens;
        }
    }
    // pendingMs is bounded by msPerToken (which fits in uint32_t),
    // so the cast is safe.
    b.pendingMs = (uint32_t)remainder;
}

bool tokenBucketTryConsume(TokenBucket& b, uint32_t cost, uint32_t nowMs) {
    tokenBucketRefill(b, nowMs);
    if (b.tokens < cost) return false;
    b.tokens -= cost;
    return true;
}

uint32_t tokenBucketRetryAfterSec(const TokenBucket& b, uint32_t cost) {
    if (cost <= b.tokens) return 0;
    uint32_t deficit = cost - b.tokens;
    // Time to next token = msPerToken - pendingMs (rounded to
    // nearest second, ceiling). Subsequent tokens add msPerToken
    // each. Example: msPerToken=5000, pendingMs=2000, deficit=2 →
    // (5000-2000) + 5000 = 8000 ms → 8 s.
    uint32_t firstWaitMs = (b.pendingMs >= b.msPerToken)
                            ? 0
                            : (b.msPerToken - b.pendingMs);
    uint64_t totalMs = (uint64_t)firstWaitMs +
                       (uint64_t)(deficit - 1) * (uint64_t)b.msPerToken;
    // Ceiling-divide to seconds. Clamp at 3600 so the HTTP
    // Retry-After header is operator-readable; the caller is welcome
    // to retry sooner — this is a hint.
    uint64_t sec = (totalMs + 999ULL) / 1000ULL;
    if (sec == 0)    sec = 1;       // never report 0 when blocked
    if (sec > 3600)  sec = 3600;
    return (uint32_t)sec;
}

// ---------------------------------------------------------------------------
// Per-(IP, endpoint) lookup table
// ---------------------------------------------------------------------------

namespace {

// Policy table — must match the order of RateLimitEndpoint.
// `capacity` is the burst allowance; `msPerToken` is the steady-
// state refill cadence. Calibrate shares the settings policy
// (similar persistence cost). See ratelimit.h for the rationale.
struct Policy { uint32_t capacity; uint32_t msPerToken; };
constexpr Policy POLICY_TABLE[(size_t)RateLimitEndpoint::Count] = {
    /* Pump      */ { 10, 1000   },   // 1 req/s steady, burst 10
    /* Settings  */ {  5, 5000   },   // 1 req / 5 s
    /* Calibrate */ {  5, 5000   },
    /* Reset     */ {  2, 60000  },   // 1 req / 60 s
};

struct Slot {
    uint32_t          ipv4;        // 0 = empty
    RateLimitEndpoint endpoint;
    TokenBucket       bucket;
    uint32_t          lastUseMs;
};

constexpr size_t TABLE_SIZE = 16;
Slot g_table[TABLE_SIZE];

// Find an existing slot for (ip, endpoint), or nullptr if absent.
Slot* findSlot(uint32_t ipv4, RateLimitEndpoint ep) {
    for (auto& s : g_table) {
        if (s.ipv4 == ipv4 && s.ipv4 != 0 && s.endpoint == ep) {
            return &s;
        }
    }
    return nullptr;
}

// Reserve a slot for (ip, endpoint). If the table is full, evict
// the least-recently-used slot. Returns a freshly-initialised
// Slot pointer (never null).
Slot* reserveSlot(uint32_t ipv4, RateLimitEndpoint ep, uint32_t nowMs) {
    // First pass: an empty slot is always the cheapest target.
    for (auto& s : g_table) {
        if (s.ipv4 == 0) {
            s.ipv4      = ipv4;
            s.endpoint  = ep;
            s.lastUseMs = nowMs;
            const Policy& p = POLICY_TABLE[(size_t)ep];
            tokenBucketInit(s.bucket, p.capacity, p.msPerToken, nowMs);
            return &s;
        }
    }
    // No empty slot — evict LRU. Use signed-diff comparison so we
    // pick the genuinely oldest entry across a millis() wrap.
    Slot* victim = &g_table[0];
    for (auto& s : g_table) {
        int32_t diff = (int32_t)(victim->lastUseMs - s.lastUseMs);
        if (diff > 0) victim = &s;  // s is older than current victim
    }
    victim->ipv4      = ipv4;
    victim->endpoint  = ep;
    victim->lastUseMs = nowMs;
    const Policy& p = POLICY_TABLE[(size_t)ep];
    tokenBucketInit(victim->bucket, p.capacity, p.msPerToken, nowMs);
    return victim;
}

}  // namespace

void rateLimitInit() {
    memset(g_table, 0, sizeof(g_table));
}

bool rateLimitCheck(uint32_t ipv4,
                    RateLimitEndpoint endpoint,
                    uint32_t nowMs,
                    uint32_t& retryAfterSec) {
    if ((size_t)endpoint >= (size_t)RateLimitEndpoint::Count) {
        // Unknown endpoint: fail open (let the caller answer 200/4xx
        // on its own merits) rather than 429-locking it forever. A
        // nonsense value here is a programming bug, not an attack.
        retryAfterSec = 0;
        return true;
    }
    // ipv4=0 maps to "unknown peer". We still rate-limit on the
    // shared 0-key bucket; better to share a global limit than to
    // exempt an unidentified peer entirely.
    Slot* s = findSlot(ipv4, endpoint);
    if (!s) s = reserveSlot(ipv4, endpoint, nowMs);

    s->lastUseMs = nowMs;
    if (tokenBucketTryConsume(s->bucket, 1, nowMs)) {
        retryAfterSec = 0;
        return true;
    }
    retryAfterSec = tokenBucketRetryAfterSec(s->bucket, 1);
    return false;
}
