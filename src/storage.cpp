// storage.cpp — NVS-backed persistence for user settings.
//
// Schema is keyed by NVS_NAMESPACE / NVS_KEY_*. NVS_KEY_VERSION lets
// us migrate or wipe stale layouts in the future. Any load that
// fails sanity (missing version, garbage strings, out-of-range
// numbers) falls back to defaults; the corrected struct is then
// written back so subsequent boots are clean.

#include "storage.h"

#include <string.h>

#include "config.h"

#ifdef ARDUINO
  #include <Arduino.h>
  #include <Preferences.h>
  static Preferences g_prefs;
#endif

void settingsLoadDefaults(Settings& out) {
    out.soilThresholdPct = DEFAULT_SOIL_THRESHOLD_PCT;
    out.minWaterPct      = DEFAULT_MIN_WATER_PCT;
    out.maxPumpSeconds   = DEFAULT_MAX_PUMP_SECONDS;
    strncpy(out.ssid, DEFAULT_AP_SSID, sizeof(out.ssid) - 1);
    out.ssid[sizeof(out.ssid) - 1] = '\0';
    strncpy(out.pass, DEFAULT_AP_PASSWORD, sizeof(out.pass) - 1);
    out.pass[sizeof(out.pass) - 1] = '\0';
    out.autoMode = DEFAULT_AUTO_MODE;
    // Calibration: nominal values per PROJECT_BRIEF.md.
    out.tank1RawDry = DEFAULT_TANK_RAW_DRY;
    out.tank1RawWet = DEFAULT_TANK_RAW_WET;
    out.tank2RawDry = DEFAULT_TANK_RAW_DRY;
    out.tank2RawWet = DEFAULT_TANK_RAW_WET;
    out.soilRawDry  = DEFAULT_SOIL_RAW_DRY;
    out.soilRawWet  = DEFAULT_SOIL_RAW_WET;
    out.soil2RawDry = DEFAULT_SOIL_RAW_DRY;
    out.soil2RawWet = DEFAULT_SOIL_RAW_WET;
    // WiFi STA
    strncpy(out.staSSID, DEFAULT_STA_SSID, sizeof(out.staSSID) - 1);
    out.staSSID[sizeof(out.staSSID) - 1] = '\0';
    strncpy(out.staPass, DEFAULT_STA_PASS, sizeof(out.staPass) - 1);
    out.staPass[sizeof(out.staPass) - 1] = '\0';
    // AI Server
    strncpy(out.serverUrl, DEFAULT_SERVER_URL, sizeof(out.serverUrl) - 1);
    out.serverUrl[sizeof(out.serverUrl) - 1] = '\0';
    strncpy(out.serverApiKey, DEFAULT_SERVER_API_KEY, sizeof(out.serverApiKey) - 1);
    out.serverApiKey[sizeof(out.serverApiKey) - 1] = '\0';

}

