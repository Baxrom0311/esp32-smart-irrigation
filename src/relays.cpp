// relays.cpp — Pump safety FSM.
//
// Pumps map 1:1 to water tanks: pump 0 → tank 1, pump 1 → tank 2.
// Each pump moves through these states, polled at RELAYS_PERIOD_MS:
//
//   Off ──► On (demand && all gates ok)
//   On  ──► LockoutRuntime  (elapsed ≥ maxPumpSeconds)
//   *   ──► LockoutDryRun   (tank% < minWaterPct)
//   *   ──► LockoutSensor   (tank sensor error)
//
// Lockouts are sticky: dry-run/sensor clear once the underlying
// reading recovers (with SAFETY_HYSTERESIS_PCT to prevent thrash);
// runtime clears after LOCKOUT_RUNTIME_COOLDOWN_MS.
//
// Request sources:
//   - Web (manual mode only): grant if no lockout, ignore otherwise.
//   - Auto: only when settings.autoMode is true; soil < threshold
//     drives demand ON, soil > threshold + hysteresis drives OFF.

#include "relays.h"

#include "config.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef ARDUINO
  #include <Arduino.h>
#endif

const char* pumpResultText(PumpRequestResult r) {
    switch (r) {
        case PumpRequestResult::Granted:        return "ok";
        case PumpRequestResult::DeniedDryRun:   return "dry_run";
        case PumpRequestResult::DeniedSensor:   return "sensor_error";
        case PumpRequestResult::DeniedRuntime:  return "runtime_lockout";
        case PumpRequestResult::DeniedInvalid:  return "invalid_request";
        case PumpRequestResult::DeniedBusy:     return "state_busy";
    }
    return "unknown";
}

const char* pumpFsmText(PumpFsm s) {
    switch (s) {
        case PumpFsm::Off:             return "off";
        case PumpFsm::On:              return "on";
        case PumpFsm::LockoutDryRun:   return "lockout_dry_run";
        case PumpFsm::LockoutRuntime:  return "lockout_runtime";
        case PumpFsm::LockoutSensor:   return "lockout_sensor";
    }
    return "unknown";
}

void composePumpsErrorMessage(const PumpState pumps[2],
                              bool tank1Err, bool tank2Err,
                              bool dhtErr,
                              char* out, size_t outLen) {
    if (out == nullptr || outLen == 0) return;
    out[0] = '\0';

    // Priority 1: sensor errors that are actively gating a pump.
    for (uint8_t i = 0; i < 2; ++i) {
        bool tankErr = (i == 0) ? tank1Err : tank2Err;
        if (tankErr || pumps[i].fsm == PumpFsm::LockoutSensor) {
            snprintf(out, outLen, "Tank %u sensor error", (unsigned)(i + 1));
            return;
        }
    }
    // Priority 2: dry-run lockouts.
    for (uint8_t i = 0; i < 2; ++i) {
        if (pumps[i].fsm == PumpFsm::LockoutDryRun) {
            snprintf(out, outLen, "Tank %u water low", (unsigned)(i + 1));
            return;
        }
    }
    // Priority 3: runtime trips.
    for (uint8_t i = 0; i < 2; ++i) {
        if (pumps[i].fsm == PumpFsm::LockoutRuntime) {
            snprintf(out, outLen, "Pump %u runtime limit", (unsigned)(i + 1));
            return;
        }
    }
    // Priority 4: DHT (informational; pumps not gated).
    if (dhtErr) {
        snprintf(out, outLen, "DHT sensor error");
        return;
    }
}

// Pure helpers ---------------------------------------------------------------

PumpRequestResult evaluatePumpOnRequest(const PumpSafetyInputs& in) {
    // Order matters: report the strongest, most actionable reason.
    if (in.tankErr)                          return PumpRequestResult::DeniedSensor;
    if (in.tankPct < in.minWaterPct)         return PumpRequestResult::DeniedDryRun;
    if (in.fsm == PumpFsm::LockoutRuntime)   return PumpRequestResult::DeniedRuntime;
    if (in.fsm == PumpFsm::LockoutSensor)    return PumpRequestResult::DeniedSensor;
    if (in.fsm == PumpFsm::LockoutDryRun)    return PumpRequestResult::DeniedDryRun;
    return PumpRequestResult::Granted;
}

bool evaluateAutoDemand(uint8_t soilPct,
                        bool soilErr,
                        uint8_t thresholdPct,
                        bool currentDemand) {
    // No reading → no demand (safe default).
    if (soilErr) return false;
    if (soilPct < thresholdPct) return true;
    // Hysteresis band: only release demand once we are well above
    // the threshold to avoid relay chatter.
    uint16_t releaseAt = (uint16_t)thresholdPct + SAFETY_HYSTERESIS_PCT;
    if (releaseAt > 100) releaseAt = 100;
    if (soilPct > releaseAt) return false;
    return currentDemand;  // hold inside the band
}

