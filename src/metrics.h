// metrics.h — Lightweight runtime counters for `/api/metrics`.
//
// Counters are monotonic and saturate at UINT32_MAX rather than
// wrap; clients can compute deltas without false negatives across
// the wrap boundary. The struct is embedded in `SystemState` and
// guarded by `SystemState::mutex` — the helpers here are pure.

#pragma once

#include <stdint.h>

struct Metrics {
    // Boot timestamp (millis() at the first metrics tick). Used to
    // compute uptime without touching `Arduino.h` from the pure
    // helpers; main.cpp populates this from millis() at startup.
    uint32_t bootMs;

    // Loop-task progress: incremented every main-loop iteration so
    // the dashboard can detect a stalled main task even before the
    // task watchdog fires.
    uint32_t mainLoopTicks;

    // Sensor failures observed (rising-edge counted in sensors.cpp).
    uint32_t sensorErrorCount;

    // Per-pump start counter (one increment per Off→On transition).
    uint32_t pumpStarts[2];
    // Per-pump cumulative runtime (seconds since boot).
    uint32_t pumpRuntimeSec[2];

    // AP-restart counter (every successful soft-AP restart, whether
    // settings-change or watchdog-driven).
    uint32_t apRestarts;

    // HTTP traffic — coarse bands for an at-a-glance health view.
    uint32_t httpRequests;
    uint32_t httpErrors4xx;
    uint32_t httpErrors5xx;

    // Heap watermark observed since boot.
    uint32_t freeHeapMin;
};

void metricsInit(Metrics& m);

// Saturating increment. Returns the post-increment value. Safe to
// call from any context; the caller owns synchronisation.
uint32_t metricsIncSat(uint32_t& counter, uint32_t by);

// Saturating add (common case for cumulative seconds / bytes).
uint32_t metricsAddSat(uint32_t& counter, uint32_t by);