bool settingsClamp(Settings& s) {
    bool changed = false;

    if (s.soilThresholdPct > MAX_THRESHOLD_PCT) {
        s.soilThresholdPct = MAX_THRESHOLD_PCT;
        changed = true;
    }
    if (s.minWaterPct > MAX_THRESHOLD_PCT) {
        s.minWaterPct = MAX_THRESHOLD_PCT;
        changed = true;
    }
    if (s.maxPumpSeconds < MIN_PUMP_SECONDS) {
        s.maxPumpSeconds = MIN_PUMP_SECONDS;
        changed = true;
    }
    if (s.maxPumpSeconds > MAX_PUMP_SECONDS) {
        s.maxPumpSeconds = MAX_PUMP_SECONDS;
        changed = true;
    }

    // SSID: must be 1..MAX_SSID_LEN chars, fall back to default if empty.
    size_t ssidLen = strnlen(s.ssid, sizeof(s.ssid));
    if (ssidLen < MIN_SSID_LEN || ssidLen > MAX_SSID_LEN) {
        strncpy(s.ssid, DEFAULT_AP_SSID, sizeof(s.ssid) - 1);
        s.ssid[sizeof(s.ssid) - 1] = '\0';
        changed = true;
    }

    // Password: empty (open AP) is legal; otherwise WPA2 needs 8..63.
    size_t passLen = strnlen(s.pass, sizeof(s.pass));
    if (passLen != 0 && (passLen < MIN_PASS_LEN || passLen > MAX_PASS_LEN)) {
        strncpy(s.pass, DEFAULT_AP_PASSWORD, sizeof(s.pass) - 1);
        s.pass[sizeof(s.pass) - 1] = '\0';
        changed = true;
    }

    // Calibration endpoints. Each raw value must fit in the ADC
    // range; a sensor whose stored span is smaller than MIN_CAL_SPAN
    // is unusable, so reset both endpoints to defaults rather than
    // silently producing garbage percentages. This is the recovery
    // path for a corrupted NVS read; /api/calibrate runs full
    // validation up front.
    auto clampRaw = [](uint16_t v) -> uint16_t {
        return v > ADC_MAX_RAW ? (uint16_t)ADC_MAX_RAW : v;
    };
    auto absDiff = [](uint16_t a, uint16_t b) -> uint16_t {
        return a >= b ? (uint16_t)(a - b) : (uint16_t)(b - a);
    };

    uint16_t t1d = clampRaw(s.tank1RawDry);
    uint16_t t1w = clampRaw(s.tank1RawWet);
    if (t1d != s.tank1RawDry || t1w != s.tank1RawWet) changed = true;
    if (absDiff(t1d, t1w) < MIN_CAL_SPAN) {
        t1d = DEFAULT_TANK_RAW_DRY; t1w = DEFAULT_TANK_RAW_WET;
        changed = true;
    }
    s.tank1RawDry = t1d; s.tank1RawWet = t1w;

    uint16_t t2d = clampRaw(s.tank2RawDry);
    uint16_t t2w = clampRaw(s.tank2RawWet);
    if (t2d != s.tank2RawDry || t2w != s.tank2RawWet) changed = true;
    if (absDiff(t2d, t2w) < MIN_CAL_SPAN) {
        t2d = DEFAULT_TANK_RAW_DRY; t2w = DEFAULT_TANK_RAW_WET;
        changed = true;
    }
    s.tank2RawDry = t2d; s.tank2RawWet = t2w;

    uint16_t sd = clampRaw(s.soilRawDry);
    uint16_t sw = clampRaw(s.soilRawWet);
    if (sd != s.soilRawDry || sw != s.soilRawWet) changed = true;
    if (absDiff(sd, sw) < MIN_CAL_SPAN) {
        sd = DEFAULT_SOIL_RAW_DRY; sw = DEFAULT_SOIL_RAW_WET;
        changed = true;
    }
    s.soilRawDry = sd; s.soilRawWet = sw;

    uint16_t s2d = clampRaw(s.soil2RawDry);
    uint16_t s2w = clampRaw(s.soil2RawWet);
    if (s2d != s.soil2RawDry || s2w != s.soil2RawWet) changed = true;
    if (absDiff(s2d, s2w) < MIN_CAL_SPAN) {
        s2d = DEFAULT_SOIL_RAW_DRY; s2w = DEFAULT_SOIL_RAW_WET;
        changed = true;
    }
    s.soil2RawDry = s2d; s.soil2RawWet = s2w;

    return changed;
}

void storageBegin() {
    // Preferences::begin() is per-load/save in this codebase to keep
    // the NVS handle scoped; nothing to do up-front.
}