bool isLockoutActive(uint32_t now, uint32_t lockoutUntilMs) {
    // 0 is the "no cooldown" sentinel. Treat it explicitly so a
    // freshly-zeroed PumpState never reports "still locked" before
    // the first relaysTick() runs.
    if (lockoutUntilMs == 0) return false;
    // Signed-difference comparison: tolerates the millis() wrap
    // because (uint32_t)(deadline - now) interpreted as int32_t is
    // the relative offset to the deadline. Positive → deadline is
    // in the future (cooldown still active); zero or negative →
    // expired.
    int32_t diff = (int32_t)(lockoutUntilMs - now);
    return diff > 0;
}

uint32_t computeDeadline(uint32_t now, uint32_t offsetMs) {
    uint32_t deadline = now + offsetMs;
    // 0 is the "no active timer" sentinel; bump to 1 if the addition
    // happens to land exactly on it (only possible at the precise
    // millis() wrap point). One ms of slop is irrelevant for any
    // real deadline.
    if (deadline == 0) deadline = 1;
    return deadline;
}

#ifdef ARDUINO
static uint32_t g_lastTickMs = 0;
// Per-pump latched demand: in manual mode, set by the web request; in
// auto mode, derived from soil/threshold each tick.
static bool g_demand[2] = {false, false};

static uint8_t pumpPin(uint8_t i) {
    return (i == 0) ? PIN_RELAY_PUMP1 : PIN_RELAY_PUMP2;
}

// Edge-triggered metrics + event-log. Called from drivePump() while
// SystemState::mutex is held (relaysTick is the sole caller of
// drivePump). eventLogAdd is the pure variant — no extra locking.
// Forward-declared so drivePump can call it.
static void recordPumpEdge(SystemState& state, uint8_t i, bool on,
                           uint32_t elapsedSec) {
    if (on) {
        metricsIncSat(state.metrics.pumpStarts[i], 1);
        char msg[40];
        snprintf(msg, sizeof(msg), "Pump %u started", (unsigned)(i + 1));
        eventLogAdd(state.events, millis(), EventLevel::Info, msg);
    } else {
        metricsAddSat(state.metrics.pumpRuntimeSec[i], elapsedSec);
        char msg[48];
        snprintf(msg, sizeof(msg), "Pump %u stopped after %us",
                 (unsigned)(i + 1), (unsigned)elapsedSec);
        eventLogAdd(state.events, millis(), EventLevel::Info, msg);
    }
}

static void drivePump(SystemState& state, PumpState& p, uint8_t i,
                      bool on, uint32_t now) {
    if (p.on == on) return;
    digitalWrite(pumpPin(i), on ? RELAY_ACTIVE_LEVEL : RELAY_IDLE_LEVEL);
    p.on = on;
    uint32_t elapsedSec = 0;
    if (on) {
        p.startedAtMs = now;
    } else {
        if (p.startedAtMs != 0) {
            elapsedSec = (now - p.startedAtMs) / 1000UL;
            // Saturating add for daily total (web/UI only). The previous
            // `total > 0xFFFFFFFFu` check was dead because `total` is
            // already a uint32_t. Detect wrap-around by comparing the
            // sum against the prior value.
            uint32_t prev  = p.totalRunSecToday;
            uint32_t total = prev + elapsedSec;
            if (total < prev) total = 0xFFFFFFFFu;
            p.totalRunSecToday = total;
        }
        p.startedAtMs = 0;
    }
    recordPumpEdge(state, i, on, elapsedSec);
    // NB: runUntilMs is intentionally NOT cleared here. Hard lockouts
    // (dry-run / sensor) drive OFF but should preserve the operator's
    // requested deadline so the pump does not silently re-run for an
    // unbounded time once the lockout clears. The dedicated timer-
    // expiry path in relaysTick() clears the field; an explicit OFF
    // request via requestPump() also clears it.
}

// Called from relaysTick() while the SystemState mutex is held.
// Logs an FSM transition into a lockout state once per occurrence.
// Caller passes the previous and new FSM so we can detect edges
// without storing per-pump scratch state outside SystemState.
static void recordLockoutEdge(SystemState& state, uint8_t i,
                              PumpFsm prev, PumpFsm next) {
    if (prev == next) return;
    auto isLockout = [](PumpFsm s) {
        return s == PumpFsm::LockoutDryRun ||
               s == PumpFsm::LockoutRuntime ||
               s == PumpFsm::LockoutSensor;
    };
    if (isLockout(next) && !isLockout(prev)) {
        char msg[64];
        const char* reason = pumpFsmText(next);
        snprintf(msg, sizeof(msg), "Pump %u entered %s",
                 (unsigned)(i + 1), reason);
        eventLogAdd(state.events, millis(), EventLevel::Warn, msg);
    } else if (!isLockout(next) && isLockout(prev)) {
        char msg[64];
        const char* prior = pumpFsmText(prev);
        snprintf(msg, sizeof(msg), "Pump %u cleared %s",
                 (unsigned)(i + 1), prior);
        eventLogAdd(state.events, millis(), EventLevel::Info, msg);
    }
}
#endif

