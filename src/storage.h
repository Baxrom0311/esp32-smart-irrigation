// storage.h — NVS-backed settings persistence.

#pragma once

#include "state.h"

// Populate `out` with sane defaults (used as the M2 starting point
// and as a fallback when NVS load fails).
void settingsLoadDefaults(Settings& out);

// Clamp an arbitrary Settings struct to legal ranges in place.
// Returns true if any field had to be adjusted.
bool settingsClamp(Settings& s);

void storageBegin();
bool storageLoadSettings(Settings& out);
bool storageSaveSettings(const Settings& s);
bool storageResetSettings();

// ---------------------------------------------------------------------------
// Pure (Arduino-free) settings-patch helper used by the HTTP layer.
//
// applySettingsPatch() validates an incoming partial-update request and,
// if all fields are legal, mutates `out` in place. It is the single
// authoritative place that decides whether a patch is acceptable;
// HTTP handlers should not duplicate these rules.
//
// The function intentionally does NOT touch NVS or hardware — it is
// safe to unit-test against the native env.
// ---------------------------------------------------------------------------

enum class SettingsPatchResult : uint8_t {
    Ok = 0,
    SsidRequired,
    SsidTooLong,
    OpenApNotAllowed,
    PassTooShort,
    PassTooLong,
};

// Caller-supplied partial update. Each `hasX` flag must be set to
// indicate "field X is present in the request". Numeric fields are
// taken as `int` (clamped internally); strings as `const char*`.
//
// `pass` may be the redacted sentinel "***", in which case the current
// password is preserved. An empty `pass` is rejected unless
// `allowOpen` is true (open AP must be an explicit, opt-in choice).
struct SettingsPatch {
    bool        hasThreshold  = false;  int  threshold  = 0;
    bool        hasMinWater   = false;  int  minWater   = 0;
    bool        hasMaxTimeMin = false;  int  maxTimeMin = 0;
    bool        hasAutoMode   = false;  bool autoMode   = false;
    bool        hasSsid       = false;  const char* ssid = nullptr;
    bool        hasPass       = false;  const char* pass = nullptr;
    bool        allowOpen     = false;
};

// Validate `patch`; on success apply to `out` and report whether the
// SSID or password actually changed (so the caller can schedule an AP
// restart). On any validation failure `out` is left untouched and the
// first offending rule is returned.
//
// Numeric clamping (0..100 for percentages, 1..60 for max_time minutes)
// is part of `Ok` — out-of-range numbers are silently coerced; only
// SSID/password violations surface as errors because those have
// security/reliability consequences a caller must see.
SettingsPatchResult applySettingsPatch(const SettingsPatch& patch,
                                       Settings& out,
                                       bool& needApRestart);

// Stable error string for transport. Suitable for `{"error": "..."}`.
const char* settingsPatchErrorText(SettingsPatchResult r);