bool storageLoadSettings(Settings& out) {
    settingsLoadDefaults(out);
#ifdef ARDUINO
    if (!g_prefs.begin(NVS_NAMESPACE, /*readOnly=*/true)) {
        Serial.println(F("[storage] NVS namespace missing; using defaults"));
        return false;
    }

    uint8_t schema = g_prefs.getUChar(NVS_KEY_VERSION, 0);
    if (schema != NVS_SCHEMA_VERSION) {
        g_prefs.end();
        Serial.printf("[storage] schema mismatch (have %u want %u); defaults\n",
                      schema, (unsigned)NVS_SCHEMA_VERSION);
        return false;
    }

    out.soilThresholdPct = g_prefs.getUChar(NVS_KEY_THRESHOLD, DEFAULT_SOIL_THRESHOLD_PCT);
    out.minWaterPct      = g_prefs.getUChar(NVS_KEY_MIN_WATER, DEFAULT_MIN_WATER_PCT);
    out.maxPumpSeconds   = g_prefs.getUShort(NVS_KEY_MAX_SECONDS, DEFAULT_MAX_PUMP_SECONDS);
    out.autoMode         = g_prefs.getBool(NVS_KEY_AUTO, DEFAULT_AUTO_MODE);

    // Calibration: missing keys fall back to defaults, so an upgrade
    // from schema 1 (which lacked them) leaves working sensors.
    out.tank1RawDry = g_prefs.getUShort(NVS_KEY_T1_DRY, DEFAULT_TANK_RAW_DRY);
    out.tank1RawWet = g_prefs.getUShort(NVS_KEY_T1_WET, DEFAULT_TANK_RAW_WET);
    out.tank2RawDry = g_prefs.getUShort(NVS_KEY_T2_DRY, DEFAULT_TANK_RAW_DRY);
    out.tank2RawWet = g_prefs.getUShort(NVS_KEY_T2_WET, DEFAULT_TANK_RAW_WET);
    out.soilRawDry  = g_prefs.getUShort(NVS_KEY_S_DRY,  DEFAULT_SOIL_RAW_DRY);
    out.soilRawWet  = g_prefs.getUShort(NVS_KEY_S_WET,  DEFAULT_SOIL_RAW_WET);
    out.soil2RawDry = g_prefs.getUShort(NVS_KEY_S2_DRY, DEFAULT_SOIL_RAW_DRY);
    out.soil2RawWet = g_prefs.getUShort(NVS_KEY_S2_WET, DEFAULT_SOIL_RAW_WET);

    String ssid = g_prefs.getString(NVS_KEY_SSID, DEFAULT_AP_SSID);
    String pass = g_prefs.getString(NVS_KEY_PASS, DEFAULT_AP_PASSWORD);
    strncpy(out.ssid, ssid.c_str(), sizeof(out.ssid) - 1);
    out.ssid[sizeof(out.ssid) - 1] = '\0';
    strncpy(out.pass, pass.c_str(), sizeof(out.pass) - 1);
    out.pass[sizeof(out.pass) - 1] = '\0';

    // WiFi STA
    String staSsid = g_prefs.getString(NVS_KEY_STA_SSID, "");
    String staPass = g_prefs.getString(NVS_KEY_STA_PASS, "");
    strncpy(out.staSSID, staSsid.c_str(), sizeof(out.staSSID) - 1);
    out.staSSID[sizeof(out.staSSID) - 1] = '\0';
    strncpy(out.staPass, staPass.c_str(), sizeof(out.staPass) - 1);
    out.staPass[sizeof(out.staPass) - 1] = '\0';

    // AI Server
    String srvUrl = g_prefs.getString(NVS_KEY_SERVER_URL, "");
    String srvKey = g_prefs.getString(NVS_KEY_SERVER_KEY, "");
    strncpy(out.serverUrl, srvUrl.c_str(), sizeof(out.serverUrl) - 1);
    out.serverUrl[sizeof(out.serverUrl) - 1] = '\0';
    strncpy(out.serverApiKey, srvKey.c_str(), sizeof(out.serverApiKey) - 1);
    out.serverApiKey[sizeof(out.serverApiKey) - 1] = '\0';

    g_prefs.end();

    bool clamped = settingsClamp(out);
    if (clamped) {
        Serial.println(F("[storage] loaded settings clamped to legal range"));
    }
#endif
    return true;
}