void relaysBegin(SystemState& state) {
#ifdef ARDUINO
    digitalWrite(PIN_RELAY_PUMP1, RELAY_IDLE_LEVEL);
    digitalWrite(PIN_RELAY_PUMP2, RELAY_IDLE_LEVEL);
    pinMode(PIN_RELAY_PUMP1, OUTPUT);
    pinMode(PIN_RELAY_PUMP2, OUTPUT);
    digitalWrite(PIN_RELAY_PUMP1, RELAY_IDLE_LEVEL);
    digitalWrite(PIN_RELAY_PUMP2, RELAY_IDLE_LEVEL);
    g_lastTickMs = 0;
    g_demand[0] = g_demand[1] = false;
#endif
    for (auto& p : state.pumps) {
        p.fsm = PumpFsm::Off;
        p.on = false;
        p.startedAtMs = 0;
        p.lockoutUntilMs = 0;
        p.totalRunSecToday = 0;
        p.runUntilMs = 0;
    }
}

void relaysTick(SystemState& state) {
#ifdef ARDUINO
    uint32_t now = millis();
    if (g_lastTickMs != 0 && (now - g_lastTickMs) < RELAYS_PERIOD_MS) {
        return;
    }
    g_lastTickMs = now;

    if (!state.mutex || xSemaphoreTake(state.mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        return;
    }
    const SensorReading s = state.sensors;
    const Settings cfg = state.settings;

    for (uint8_t i = 0; i < 2; ++i) {
        PumpState& p = state.pumps[i];
        PumpFsm    fsmEntry = p.fsm;  // snapshot for lockout-edge log
        uint8_t  tankPct = (i == 0) ? s.tank1Pct : s.tank2Pct;
        bool     tankErr = (i == 0) ? s.tank1Err : s.tank2Err;

        // 1) Hard lockouts (override demand, force OFF).
        bool dry = !tankErr && (tankPct < cfg.minWaterPct);
        if (tankErr) {
            drivePump(state, p, i, false, now);
            p.fsm = PumpFsm::LockoutSensor;
        } else if (dry) {
            drivePump(state, p, i, false, now);
            p.fsm = PumpFsm::LockoutDryRun;
        } else if (p.fsm == PumpFsm::LockoutRuntime &&
                   isLockoutActive(now, p.lockoutUntilMs)) {
            // Runtime cooldown: rollover-safe via signed-diff helper.
            // A naive `now < p.lockoutUntilMs` would invert across the
            // ~49.7-day millis() wrap and either release the pump
            // early or hold it forever.
            drivePump(state, p, i, false, now);
        } else {
            // 2) Clear non-active lockouts when conditions allow.
            if (p.fsm == PumpFsm::LockoutDryRun &&
                tankPct >= cfg.minWaterPct + SAFETY_HYSTERESIS_PCT) {
                p.fsm = PumpFsm::Off;
            }
            if (p.fsm == PumpFsm::LockoutSensor && !tankErr) {
                p.fsm = PumpFsm::Off;
            }
            if (p.fsm == PumpFsm::LockoutRuntime &&
                !isLockoutActive(now, p.lockoutUntilMs)) {
                p.fsm = PumpFsm::Off;
                p.lockoutUntilMs = 0;
            }
        }

        // 3) Auto-mode demand evaluation.
        if (cfg.autoMode) {
            g_demand[i] = evaluateAutoDemand(s.soilPct, s.soilErr,
                                             cfg.soilThresholdPct,
                                             g_demand[i]);
        }

        // 4) Apply demand if FSM is in a runnable state.
        bool runnable = (p.fsm == PumpFsm::Off || p.fsm == PumpFsm::On);
        if (runnable) {
            // Manual run-duration timer: when set, expiring it must
            // drive OFF and clear demand so the pump does not
            // immediately restart on the next tick. Honoured BEFORE
            // we re-evaluate demand so a stale `g_demand[i]=true`
            // from a manual ON cannot win over an expired timer.
            bool timerExpired = (p.runUntilMs != 0) &&
                                !isLockoutActive(now, p.runUntilMs);
            if (timerExpired) {
                if (p.on) drivePump(state, p, i, false, now);
                p.fsm = PumpFsm::Off;
                p.runUntilMs = 0;
                g_demand[i] = false;
            } else if (g_demand[i] && !p.on) {
                drivePump(state, p, i, true, now);
                p.fsm = PumpFsm::On;
            } else if (!g_demand[i] && p.on) {
                drivePump(state, p, i, false, now);
                p.fsm = PumpFsm::Off;
                p.runUntilMs = 0;  // explicit demand-drop clears the timer
            }
        }

        // 5) Runtime trip.
        if (p.fsm == PumpFsm::On && p.startedAtMs != 0) {
            uint32_t runMs = now - p.startedAtMs;
            if (runMs >= (uint32_t)cfg.maxPumpSeconds * 1000UL) {
                drivePump(state, p, i, false, now);
                p.fsm = PumpFsm::LockoutRuntime;
                // Also clear the manual timer: a runtime-trip is a
                // safety event that supersedes the operator's
                // requested duration; the cooldown enforces a rest
                // period before any new ON request is accepted.
                p.runUntilMs = 0;
                p.lockoutUntilMs = computeDeadline(now, LOCKOUT_RUNTIME_COOLDOWN_MS);
                g_demand[i] = false;  // require re-arm
            }
        }

        // Lockout entry/exit transitions are surfaced to the event
        // log so the dashboard tail can show *why* a pump went off.
        // Computed from the entry-snapshot vs the post-tick FSM so
        // a single tick that both enters and exits a lockout (which
        // shouldn't happen with hysteresis but might under sensor
        // chatter) still produces a single intelligible audit line.
        recordLockoutEdge(state, i, fsmEntry, p.fsm);
    }

    // Snapshot pump FSM states under the mutex so the error-compose
    // step below stays consistent and lock-free.
    PumpState pumpsSnap[2] = { state.pumps[0], state.pumps[1] };
    xSemaphoreGive(state.mutex);

    // Refresh operator-facing error message based on FSM outcome.
    // Re-takes the mutex briefly to write `lastError`. `pumpsSnap`
    // and `s` are local snapshots so this is race-free.
    char errBuf[sizeof(state.lastError)];
    composePumpsErrorMessage(pumpsSnap,
                             s.tank1Err, s.tank2Err, s.dhtErr,
                             errBuf, sizeof(errBuf));
    if (xSemaphoreTake(state.mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        if (strncmp(errBuf, state.lastError, sizeof(state.lastError)) != 0) {
            strncpy(state.lastError, errBuf, sizeof(state.lastError) - 1);
            state.lastError[sizeof(state.lastError) - 1] = '\0';
        }
        xSemaphoreGive(state.mutex);
    }
#else
    (void)state;
#endif
}

PumpRequestResult requestPump(SystemState& state,
                              uint8_t pumpIdx,
                              bool desired,
                              PumpRequestSource source,
                              uint32_t durationSec) {
    if (pumpIdx >= 2) return PumpRequestResult::DeniedInvalid;

#ifdef ARDUINO
    // Auto requests are absorbed by the periodic FSM, not this path.
    (void)source;

    if (!state.mutex || xSemaphoreTake(state.mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        // Transient lock contention; surface as Busy (retryable)
        // instead of the misleading "invalid_request" the operator
        // would otherwise see in the API response.
        return PumpRequestResult::DeniedBusy;
    }
    PumpState& p = state.pumps[pumpIdx];
    const SensorReading s = state.sensors;
    const Settings cfg = state.settings;
    PumpRequestResult result = PumpRequestResult::Granted;

    if (desired) {
        PumpSafetyInputs in;
        in.tankPct      = (pumpIdx == 0) ? s.tank1Pct : s.tank2Pct;
        in.tankErr      = (pumpIdx == 0) ? s.tank1Err : s.tank2Err;
        in.fsm          = p.fsm;
        in.minWaterPct  = cfg.minWaterPct;
        result = evaluatePumpOnRequest(in);
        if (result == PumpRequestResult::Granted) {
            g_demand[pumpIdx] = true;
            // Set the auto-off deadline. A duration of 0 means
            // "unlimited"; runtime-trip lockout still bounds the
            // session at maxPumpSeconds. Cap at MAX_MANUAL_PUMP_SECONDS
            // for defence in depth — the HTTP layer should already
            // have validated, but FSM is the safety boundary.
            if (durationSec > 0) {
                if (durationSec > MAX_MANUAL_PUMP_SECONDS) {
                    durationSec = MAX_MANUAL_PUMP_SECONDS;
                }
                p.runUntilMs = computeDeadline(millis(), durationSec * 1000UL);
            } else {
                p.runUntilMs = 0;
            }
        }
    } else {
        g_demand[pumpIdx] = false;  // OFF is always allowed.
        // Explicit OFF clears any pending manual timer so a
        // subsequent unlimited ON does not inherit a stale deadline.
        p.runUntilMs = 0;
    }
    xSemaphoreGive(state.mutex);
    return result;
#else
    (void)state; (void)desired; (void)source; (void)durationSec;
    return PumpRequestResult::Granted;
#endif
}
