// sensors.cpp — Water-level, soil-moisture and DHT22 reads.
//
// Cadence: SENSORS_PERIOD_MS (2000 ms). All analog pins are ADC1
// channels and are sampled with ADC_OVERSAMPLE-fold averaging.
//
// Error model:
//   - "stuck" raw values (≤ ADC_STUCK_LOW or ≥ ADC_STUCK_HIGH) for
//     ADC_STUCK_STREAK consecutive reads → set the corresponding
//     *Err flag. The relay FSM treats *Err as a hard lockout for
//     any pump that depends on that sensor.
//   - DHT22 NaN read → dhtErr = true. Pumps continue (DHT does not
//     gate them) but the dashboard banner reflects it.

#include "sensors.h"

#include "config.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>

#ifdef ARDUINO
  #include <Arduino.h>
  #include <DHT.h>

  static DHT g_dht(PIN_DHT, DHT_TYPE);
  static uint32_t g_lastReadMs = 0;
  static uint8_t  g_tank1Streak = 0;
  static uint8_t  g_tank2Streak = 0;
  static uint8_t  g_soil1Streak = 0;
  static uint8_t  g_soil2Streak = 0;
  static bool     g_prevTank1Err = false;
  static bool     g_prevTank2Err = false;
  static bool     g_prevSoil1Err = false;
  static bool     g_prevSoil2Err = false;
  static bool     g_prevDhtErr   = false;
#endif

// ---------------------------------------------------------------------------
// Pure conversion (endpoint-aware).
//
// Linear interpolation between (rawDry → 0 %) and (rawWet → 100 %).
// Works in either direction: the helper does not require Dry < Wet.
//
// Strategy:
//   - Treat both endpoints as int32 to side-step uint16 underflow
//     when computing (raw - rawDry) for an inverted sensor.
//   - Clamp the input to the [min(Dry,Wet), max(Dry,Wet)] band
//     before interpolation; out-of-band readings saturate at 0 or
//     100 % and never produce negative percentages.
//   - On a degenerate span (< MIN_CAL_SPAN) return 0; the patch
//     validator should have rejected this configuration but the
//     runtime path stays defensive in case NVS gets corrupted.
// ---------------------------------------------------------------------------
uint8_t rawToPct(uint16_t avgRaw, uint16_t rawDry, uint16_t rawWet) {
    int32_t raw = (int32_t)avgRaw;
    int32_t dry = (int32_t)rawDry;
    int32_t wet = (int32_t)rawWet;

    int32_t lo = dry < wet ? dry : wet;
    int32_t hi = dry < wet ? wet : dry;
    int32_t span = hi - lo;
    if (span < MIN_CAL_SPAN) {
        return 0;  // safe default; FSM treats 0% as dry-run.
    }
    if (raw < lo) raw = lo;
    if (raw > hi) raw = hi;

    // Distance from the dry endpoint, scaled to [0, span].
    int32_t fromDry = (dry < wet) ? (raw - dry) : (dry - raw);
    if (fromDry < 0)    fromDry = 0;
    if (fromDry > span) fromDry = span;

    int32_t pct = (fromDry * 100) / span;
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    return (uint8_t)pct;
}

uint8_t rawWaterToPct(uint16_t avgRaw) {
    return rawToPct(avgRaw, DEFAULT_TANK_RAW_DRY, DEFAULT_TANK_RAW_WET);
}

uint8_t rawSoilToPct(uint16_t avgRaw) {
    return rawToPct(avgRaw, DEFAULT_SOIL_RAW_DRY, DEFAULT_SOIL_RAW_WET);
}

// Update a "consecutive stuck" streak counter; return true when it
// has exceeded ADC_STUCK_STREAK (i.e. the sensor is considered bad).
// Pure — no Arduino dependency — so it can be unit-tested.
bool updateStuckStreak(uint16_t raw, uint8_t& streak) {
    bool stuck = (raw <= ADC_STUCK_LOW) || (raw >= ADC_STUCK_HIGH);
    if (stuck) {
        if (streak < 255) streak++;
    } else {
        streak = 0;
    }
    return streak >= ADC_STUCK_STREAK;
}

// ---------------------------------------------------------------------------
// applyCalibrationPatch — validate-then-apply, pure.
//
// Two-phase contract: validation must complete before any write to
// `out`. The /api/calibrate handler relies on this to short-circuit
// without leaving a half-mutated Settings struct in RAM.
// ---------------------------------------------------------------------------
static bool spanOk(uint16_t a, uint16_t b) {
    int32_t d = (int32_t)a - (int32_t)b;
    if (d < 0) d = -d;
    return d >= MIN_CAL_SPAN;
}

