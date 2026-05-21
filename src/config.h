// config.h — Pin assignments, defaults, and tuning constants.
//
// Single source of truth for hardware pinout and runtime defaults.
// Do not introduce magic numbers in module sources; add a constant
// here and reference it.
//
// All sensor analog pins are on ADC1 (GPIO32/33/34/35) so they
// coexist with WiFi (ADC2 cannot be read while WiFi is active).

#pragma once

#include <stdint.h>

// ---------------------------------------------------------------------------
// Hardware pin map (ESP32 DevKit V1)
// ---------------------------------------------------------------------------

// Water level sensors (MH analog, ADC1)
#define PIN_WATER_TANK1     34
#define PIN_WATER_TANK2     35

// Soil moisture sensors (Capacitive v1.2, analog, ADC1) — 2 ta agarot
#define PIN_SOIL_MOISTURE1  32
#define PIN_SOIL_MOISTURE2  33

// DHT11 — temperature + humidity
#define PIN_DHT             14
#define DHT_TYPE            11  // DHT11

// Pump relays (active LOW). Driven HIGH = OFF on boot.
#define PIN_RELAY_PUMP1     26
#define PIN_RELAY_PUMP2     27

// ---------------------------------------------------------------------------
// Relay polarity
// ---------------------------------------------------------------------------
#define RELAY_ACTIVE_LEVEL  LOW
#define RELAY_IDLE_LEVEL    HIGH

// ---------------------------------------------------------------------------
// ADC
// ---------------------------------------------------------------------------
#define ADC_RESOLUTION_BITS 12
#define ADC_MAX_RAW         4095
#define ADC_OVERSAMPLE      8     // averaging count per analog read
#define ADC_STUCK_LOW       50    // raw <= this for N reads = error
#define ADC_STUCK_HIGH      4045  // raw >= this for N reads = error
#define ADC_STUCK_STREAK    3     // consecutive bad reads to trip error

// ---------------------------------------------------------------------------
// Cadence (milliseconds)
// ---------------------------------------------------------------------------
#define SENSORS_PERIOD_MS   2000  // sensor read cadence (brief: 2 s)
#define RELAYS_PERIOD_MS     100  // safety FSM tick
#define WDT_TIMEOUT_S         30  // task watchdog

// ---------------------------------------------------------------------------
// Settings defaults (used on first boot, before NVS has values)
// ---------------------------------------------------------------------------
#define DEFAULT_SOIL_THRESHOLD_PCT   40
#define DEFAULT_MIN_WATER_PCT        20
#define DEFAULT_MAX_PUMP_SECONDS   1800   // 30 minutes
#define DEFAULT_AP_SSID            "SmartIrrigation"
#define DEFAULT_AP_PASSWORD        "12345678"
#define DEFAULT_AUTO_MODE          true

// Settings clamp bounds
#define MIN_THRESHOLD_PCT    0
#define MAX_THRESHOLD_PCT  100
#define MIN_PUMP_SECONDS    60
#define MAX_PUMP_SECONDS  3600
#define MIN_SSID_LEN         1
#define MAX_SSID_LEN        32
#define MIN_PASS_LEN         8   // WPA2 minimum (or 0 for open AP)
#define MAX_PASS_LEN        63

// ---------------------------------------------------------------------------
// Safety / control
// ---------------------------------------------------------------------------
#define SAFETY_HYSTERESIS_PCT          5    // soil & water rearm hysteresis
#define LOCKOUT_RUNTIME_COOLDOWN_MS  (5UL * 60UL * 1000UL)  // 5 min