bool storageSaveSettings(const Settings& s) {
#ifdef ARDUINO
    Settings tmp = s;
    settingsClamp(tmp);

    if (!g_prefs.begin(NVS_NAMESPACE, /*readOnly=*/false)) {
        Serial.println(F("[storage] NVS open RW failed"));
        return false;
    }
    // Each Preferences::putXxx returns the number of bytes written
    // (0 on failure). Track success so a partial write — e.g. NVS
    // namespace full — is reported back to the caller, who depends
    // on this signal for the persist-before-apply path.
    bool ok = true;
    ok &= (g_prefs.putUChar(NVS_KEY_VERSION,    NVS_SCHEMA_VERSION)        != 0);
    ok &= (g_prefs.putUChar(NVS_KEY_THRESHOLD,  tmp.soilThresholdPct)      != 0);
    ok &= (g_prefs.putUChar(NVS_KEY_MIN_WATER,  tmp.minWaterPct)           != 0);
    ok &= (g_prefs.putUShort(NVS_KEY_MAX_SECONDS, tmp.maxPumpSeconds)      != 0);
    // putBool returns the size of a uint8_t key; treat 0 as failure.
    ok &= (g_prefs.putBool(NVS_KEY_AUTO,        tmp.autoMode)              != 0);
    ok &= (g_prefs.putString(NVS_KEY_SSID,      tmp.ssid)                  != 0);
    // Empty password is legal (open AP); putString returns 0 for an
    // empty value, so don't AND it into ok in that case.
    size_t passW = g_prefs.putString(NVS_KEY_PASS, tmp.pass);
    if (tmp.pass[0] != '\0' && passW == 0) ok = false;
    // Calibration endpoints. putUShort returns the bytes written
    // (>0 on success); 0 means failure (NVS full / write error).
    ok &= (g_prefs.putUShort(NVS_KEY_T1_DRY, tmp.tank1RawDry) != 0);
    ok &= (g_prefs.putUShort(NVS_KEY_T1_WET, tmp.tank1RawWet) != 0);
    ok &= (g_prefs.putUShort(NVS_KEY_T2_DRY, tmp.tank2RawDry) != 0);
    ok &= (g_prefs.putUShort(NVS_KEY_T2_WET, tmp.tank2RawWet) != 0);
    ok &= (g_prefs.putUShort(NVS_KEY_S_DRY,  tmp.soilRawDry)  != 0);
    ok &= (g_prefs.putUShort(NVS_KEY_S_WET,  tmp.soilRawWet)  != 0);
    ok &= (g_prefs.putUShort(NVS_KEY_S2_DRY, tmp.soil2RawDry) != 0);
    ok &= (g_prefs.putUShort(NVS_KEY_S2_WET, tmp.soil2RawWet) != 0);
    // WiFi STA
    g_prefs.putString(NVS_KEY_STA_SSID, tmp.staSSID);
    g_prefs.putString(NVS_KEY_STA_PASS, tmp.staPass);
    // AI Server
    g_prefs.putString(NVS_KEY_SERVER_URL, tmp.serverUrl);
    g_prefs.putString(NVS_KEY_SERVER_KEY, tmp.serverApiKey);
    g_prefs.end();
    if (!ok) {
        Serial.println(F("[storage] partial NVS write; settings not fully persisted"));
        return false;
    }
    Serial.println(F("[storage] settings saved"));
#else
    (void)s;
#endif
    return true;
}

bool storageResetSettings() {
#ifdef ARDUINO
    if (!g_prefs.begin(NVS_NAMESPACE, /*readOnly=*/false)) return false;
    g_prefs.clear();
    g_prefs.end();
    Serial.println(F("[storage] settings cleared"));
#endif
    return true;
}