CalibrationPatchResult applyCalibrationPatch(const CalibrationPatch& patch,
                                             Settings& out) {
    // ---- Phase 1a: range check on every supplied raw value. ----
    auto rangeOk = [](uint16_t v) { return v <= ADC_MAX_RAW; };
    if (patch.hasTank1Dry && !rangeOk(patch.tank1Dry)) return CalibrationPatchResult::OutOfRange;
    if (patch.hasTank1Wet && !rangeOk(patch.tank1Wet)) return CalibrationPatchResult::OutOfRange;
    if (patch.hasTank2Dry && !rangeOk(patch.tank2Dry)) return CalibrationPatchResult::OutOfRange;
    if (patch.hasTank2Wet && !rangeOk(patch.tank2Wet)) return CalibrationPatchResult::OutOfRange;
    if (patch.hasSoilDry  && !rangeOk(patch.soilDry))  return CalibrationPatchResult::OutOfRange;
    if (patch.hasSoilWet  && !rangeOk(patch.soilWet))  return CalibrationPatchResult::OutOfRange;
    if (patch.hasSoil2Dry && !rangeOk(patch.soil2Dry)) return CalibrationPatchResult::OutOfRange;
    if (patch.hasSoil2Wet && !rangeOk(patch.soil2Wet)) return CalibrationPatchResult::OutOfRange;

    // ---- Phase 1b: post-merge span check per sensor. ----
    uint16_t t1d = patch.hasTank1Dry ? patch.tank1Dry : out.tank1RawDry;
    uint16_t t1w = patch.hasTank1Wet ? patch.tank1Wet : out.tank1RawWet;
    uint16_t t2d = patch.hasTank2Dry ? patch.tank2Dry : out.tank2RawDry;
    uint16_t t2w = patch.hasTank2Wet ? patch.tank2Wet : out.tank2RawWet;
    uint16_t sd  = patch.hasSoilDry  ? patch.soilDry  : out.soilRawDry;
    uint16_t sw  = patch.hasSoilWet  ? patch.soilWet  : out.soilRawWet;
    uint16_t s2d = patch.hasSoil2Dry ? patch.soil2Dry : out.soil2RawDry;
    uint16_t s2w = patch.hasSoil2Wet ? patch.soil2Wet : out.soil2RawWet;

    bool t1Touch = patch.hasTank1Dry || patch.hasTank1Wet;
    bool t2Touch = patch.hasTank2Dry || patch.hasTank2Wet;
    bool sTouch  = patch.hasSoilDry  || patch.hasSoilWet;
    bool s2Touch = patch.hasSoil2Dry || patch.hasSoil2Wet;

    if (t1Touch && !spanOk(t1d, t1w)) return CalibrationPatchResult::SpanTooSmall;
    if (t2Touch && !spanOk(t2d, t2w)) return CalibrationPatchResult::SpanTooSmall;
    if (sTouch  && !spanOk(sd,  sw))  return CalibrationPatchResult::SpanTooSmall;
    if (s2Touch && !spanOk(s2d, s2w)) return CalibrationPatchResult::SpanTooSmall;

    // ---- Phase 2: apply. ----
    if (patch.hasTank1Dry) out.tank1RawDry = patch.tank1Dry;
    if (patch.hasTank1Wet) out.tank1RawWet = patch.tank1Wet;
    if (patch.hasTank2Dry) out.tank2RawDry = patch.tank2Dry;
    if (patch.hasTank2Wet) out.tank2RawWet = patch.tank2Wet;
    if (patch.hasSoilDry)  out.soilRawDry  = patch.soilDry;
    if (patch.hasSoilWet)  out.soilRawWet  = patch.soilWet;
    if (patch.hasSoil2Dry) out.soil2RawDry = patch.soil2Dry;
    if (patch.hasSoil2Wet) out.soil2RawWet = patch.soil2Wet;
    return CalibrationPatchResult::Ok;
}

const char* calibrationPatchErrorText(CalibrationPatchResult r) {
    switch (r) {
        case CalibrationPatchResult::Ok:           return "ok";
        case CalibrationPatchResult::OutOfRange:   return "raw_out_of_range";
        case CalibrationPatchResult::SpanTooSmall: return "span_too_small";
    }
    return "unknown";
}

