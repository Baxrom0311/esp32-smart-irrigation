// state.h — Shared system state shape used across modules.
//
// SystemState is the single source of truth for the latest sensor
// readings, pump states, and persisted settings. It is owned by
// main.cpp and passed by reference to module tick functions.
//
// AsyncWebServer callbacks run on the TCP task; state mutated from
// HTTP handlers must be protected via the mutex declared here.

#pragma once

#include <stdint.h>

#ifdef ARDUINO
  #include <Arduino.h>
  #include <IPAddress.h>
  #include <freertos/FreeRTOS.h>
  #include <freertos/semphr.h>
#endif

#include "config.h"
#include "eventlog.h"
#include "metrics.h"

// ---------------------------------------------------------------------------
// Persisted user settings
// ---------------------------------------------------------------------------
struct Settings {
    uint8_t  soilThresholdPct;   // auto-mode trigger (0..100)
    uint8_t  minWaterPct;        // dry-run protection floor (0..100)
    uint16_t maxPumpSeconds;     // max continuous pump runtime
    char     ssid[MAX_SSID_LEN + 1];
    char     pass[MAX_PASS_LEN + 2];   // +1 NUL, +1 slack
    bool     autoMode;
    // Per-sensor raw ADC calibration endpoints.
    uint16_t tank1RawDry;
    uint16_t tank1RawWet;
    uint16_t tank2RawDry;
    uint16_t tank2RawWet;
    uint16_t soilRawDry;
    uint16_t soilRawWet;
    uint16_t soil2RawDry;
    uint16_t soil2RawWet;
    // WiFi STA (internet uchun)
    char     staSSID[MAX_STA_SSID_LEN + 1];
    char     staPass[MAX_STA_PASS_LEN + 2];
    // Telegram bot
    bool     tgEnabled;
    char     tgToken[MAX_TG_TOKEN_LEN + 1];
    uint8_t  tgChatCount;
    char     tgChatIds[MAX_TG_CHAT_IDS][MAX_TG_CHAT_ID_LEN + 1];
};

// ---------------------------------------------------------------------------
// Latest sensor readings
// ---------------------------------------------------------------------------
struct SensorReading {
    uint8_t  tank1Pct;
    uint8_t  tank2Pct;
    uint8_t  soil1Pct;     // Agarot 1
    uint8_t  soil2Pct;     // Agarot 2
    float    tempC;
    float    humPct;
    bool     tank1Err;
    bool     tank2Err;
    bool     soil1Err;
    bool     soil2Err;
    bool     dhtErr;
    uint32_t lastUpdateMs;
};

// ---------------------------------------------------------------------------
// Pump state (one per pump). Owned exclusively by relays.cpp.
// ---------------------------------------------------------------------------
enum class PumpFsm : uint8_t {
    Off = 0,
    On,
    LockoutDryRun,
    LockoutRuntime,
    LockoutSensor,
};

struct PumpState {
    PumpFsm  fsm;
    bool     on;                   // physical relay state
    uint32_t startedAtMs;          // 0 if off
    uint32_t lockoutUntilMs;       // 0 if no cooldown
    uint32_t totalRunSecToday;
    // Manual-duration auto-off deadline (millis). 0 = no timer
    // (run until OFF, an external lockout, or the runtime trip).
    // Set by `requestPump(..., durationSec>0, ...)`. Honoured by
    // relaysTick(), which drives the relay OFF and clears the
    // field when the deadline is reached. Rollover-safe via
    // isLockoutActive() — never compare directly with `now`.
    uint32_t runUntilMs;
};

// ---------------------------------------------------------------------------
// Aggregate
// ---------------------------------------------------------------------------
struct SystemState {
    SensorReading sensors;
    PumpState     pumps[2];
    Settings      settings;
#ifdef ARDUINO
    IPAddress     apIp;
    SemaphoreHandle_t mutex;       // guards cross-task access
#endif
    char          lastError[64];   // most recent error string for UI/API
    EventLog      events;          // ring-buffer tail of recent events
    Metrics       metrics;         // saturating runtime counters
    // Last tick at which the AP was confirmed healthy (non-zero IP).
    // 0 before the first health check completes. Owned by the AP
    // health watchdog in webserver.cpp; readable for /api/metrics.
    uint32_t      apHealthLastOkMs;
};

// Result of a pump-state-change request (web → safety FSM).
//
// `DeniedBusy` is reserved for transient state contention (mutex
// could not be acquired in time). Distinguishing it from
// `DeniedInvalid` lets the HTTP layer return a retryable 503
// rather than the misleading 409 "invalid_request" the operator
// would otherwise see during a brief lock collision.
enum class PumpRequestResult : uint8_t {
    Granted = 0,
    DeniedDryRun,
    DeniedSensor,
    DeniedRuntime,
    DeniedInvalid,
    DeniedBusy,
};

enum class PumpRequestSource : uint8_t {
    Web = 0,
    Auto,
    Boot,
};
