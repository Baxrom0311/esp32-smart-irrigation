// sensors.h — Sensor module public interface.
//
// Reads two water-level sensors, one soil-moisture sensor, and one
// DHT22 every SENSORS_PERIOD_MS, populates SystemState.sensors.

#pragma once

#include "state.h"

void sensorsBegin(SystemState& state);
void sensorsTick(SystemState& state);

// ---------------------------------------------------------------------------
// Pure conversion helpers — exposed so native unit tests can verify
// calibration without pulling in Arduino.h.
// ---------------------------------------------------------------------------

// Endpoint-aware linear conversion from a raw ADC reading to 0..100%.
//
// `rawDry` is the raw value that maps to 0%; `rawWet` maps to 100%.
// Either argument may be the larger value (water-level sensors count
// UP with level, soil sensors count DOWN with moisture), so callers
// don't have to swap them — the helper handles both orientations.
//
// If |rawDry - rawWet| < MIN_CAL_SPAN the calibration is treated as
// degenerate; the helper returns 0 to fail safe (operator-visible
// in the dashboard, conservative for pump gating). The /api/calibrate
// validator should already have rejected such patches; this is the
// final defence.
uint8_t rawToPct(uint16_t avgRaw, uint16_t rawDry, uint16_t rawWet);

// Backwards-compatible wrappers that use the project's nominal
// endpoints (the wiring described in PROJECT_BRIEF.md). Native
// tests written before calibration was settable continue to use
// these — runtime sensor reads now go through the endpoint-aware
// version with values from Settings.
uint8_t rawWaterToPct(uint16_t avgRaw);
uint8_t rawSoilToPct(uint16_t avgRaw);

// ADC stuck-value detector.
//
// Updates `streak` in place: incremented (saturating at 255) when the
// raw reading is at the low or high rail (≤ ADC_STUCK_LOW or
// ≥ ADC_STUCK_HIGH), zeroed otherwise. Returns true once `streak`
// has reached ADC_STUCK_STREAK consecutive bad reads — the caller
// then treats the sensor as faulted.
//
// Keeping this pure (no Arduino dependency) lets the native test env
// verify the latch behaviour: for safety-critical sensors a stuck
// reading must trip the FSM, but a single noise spike must NOT.
bool updateStuckStreak(uint16_t raw, uint8_t& streak);

// ---------------------------------------------------------------------------
// Calibration patch (pure helper — used by /api/calibrate handler).
// ---------------------------------------------------------------------------
//
// A calibration patch is a partial update: any subset of the six
// endpoints (tank1Dry/Wet, tank2Dry/Wet, soilDry/Wet) may be
// supplied. Validation rules:
//   - Each raw value must fit in 0..ADC_MAX_RAW.
//   - For any sensor whose pair is being changed (or whose stored
//     pair after applying the patch would be invalid), the absolute
//     distance |dry - wet| must be ≥ MIN_CAL_SPAN.
//
// Validation runs to completion before any field of `out` is
// touched, so a rejected patch leaves `out` byte-identical to its
// pre-call state — same contract as applySettingsPatch().
enum class CalibrationPatchResult : uint8_t {
    Ok = 0,
    OutOfRange,    // a raw value > ADC_MAX_RAW
    SpanTooSmall,  // |dry - wet| < MIN_CAL_SPAN for some sensor
};

struct CalibrationPatch {
    bool     hasTank1Dry = false;  uint16_t tank1Dry = 0;
    bool     hasTank1Wet = false;  uint16_t tank1Wet = 0;
    bool     hasTank2Dry = false;  uint16_t tank2Dry = 0;
    bool     hasTank2Wet = false;  uint16_t tank2Wet = 0;
    bool     hasSoilDry  = false;  uint16_t soilDry  = 0;
    bool     hasSoilWet  = false;  uint16_t soilWet  = 0;
    bool     hasSoil2Dry = false;  uint16_t soil2Dry = 0;
    bool     hasSoil2Wet = false;  uint16_t soil2Wet = 0;
};

CalibrationPatchResult applyCalibrationPatch(const CalibrationPatch& patch,
                                             Settings& out);

const char* calibrationPatchErrorText(CalibrationPatchResult r);