// ---------------------------------------------------------------------------
// Storage (NVS / Preferences)
// ---------------------------------------------------------------------------
#define NVS_NAMESPACE        "irrig"
#define NVS_KEY_THRESHOLD    "thr"
#define NVS_KEY_MIN_WATER    "minw"
#define NVS_KEY_MAX_SECONDS  "maxs"
#define NVS_KEY_SSID         "ssid"
#define NVS_KEY_PASS         "pass"
#define NVS_KEY_AUTO         "auto"
#define NVS_KEY_VERSION      "ver"
#define NVS_KEY_T1_DRY       "t1d"
#define NVS_KEY_T1_WET       "t1w"
#define NVS_KEY_T2_DRY       "t2d"
#define NVS_KEY_T2_WET       "t2w"
#define NVS_KEY_S_DRY        "sd"
#define NVS_KEY_S_WET        "sw"
#define NVS_KEY_S2_DRY       "s2d"
#define NVS_KEY_S2_WET       "s2w"
#define NVS_SCHEMA_VERSION   3

// ---------------------------------------------------------------------------
// Calibration defaults & sanity
// ---------------------------------------------------------------------------
// MH Water level: 0V = empty (raw 0), 3.3V = full (raw 4095).
// Capacitive Soil v1.2: ~3.3V = dry (raw 4095), ~1.5V = wet (raw ~1860).
#define DEFAULT_TANK_RAW_DRY    0
#define DEFAULT_TANK_RAW_WET    ADC_MAX_RAW
#define DEFAULT_SOIL_RAW_DRY    ADC_MAX_RAW
#define DEFAULT_SOIL_RAW_WET    0

#define MIN_CAL_SPAN           200

// ---------------------------------------------------------------------------
// HTTP server
// ---------------------------------------------------------------------------
#define HTTP_PORT            80
#define HTTP_MAX_BODY_BYTES  1024

// ---------------------------------------------------------------------------
// Manual pump duration
// ---------------------------------------------------------------------------
#define MAX_MANUAL_PUMP_SECONDS  3600
#define MIN_MANUAL_PUMP_SECONDS     1

// ---------------------------------------------------------------------------
// Heap monitoring
// ---------------------------------------------------------------------------
#define HEAP_LOG_PERIOD_MS         (60UL * 1000UL)
#define HEAP_LOW_THRESHOLD_BYTES   (20U * 1024U)

// ---------------------------------------------------------------------------
// AP health watchdog
// ---------------------------------------------------------------------------
#define AP_HEALTH_CHECK_PERIOD_MS   5000UL
#define AP_HEALTH_FAIL_TIMEOUT_MS  30000UL

// ---------------------------------------------------------------------------
// WiFi STA (internet uchun — Telegram bot)
// ---------------------------------------------------------------------------
#define DEFAULT_STA_SSID       ""
#define DEFAULT_STA_PASS       ""
#define MAX_STA_SSID_LEN       32
#define MAX_STA_PASS_LEN       63
#define STA_CONNECT_TIMEOUT_MS 15000UL
#define STA_RECONNECT_INTERVAL_MS 30000UL
#define NVS_KEY_STA_SSID       "stassid"
#define NVS_KEY_STA_PASS       "stapass"

// ---------------------------------------------------------------------------
// Telegram bot
// ---------------------------------------------------------------------------
#define DEFAULT_TG_TOKEN       "8828743889:AAGoeF42Q9SWmMngbjxMyvtmuSFPuyZXEel"
#define DEFAULT_TG_ENABLED     true
#define DEFAULT_TG_CHAT_COUNT  3
#define DEFAULT_TG_CHAT_ID_1   "549517499"
#define DEFAULT_TG_CHAT_ID_2   "1331915608"
#define DEFAULT_TG_CHAT_ID_3   "5146421024"

#define MAX_TG_TOKEN_LEN       64
#define MAX_TG_CHAT_IDS        5
#define MAX_TG_CHAT_ID_LEN     16
#define TG_SEND_INTERVAL_MS    2000UL
#define TG_QUEUE_SIZE          8
#define NVS_KEY_TG_TOKEN       "tgtkn"
#define NVS_KEY_TG_CHAT_CNT    "tgcnt"
#define NVS_KEY_TG_CHAT_PFX    "tgc"
#define NVS_KEY_TG_ENABLED     "tgen"
