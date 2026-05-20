// ratelimit.h — Token-bucket rate limiter (pure helpers + per-IP table).
//
// Two layers:
//
//   1) `TokenBucket` + the ms-based refill helpers below are pure C++
//      (no Arduino dependency) so they can be exercised by the
//      native test environment. They implement classic
//      token-bucket arithmetic with integer refill: tokens are added
//      one-per-`msPerToken` based on signed-difference time math,
//      which is rollover-safe across the millis() wrap.
//
//   2) The Arduino-only `rateLimitCheck()` looks up (or evicts) a
//      per-(IP, endpoint) bucket from a fixed-size LRU table and
//      forwards to the pure helpers. Used by webserver.cpp on the
//      mutating endpoints (`/api/pump`, `/api/settings`,
//      `/api/calibrate`, `/api/reset`).
//
// Policy choice rationale (defaults — see ratelimit.cpp):
//
//   /api/pump      — capacity 10, refill 1 token/sec. Operator can
//                    spam-click during testing without hitting 429,
//                    sustained traffic is bounded.
//   /api/settings  — capacity  5, refill 1 token / 5 sec. Settings
//                    changes are rare; a misbehaving client cannot
//                    grind NVS.
//   /api/calibrate — shares the settings policy.
//   /api/reset     — capacity  2, refill 1 token / 60 sec. Factory
//                    reset is the most destructive endpoint we
//                    expose; rate limit it hardest.

#pragma once

#include <stddef.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Pure token bucket (no Arduino dependency).
//
// Tokens are stored as integer counts. Sub-token elapsed time is
// accumulated in `pendingMs`, which is folded into a new token once
// it crosses `msPerToken`. This avoids floating-point in firmware
// and keeps refill deterministic for unit tests.
// ---------------------------------------------------------------------------

struct TokenBucket {
    uint32_t lastRefillMs;   // millis() of the last refill
    uint32_t tokens;         // current count, ≤ capacity
    uint32_t capacity;       // max tokens
    uint32_t msPerToken;     // refill cadence (≥1)
    uint32_t pendingMs;      // unaccounted elapsed ms (< msPerToken)
};

// Initialise a bucket to FULL. `capacity` must be ≥1; `msPerToken`
// is clamped to ≥1 so the helpers never divide by zero.
void tokenBucketInit(TokenBucket& b,
                     uint32_t capacity,
                     uint32_t msPerToken,
                     uint32_t nowMs);

// Refill the bucket based on the elapsed ms since `lastRefillMs`.
// Rollover-safe: a backwards `nowMs` (which can happen if the caller
// resets the time source) is treated as zero elapsed ms rather than
// a huge wrap-around credit. Tokens saturate at `capacity`.
void tokenBucketRefill(TokenBucket& b, uint32_t nowMs);

// Refill, then try to consume `cost` tokens. Returns true on
// success (tokens are decremented). On failure the bucket is
// untouched apart from the refill; the caller can use
// `tokenBucketRetryAfterSec()` to compute a Retry-After header.
bool tokenBucketTryConsume(TokenBucket& b, uint32_t cost, uint32_t nowMs);

// Seconds until at least `cost` tokens would be available, rounded
// UP. Returns 0 if `cost` tokens are already available now (no
// refill performed — the caller should refill first if it wants a
// post-refill answer). The cap at 3600 mirrors the largest
// Retry-After value the operator-facing dashboard would surface
// usefully; truly degenerate cases would still return a sane upper
// bound.
uint32_t tokenBucketRetryAfterSec(const TokenBucket& b, uint32_t cost);

// ---------------------------------------------------------------------------
// Per-(IP, endpoint) table — Arduino-only.
// ---------------------------------------------------------------------------

// Endpoint policy slots. Order matters only insofar as it matches
// the policy table in ratelimit.cpp.
enum class RateLimitEndpoint : uint8_t {
    Pump      = 0,
    Settings  = 1,
    Calibrate = 2,
    Reset     = 3,
    Count     = 4,
};

// Reset the per-IP table at boot. Safe to call repeatedly; existing
// entries are zeroed.
void rateLimitInit();

// Try to acquire 1 token for (`ipv4`, `endpoint`) at `nowMs`.
// Returns true if the request is allowed, false if rate-limited.
// On rate-limit, `retryAfterSec` is set to the worst-case wait
// (clamped to ≥1 so the HTTP `Retry-After` header is meaningful).
bool rateLimitCheck(uint32_t ipv4,
                    RateLimitEndpoint endpoint,
                    uint32_t nowMs,
                    uint32_t& retryAfterSec);