// ---------------------------------------------------------------------------
// applySettingsPatch — pure validate+apply helper.
//
// Validation is performed in full BEFORE any field of `out` is touched
// so that a rejected request never leaves the struct half-mutated.
// This is a hard requirement for the HTTP rollback path: the caller
// must be able to assume that a non-Ok result means `out` is byte-
// identical to its pre-call state.
// ---------------------------------------------------------------------------
SettingsPatchResult applySettingsPatch(const SettingsPatch& patch,
                                       Settings& out,
                                       bool& needApRestart) {
    needApRestart = false;

    // ---- Phase 1: validation (string fields only — numerics are clamped). ----
    if (patch.hasSsid) {
        if (!patch.ssid || patch.ssid[0] == '\0') {
            return SettingsPatchResult::SsidRequired;
        }
        size_t n = strlen(patch.ssid);
        if (n > MAX_SSID_LEN) {
            return SettingsPatchResult::SsidTooLong;
        }
    }

    // The redacted sentinel "***" means "leave password alone" — it is
    // what GET /api/settings returns, and round-tripping it must not
    // be treated as a password change.
    const bool passIsSentinel =
        patch.hasPass && patch.pass && strcmp(patch.pass, "***") == 0;

    if (patch.hasPass && patch.pass && !passIsSentinel) {
        size_t n = strlen(patch.pass);
        if (n == 0) {
            // Empty password = open AP. Refuse unless caller explicitly
            // opts in. settingsClamp() would otherwise have silently
            // downgraded a missing password to DEFAULT_AP_PASSWORD,
            // a publicly documented credential.
            if (!patch.allowOpen) {
                return SettingsPatchResult::OpenApNotAllowed;
            }
        } else if (n < MIN_PASS_LEN) {
            return SettingsPatchResult::PassTooShort;
        } else if (n > MAX_PASS_LEN) {
            return SettingsPatchResult::PassTooLong;
        }
    }

    // ---- Phase 2: apply (only after every rule above has passed). ----
    if (patch.hasThreshold) {
        int v = patch.threshold;
        if (v < 0)   v = 0;
        if (v > 100) v = 100;
        out.soilThresholdPct = (uint8_t)v;
    }
    if (patch.hasMinWater) {
        int v = patch.minWater;
        if (v < 0)   v = 0;
        if (v > 100) v = 100;
        out.minWaterPct = (uint8_t)v;
    }
    if (patch.hasMaxTimeMin) {
        // API contract: max_time is minutes; we store seconds.
        int mins = patch.maxTimeMin;
        if (mins < 1)  mins = 1;
        if (mins > 60) mins = 60;
        out.maxPumpSeconds = (uint16_t)(mins * 60);
    }
    if (patch.hasAutoMode) {
        out.autoMode = patch.autoMode;
    }
    if (patch.hasSsid) {
        if (strncmp(patch.ssid, out.ssid, sizeof(out.ssid)) != 0) {
            strncpy(out.ssid, patch.ssid, sizeof(out.ssid) - 1);
            out.ssid[sizeof(out.ssid) - 1] = '\0';
            needApRestart = true;
        }
    }
    if (patch.hasPass && patch.pass && !passIsSentinel) {
        if (strncmp(patch.pass, out.pass, sizeof(out.pass)) != 0) {
            strncpy(out.pass, patch.pass, sizeof(out.pass) - 1);
            out.pass[sizeof(out.pass) - 1] = '\0';
            needApRestart = true;
        }
    }
    return SettingsPatchResult::Ok;
}

const char* settingsPatchErrorText(SettingsPatchResult r) {
    switch (r) {
        case SettingsPatchResult::Ok:               return "ok";
        case SettingsPatchResult::SsidRequired:     return "ssid_required";
        case SettingsPatchResult::SsidTooLong:      return "ssid_too_long";
        case SettingsPatchResult::OpenApNotAllowed: return "open_ap_not_allowed";
        case SettingsPatchResult::PassTooShort:     return "pass_too_short";
        case SettingsPatchResult::PassTooLong:      return "pass_too_long";
    }
    return "unknown";
}
