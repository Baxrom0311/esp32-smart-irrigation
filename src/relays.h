// relays.h — Pump relay control + safety FSM public interface.
//
// All relay GPIO writes go through this module. Web/auto code
// requests state changes via requestPump(); the FSM validates
// against dry-run, runtime and sensor lockouts before granting.

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "state.h"

void relaysBegin(SystemState& state);
void relaysTick(SystemState& state);

// Request a pump state change.
//
// `desired` = true asks the FSM to run pump `pumpIdx`; false asks
// it to stop. For ON requests, `durationSec` schedules an
// auto-off `durationSec` seconds from now (0 = unlimited; the
// existing maxPumpSeconds runtime trip still applies). For OFF
// requests, `durationSec` is ignored and any pending timer is
// cleared.
//
// The duration is enforced by relaysTick(), so existing
// dry-run / runtime / sensor lockouts continue to win — the
// timer is an additional auto-off, not a privileged path that
// bypasses safety.
PumpRequestResult requestPump(SystemState& state,
                              uint8_t pumpIdx,
                              bool desired,
                              PumpRequestSource source,
                              uint32_t durationSec = 0);

// Human-readable reason for a denied/granted result; safe to embed
// in JSON or display banners.
const char* pumpResultText(PumpRequestResult r);

// Human-readable label for a pump FSM state. Stable strings; safe
// to ship in JSON and display banners. Used by the dashboard to
// surface lockout reasons to the operator.
const char* pumpFsmText(PumpFsm s);

// Compose a one-line operator-facing error/status string summarising
// the worst condition across two pumps. Returns an empty string if
// both pumps are in normal Off/On states. The string fits in
// SystemState::lastError (64 chars). Pure helper so native tests can
// exercise it without Arduino.h.
//
// Priority order (most actionable first):
//   1. Sensor error      → "tank N sensor error"
//   2. Dry-run lockout   → "tank N water low"
//   3. Runtime lockout   → "pump N runtime limit"
//   4. DHT sensor error  → "DHT sensor error"
void composePumpsErrorMessage(const PumpState pumps[2],
                              bool tank1Err, bool tank2Err,
                              bool dhtErr,
                              char* out, size_t outLen);

// ---------------------------------------------------------------------------
// Pure helpers exposed for native unit tests (no Arduino dependency).
// ---------------------------------------------------------------------------

struct PumpSafetyInputs {
    uint8_t tankPct;
    bool    tankErr;
    PumpFsm fsm;
    uint8_t minWaterPct;
};

// Evaluates whether an ON request would be granted right now. OFF
// requests are always allowed and do not need to call this. The
// function is order-aware: hard sensor/dry-run conditions take
// precedence over historical lockouts so the most informative
// reason is reported to the operator.
PumpRequestResult evaluatePumpOnRequest(const PumpSafetyInputs& in);

// Auto-mode demand decision based on soil reading and threshold,
// applying SAFETY_HYSTERESIS_PCT to prevent thrash near the
// threshold. `currentDemand` is the previous tick's demand state.
bool evaluateAutoDemand(uint8_t soilPct,
                        bool soilErr,
                        uint8_t thresholdPct,
                        bool currentDemand);

// Returns true if the runtime-lockout cooldown set by `lockoutUntilMs`
// is still active at the given `now` timestamp.
//
// `millis()` wraps every ~49.7 days. A naive `now < lockoutUntilMs`
// (or `now >= lockoutUntilMs`) breaks across that wrap: a deadline
// stored as `now + COOLDOWN` can wrap below the current `now`,
// causing the comparison to silently invert and either release a
// pump that should still be locked out or hold one forever.
//
// This helper uses signed-difference arithmetic on uint32_t, which
// is the canonical Arduino-safe pattern: the wrap behaviour of
// unsigned subtraction combined with a final cast to int32_t treats
// the result as a relative offset rather than an absolute ordering.
//
// Convention: `lockoutUntilMs == 0` means "no active cooldown" and
// always returns false. Lock holders must therefore avoid storing
// 0 as a real deadline; any boot-time wrap window (the first ms of
// uptime) is too short to matter for the 5-minute cooldown.
//
// The same helper is reused for the manual run-duration timer
// (PumpState::runUntilMs); the rollover semantics are identical.
bool isLockoutActive(uint32_t now, uint32_t lockoutUntilMs);

// Compute a never-zero millis() deadline `now + offsetMs` apart. 0
// is the "no active timer" sentinel for both lockoutUntilMs and
// runUntilMs; callers must avoid storing 0 as a real deadline,
// which is otherwise possible at the precise wrap boundary. Bumps
// to 1 in the degenerate case so isLockoutActive() stays consistent.
uint32_t computeDeadline(uint32_t now, uint32_t offsetMs);