#ifdef ARDUINO
static uint16_t readAdcAveraged(uint8_t pin) {
    uint32_t sum = 0;
    for (uint8_t i = 0; i < ADC_OVERSAMPLE; ++i) {
        sum += analogRead(pin);
        delayMicroseconds(200);
    }
    return static_cast<uint16_t>(sum / ADC_OVERSAMPLE);
}
#endif

void sensorsBegin(SystemState& state) {
#ifdef ARDUINO
    analogReadResolution(ADC_RESOLUTION_BITS);
    // analogSetAttenuation defaults to 11 dB which gives ~0..3.3 V FS.
    analogSetAttenuation(ADC_11db);
    pinMode(PIN_WATER_TANK1, INPUT);
    pinMode(PIN_WATER_TANK2, INPUT);
    pinMode(PIN_SOIL_MOISTURE1, INPUT);
    pinMode(PIN_SOIL_MOISTURE2, INPUT);

    g_dht.begin();
    g_lastReadMs = 0;
#endif

    // Initialise readings to "unknown but not errored" so the UI
    // doesn't flash red banners during the first 2 s of boot.
    state.sensors = {};
    state.sensors.tempC = NAN;
    state.sensors.humPct = NAN;
}

void sensorsTick(SystemState& state) {
#ifdef ARDUINO
    uint32_t now = millis();
    if (g_lastReadMs != 0 && (now - g_lastReadMs) < SENSORS_PERIOD_MS) {
        return;
    }
    g_lastReadMs = now;

    uint16_t rawT1 = readAdcAveraged(PIN_WATER_TANK1);
    uint16_t rawT2 = readAdcAveraged(PIN_WATER_TANK2);
    uint16_t rawS1 = readAdcAveraged(PIN_SOIL_MOISTURE1);
    uint16_t rawS2 = readAdcAveraged(PIN_SOIL_MOISTURE2);

    bool t1Err = false;  // tank sensors: 0% when disconnected, no error
    bool t2Err = false;
    bool s1Err = updateStuckStreak(rawS1, g_soil1Streak);
    bool s2Err = updateStuckStreak(rawS2, g_soil2Streak);

    float t = g_dht.readTemperature();
    float h = g_dht.readHumidity();
    bool dhtErr = isnan(t) || isnan(h) || t < -40.0f || t > 80.0f
                                       || h <   0.0f || h > 100.0f;

    if (state.mutex && xSemaphoreTake(state.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        const Settings cfg = state.settings;
        state.sensors.tank1Pct = t1Err ? 0
            : rawToPct(rawT1, cfg.tank1RawDry, cfg.tank1RawWet);
        state.sensors.tank2Pct = t2Err ? 0
            : rawToPct(rawT2, cfg.tank2RawDry, cfg.tank2RawWet);
        state.sensors.soil1Pct = s1Err ? 0
            : rawToPct(rawS1, cfg.soilRawDry,  cfg.soilRawWet);
        state.sensors.soil2Pct = s2Err ? 0
            : rawToPct(rawS2, cfg.soil2RawDry, cfg.soil2RawWet);
        state.sensors.tempC    = dhtErr ? NAN : t;
        state.sensors.humPct   = dhtErr ? NAN : h;
        state.sensors.tank1Err = t1Err;
        state.sensors.tank2Err = t2Err;
        state.sensors.soil1Err = s1Err;
        state.sensors.soil2Err = s2Err;
        state.sensors.dhtErr   = dhtErr;
        state.sensors.lastUpdateMs = now;

        struct EdgeSpec {
            bool         err;
            bool&        prev;
            const char*  name;
        } edges[] = {
            { t1Err,  g_prevTank1Err, "tank 1" },
            { t2Err,  g_prevTank2Err, "tank 2" },
            { s1Err,  g_prevSoil1Err, "soil 1" },
            { s2Err,  g_prevSoil2Err, "soil 2" },
            { dhtErr, g_prevDhtErr,   "dht"    },
        };
        for (auto& e : edges) {
            if (e.err && !e.prev) {
                metricsIncSat(state.metrics.sensorErrorCount, 1);
                char msg[40];
                snprintf(msg, sizeof(msg), "%s sensor error", e.name);
                eventLogAdd(state.events, now, EventLevel::Warn, msg);
            } else if (!e.err && e.prev) {
                char msg[40];
                snprintf(msg, sizeof(msg), "%s sensor recovered", e.name);
                eventLogAdd(state.events, now, EventLevel::Info, msg);
            }
            e.prev = e.err;
        }

        xSemaphoreGive(state.mutex);
    }
#else
    (void)state;
#endif
}
