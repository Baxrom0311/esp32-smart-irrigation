// test_native/test_main.cpp — Unity tests for pure helpers.
//
// These run under the `native` PlatformIO env (no Arduino.h). The
// helpers under test are guarded by #ifdef ARDUINO so they compile
// here too. Coverage:
//   - rawWaterToPct   — linear 0..4095 → 0..100, saturated.
//   - rawSoilToPct    — inverted, 4095 → 0%, 0 → 100%.
//   - settingsLoadDefaults — produces valid + clamped defaults.
//   - settingsClamp   — fixes out-of-range fields, leaves valid alone.

#include <unity.h>
#include <stdio.h>
#include <string.h>

#include "../../src/sensors.h"
#include "../../src/storage.h"
#include "../../src/relays.h"
#include "../../src/state.h"
#include "../../src/config.h"
#include "../../src/eventlog.h"
#include "../../src/metrics.h"
#include "../../src/ratelimit.h"

void setUp(void)    {}
void tearDown(void) {}

// ---------- ADC conversion ----------

void test_water_zero_is_zero(void) {
    TEST_ASSERT_EQUAL_UINT8(0, rawWaterToPct(0));
}
void test_water_full_is_hundred(void) {
    TEST_ASSERT_EQUAL_UINT8(100, rawWaterToPct(4095));
}
void test_water_midpoint(void) {
    // ~50% ± 1 (integer rounding).
    uint8_t v = rawWaterToPct(2048);
    TEST_ASSERT_TRUE(v == 50 || v == 49 || v == 51);
}
void test_water_saturates_above_max(void) {
    TEST_ASSERT_EQUAL_UINT8(100, rawWaterToPct(9999));
}

void test_soil_dry_is_zero(void) {
    TEST_ASSERT_EQUAL_UINT8(0, rawSoilToPct(4095));
}
void test_soil_wet_is_hundred(void) {
    TEST_ASSERT_EQUAL_UINT8(100, rawSoilToPct(0));
}
void test_soil_midpoint(void) {
    uint8_t v = rawSoilToPct(2048);
    TEST_ASSERT_TRUE(v == 50 || v == 49 || v == 51);
}

// ---------- Settings ----------

void test_defaults_are_sane(void) {
    Settings s;
    settingsLoadDefaults(s);
    TEST_ASSERT_EQUAL_UINT8 (DEFAULT_SOIL_THRESHOLD_PCT, s.soilThresholdPct);
    TEST_ASSERT_EQUAL_UINT8 (DEFAULT_MIN_WATER_PCT,      s.minWaterPct);
    TEST_ASSERT_EQUAL_UINT16(DEFAULT_MAX_PUMP_SECONDS,   s.maxPumpSeconds);
    TEST_ASSERT_EQUAL_STRING(DEFAULT_AP_SSID, s.ssid);
    TEST_ASSERT_EQUAL_STRING(DEFAULT_AP_PASSWORD, s.pass);
    TEST_ASSERT_TRUE(s.autoMode == DEFAULT_AUTO_MODE);
    // Defaults must already be in-range; clamp() should not change them.
    Settings copy = s;
    bool changed = settingsClamp(copy);
    TEST_ASSERT_FALSE(changed);
}

void test_clamp_fixes_threshold_overflow(void) {
    Settings s;
    settingsLoadDefaults(s);
    s.soilThresholdPct = 250;  // bogus
    bool changed = settingsClamp(s);
    TEST_ASSERT_TRUE(changed);
    TEST_ASSERT_EQUAL_UINT8(MAX_THRESHOLD_PCT, s.soilThresholdPct);
}

void test_clamp_fixes_runtime_too_short(void) {
    Settings s;
    settingsLoadDefaults(s);
    s.maxPumpSeconds = 5;
    bool changed = settingsClamp(s);
    TEST_ASSERT_TRUE(changed);
    TEST_ASSERT_EQUAL_UINT16(MIN_PUMP_SECONDS, s.maxPumpSeconds);
}

void test_clamp_fixes_runtime_too_long(void) {
    Settings s;
    settingsLoadDefaults(s);
    s.maxPumpSeconds = 9999;  // > MAX_PUMP_SECONDS (3600), fits in uint16_t
    bool changed = settingsClamp(s);
    TEST_ASSERT_TRUE(changed);
    TEST_ASSERT_EQUAL_UINT16(MAX_PUMP_SECONDS, s.maxPumpSeconds);
}

void test_clamp_replaces_empty_ssid_with_default(void) {
    Settings s;
    settingsLoadDefaults(s);
    s.ssid[0] = '\0';
    bool changed = settingsClamp(s);
    TEST_ASSERT_TRUE(changed);
    TEST_ASSERT_EQUAL_STRING(DEFAULT_AP_SSID, s.ssid);
}

void test_clamp_allows_empty_password_open_ap(void) {
    Settings s;
    settingsLoadDefaults(s);
    s.pass[0] = '\0';
    bool changed = settingsClamp(s);
    TEST_ASSERT_FALSE(changed);  // empty password is legal (open AP)
    TEST_ASSERT_EQUAL_STRING("", s.pass);
}

void test_clamp_rejects_short_password(void) {
    Settings s;
    settingsLoadDefaults(s);
    strcpy(s.pass, "short");  // <8 chars but non-empty → invalid WPA2
    bool changed = settingsClamp(s);
    TEST_ASSERT_TRUE(changed);
    TEST_ASSERT_EQUAL_STRING(DEFAULT_AP_PASSWORD, s.pass);
}

// ---------- Pump safety FSM (pure) ----------

static PumpSafetyInputs make_inputs(uint8_t tankPct, bool tankErr,
                                    PumpFsm fsm, uint8_t minWater) {
    PumpSafetyInputs in;
    in.tankPct     = tankPct;
    in.tankErr     = tankErr;
    in.fsm         = fsm;
    in.minWaterPct = minWater;
    return in;
}

void test_pump_on_granted_when_safe(void) {
    auto in = make_inputs(80, false, PumpFsm::Off, 20);
    TEST_ASSERT_EQUAL((int)PumpRequestResult::Granted,
                      (int)evaluatePumpOnRequest(in));
}

void test_pump_on_denied_dry_run(void) {
    auto in = make_inputs(15, false, PumpFsm::Off, 20);
    TEST_ASSERT_EQUAL((int)PumpRequestResult::DeniedDryRun,
                      (int)evaluatePumpOnRequest(in));
}

void test_pump_on_denied_at_minimum_boundary_inclusive(void) {
    // tankPct == minWaterPct → should pass (strict <).
    auto ok    = make_inputs(20, false, PumpFsm::Off, 20);
    auto under = make_inputs(19, false, PumpFsm::Off, 20);
    TEST_ASSERT_EQUAL((int)PumpRequestResult::Granted,
                      (int)evaluatePumpOnRequest(ok));
    TEST_ASSERT_EQUAL((int)PumpRequestResult::DeniedDryRun,
                      (int)evaluatePumpOnRequest(under));
}

void test_pump_on_denied_when_tank_sensor_error(void) {
    auto in = make_inputs(80, true, PumpFsm::Off, 20);
    TEST_ASSERT_EQUAL((int)PumpRequestResult::DeniedSensor,
                      (int)evaluatePumpOnRequest(in));
}

void test_pump_on_denied_when_runtime_locked(void) {
    auto in = make_inputs(80, false, PumpFsm::LockoutRuntime, 20);
    TEST_ASSERT_EQUAL((int)PumpRequestResult::DeniedRuntime,
                      (int)evaluatePumpOnRequest(in));
}

void test_pump_on_denied_when_dry_run_locked(void) {
    auto in = make_inputs(80, false, PumpFsm::LockoutDryRun, 20);
    TEST_ASSERT_EQUAL((int)PumpRequestResult::DeniedDryRun,
                      (int)evaluatePumpOnRequest(in));
}

void test_pump_on_dry_run_takes_priority_over_runtime_lockout(void) {
    // Both conditions present: low water is the more urgent reason
    // because filling the tank is the operator's first action.
    auto in = make_inputs(10, false, PumpFsm::LockoutRuntime, 20);
    TEST_ASSERT_EQUAL((int)PumpRequestResult::DeniedDryRun,
                      (int)evaluatePumpOnRequest(in));
}

void test_pump_on_denied_when_fsm_lockout_sensor_without_live_err(void) {
    // FSM is in LockoutSensor (sticky from a prior fault) but the
    // current read happens to be clean. The lockout must still be
    // honoured — recovery is decided by the periodic FSM tick after
    // the streak detector clears, not by a one-shot ON request.
    auto in = make_inputs(80, /*tankErr*/false,
                          PumpFsm::LockoutSensor, 20);
    TEST_ASSERT_EQUAL((int)PumpRequestResult::DeniedSensor,
                      (int)evaluatePumpOnRequest(in));
}

void test_pump_on_live_sensor_err_outranks_runtime_lockout(void) {
    // tankErr beats both LockoutRuntime FSM and historical state;
    // a sensor fault is the worst-actionable condition.
    auto in = make_inputs(80, /*tankErr*/true,
                          PumpFsm::LockoutRuntime, 20);
    TEST_ASSERT_EQUAL((int)PumpRequestResult::DeniedSensor,
                      (int)evaluatePumpOnRequest(in));
}

// ---------- Auto-mode demand (pure) ----------

void test_auto_demand_on_when_dry(void) {
    TEST_ASSERT_TRUE(evaluateAutoDemand(/*soil*/ 30, /*err*/ false,
                                        /*threshold*/ 40,
                                        /*current*/ false));
}

void test_auto_demand_off_when_well_above_threshold(void) {
    // threshold=40, hysteresis=5 → release at >45.
    TEST_ASSERT_FALSE(evaluateAutoDemand(50, false, 40, true));
}

void test_auto_demand_holds_in_hysteresis_band(void) {
    // soil between threshold (40) and threshold+hysteresis (45):
    // demand should be unchanged.
    TEST_ASSERT_TRUE (evaluateAutoDemand(42, false, 40, true));
    TEST_ASSERT_FALSE(evaluateAutoDemand(42, false, 40, false));
}

void test_auto_demand_off_on_sensor_error(void) {
    TEST_ASSERT_FALSE(evaluateAutoDemand(0, /*err*/ true, 40, true));
}

void test_auto_demand_handles_high_threshold_overflow(void) {
    // thresholdPct=100 → release at >100 which is never; once on,
    // demand stays on. We must not wrap to 0.
    TEST_ASSERT_TRUE(evaluateAutoDemand(99,  false, 100, true));
    TEST_ASSERT_TRUE(evaluateAutoDemand(100, false, 100, true));
}

// ---------- pumpFsmText / pumpResultText ----------

void test_pump_fsm_text_off(void) {
    TEST_ASSERT_EQUAL_STRING("off", pumpFsmText(PumpFsm::Off));
}
void test_pump_fsm_text_on(void) {
    TEST_ASSERT_EQUAL_STRING("on", pumpFsmText(PumpFsm::On));
}
void test_pump_fsm_text_lockouts(void) {
    TEST_ASSERT_EQUAL_STRING("lockout_dry_run", pumpFsmText(PumpFsm::LockoutDryRun));
    TEST_ASSERT_EQUAL_STRING("lockout_runtime", pumpFsmText(PumpFsm::LockoutRuntime));
    TEST_ASSERT_EQUAL_STRING("lockout_sensor",  pumpFsmText(PumpFsm::LockoutSensor));
}
void test_pump_result_text_basic(void) {
    TEST_ASSERT_EQUAL_STRING("ok",              pumpResultText(PumpRequestResult::Granted));
    TEST_ASSERT_EQUAL_STRING("dry_run",         pumpResultText(PumpRequestResult::DeniedDryRun));
    TEST_ASSERT_EQUAL_STRING("sensor_error",    pumpResultText(PumpRequestResult::DeniedSensor));
    TEST_ASSERT_EQUAL_STRING("runtime_lockout", pumpResultText(PumpRequestResult::DeniedRuntime));
    TEST_ASSERT_EQUAL_STRING("invalid_request", pumpResultText(PumpRequestResult::DeniedInvalid));
}

void test_pump_result_text_busy_distinct_from_invalid(void) {
    // DeniedBusy must NOT collapse to "invalid_request" — a transient
    // mutex contention is retryable and the operator needs to see
    // that, not a misleading 409 invalid_request that suggests they
    // sent a malformed request.
    TEST_ASSERT_EQUAL_STRING("state_busy",
        pumpResultText(PumpRequestResult::DeniedBusy));
    // And the two values must be distinct enum members so the HTTP
    // layer can branch on them.
    TEST_ASSERT_NOT_EQUAL((int)PumpRequestResult::DeniedInvalid,
                          (int)PumpRequestResult::DeniedBusy);
}

// ---------- composePumpsErrorMessage ----------

static PumpState mkPump(PumpFsm fsm) {
    PumpState p{};
    p.fsm = fsm;
    p.on = (fsm == PumpFsm::On);
    return p;
}

void test_compose_error_empty_when_all_clean(void) {
    PumpState pumps[2] = { mkPump(PumpFsm::Off), mkPump(PumpFsm::On) };
    char buf[64] = "old";
    composePumpsErrorMessage(pumps, false, false, false, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("", buf);
}

void test_compose_error_sensor_takes_priority(void) {
    PumpState pumps[2] = { mkPump(PumpFsm::Off), mkPump(PumpFsm::LockoutDryRun) };
    char buf[64];
    // Tank 1 sensor error, pump 2 dry-run lockout — sensor wins.
    composePumpsErrorMessage(pumps, /*t1*/true, /*t2*/false, /*dht*/true,
                             buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("Tank 1 sensor error", buf);
}

void test_compose_error_dry_run_above_runtime(void) {
    PumpState pumps[2] = { mkPump(PumpFsm::LockoutRuntime),
                           mkPump(PumpFsm::LockoutDryRun) };
    char buf[64];
    composePumpsErrorMessage(pumps, false, false, false, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("Tank 2 water low", buf);
}

void test_compose_error_runtime_when_only_one(void) {
    PumpState pumps[2] = { mkPump(PumpFsm::LockoutRuntime),
                           mkPump(PumpFsm::Off) };
    char buf[64];
    composePumpsErrorMessage(pumps, false, false, false, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("Pump 1 runtime limit", buf);
}

void test_compose_error_dht_only_lowest_priority(void) {
    PumpState pumps[2] = { mkPump(PumpFsm::Off), mkPump(PumpFsm::On) };
    char buf[64];
    composePumpsErrorMessage(pumps, false, false, /*dht*/true,
                             buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("DHT sensor error", buf);
}

void test_compose_error_handles_null_buffer(void) {
    PumpState pumps[2] = { mkPump(PumpFsm::LockoutDryRun), mkPump(PumpFsm::Off) };
    // Must not crash.
    composePumpsErrorMessage(pumps, false, false, false, nullptr, 0);
    char tiny[1] = {'X'};
    composePumpsErrorMessage(pumps, false, false, false, tiny, 1);
    TEST_ASSERT_EQUAL_CHAR('\0', tiny[0]);  // truncated cleanly
}

// ---------- Additional edge cases ----------

void test_auto_demand_exactly_at_threshold(void) {
    // soil == threshold. Strict `<` means "not yet dry"; demand
    // depends on hysteresis hold (currentDemand). At threshold we
    // are inside the hysteresis band, so demand must be held.
    TEST_ASSERT_FALSE(evaluateAutoDemand(40, false, 40, false));
    TEST_ASSERT_TRUE (evaluateAutoDemand(40, false, 40, true));
}

void test_auto_demand_just_below_threshold(void) {
    // 39 < 40 → demand ON regardless of previous state.
    TEST_ASSERT_TRUE(evaluateAutoDemand(39, false, 40, false));
    TEST_ASSERT_TRUE(evaluateAutoDemand(39, false, 40, true));
}

void test_auto_demand_zero_threshold_is_never_on(void) {
    // threshold=0 means "never auto-water"; soil cannot be < 0.
    TEST_ASSERT_FALSE(evaluateAutoDemand(0,   false, 0, false));
    TEST_ASSERT_FALSE(evaluateAutoDemand(50,  false, 0, false));
    // If we were already on (e.g. user lowered threshold), release
    // when soil > hysteresis band, hold otherwise.
    TEST_ASSERT_FALSE(evaluateAutoDemand(50,  false, 0, true));
}

void test_pump_evaluate_off_request_helper_unused(void) {
    // OFF requests bypass evaluatePumpOnRequest entirely; verify the
    // helper still returns Granted for "safe + Off FSM" so the web
    // layer doesn't accidentally deny a benign re-OFF.
    auto in = make_inputs(80, false, PumpFsm::Off, 20);
    TEST_ASSERT_EQUAL((int)PumpRequestResult::Granted,
                      (int)evaluatePumpOnRequest(in));
}

void test_clamp_accepts_minimum_legal_password(void) {
    Settings s;
    settingsLoadDefaults(s);
    strcpy(s.pass, "12345678");  // 8 chars, WPA2 minimum
    bool changed = settingsClamp(s);
    TEST_ASSERT_FALSE(changed);
    TEST_ASSERT_EQUAL_STRING("12345678", s.pass);
}

void test_clamp_accepts_maximum_legal_password(void) {
    Settings s;
    settingsLoadDefaults(s);
    // 63 chars, WPA2 maximum.
    const char p63[] = "abcdefghijabcdefghijabcdefghijabcdefghijabcdefghijabcdefghij123";
    TEST_ASSERT_EQUAL_size_t(63u, strlen(p63));
    strcpy(s.pass, p63);
    bool changed = settingsClamp(s);
    TEST_ASSERT_FALSE(changed);
    TEST_ASSERT_EQUAL_STRING(p63, s.pass);
}

void test_clamp_min_water_overflow(void) {
    Settings s;
    settingsLoadDefaults(s);
    s.minWaterPct = 200;
    bool changed = settingsClamp(s);
    TEST_ASSERT_TRUE(changed);
    TEST_ASSERT_EQUAL_UINT8(MAX_THRESHOLD_PCT, s.minWaterPct);
}

void test_compose_error_truncates_to_buffer(void) {
    // Buffer just barely too small: snprintf truncates with NUL.
    PumpState pumps[2] = { mkPump(PumpFsm::LockoutDryRun), mkPump(PumpFsm::Off) };
    char buf[8];
    composePumpsErrorMessage(pumps, false, false, false, buf, sizeof(buf));
    // "Tank 1 water low" doesn't fit in 8 bytes; expect truncation
    // with NUL terminator at index 7.
    TEST_ASSERT_EQUAL_CHAR('\0', buf[sizeof(buf) - 1]);
    TEST_ASSERT_EQUAL_size_t(sizeof(buf) - 1, strnlen(buf, sizeof(buf)));
}

// ---------- applySettingsPatch (pure validate+apply) ----------
//
// These tests exercise the helper extracted from handlePostSettings
// (webserver.cpp). The helper is the single authority for SSID/pass
// rules; covering it here means the HTTP layer cannot drift away
// from spec without breaking these assertions.

static Settings make_settings_baseline(void) {
    Settings s;
    settingsLoadDefaults(s);
    return s;
}

void test_patch_empty_is_noop(void) {
    Settings s = make_settings_baseline();
    Settings before = s;
    SettingsPatch p;  // no fields set
    bool restart = true;  // verify it gets cleared even on no-op
    auto r = applySettingsPatch(p, s, restart);
    TEST_ASSERT_EQUAL((int)SettingsPatchResult::Ok, (int)r);
    TEST_ASSERT_FALSE(restart);
    // No field should have changed.
    TEST_ASSERT_EQUAL_UINT8 (before.soilThresholdPct, s.soilThresholdPct);
    TEST_ASSERT_EQUAL_UINT8 (before.minWaterPct,      s.minWaterPct);
    TEST_ASSERT_EQUAL_UINT16(before.maxPumpSeconds,   s.maxPumpSeconds);
    TEST_ASSERT_EQUAL_STRING(before.ssid, s.ssid);
    TEST_ASSERT_EQUAL_STRING(before.pass, s.pass);
}

void test_patch_ssid_required_rejects_empty(void) {
    Settings s = make_settings_baseline();
    Settings before = s;
    SettingsPatch p;
    p.hasSsid = true; p.ssid = "";
    bool restart = false;
    auto r = applySettingsPatch(p, s, restart);
    TEST_ASSERT_EQUAL((int)SettingsPatchResult::SsidRequired, (int)r);
    TEST_ASSERT_FALSE(restart);
    // On error, struct must be byte-identical to its pre-call state.
    TEST_ASSERT_EQUAL_STRING(before.ssid, s.ssid);
}

void test_patch_ssid_required_rejects_null(void) {
    Settings s = make_settings_baseline();
    SettingsPatch p;
    p.hasSsid = true; p.ssid = nullptr;
    bool restart = false;
    auto r = applySettingsPatch(p, s, restart);
    TEST_ASSERT_EQUAL((int)SettingsPatchResult::SsidRequired, (int)r);
}

void test_patch_ssid_too_long(void) {
    Settings s = make_settings_baseline();
    SettingsPatch p;
    // 33 chars: one over MAX_SSID_LEN (32).
    p.hasSsid = true; p.ssid = "abcdefghijabcdefghijabcdefghij123";
    bool restart = false;
    auto r = applySettingsPatch(p, s, restart);
    TEST_ASSERT_EQUAL((int)SettingsPatchResult::SsidTooLong, (int)r);
    // Original SSID untouched.
    TEST_ASSERT_EQUAL_STRING(DEFAULT_AP_SSID, s.ssid);
}

void test_patch_pass_too_short(void) {
    Settings s = make_settings_baseline();
    Settings before = s;
    SettingsPatch p;
    p.hasPass = true; p.pass = "short";  // 5 < 8
    bool restart = false;
    auto r = applySettingsPatch(p, s, restart);
    TEST_ASSERT_EQUAL((int)SettingsPatchResult::PassTooShort, (int)r);
    // Critically: pass must NOT have been silently replaced with
    // DEFAULT_AP_PASSWORD (the regression we're guarding against).
    TEST_ASSERT_EQUAL_STRING(before.pass, s.pass);
}

void test_patch_pass_too_long(void) {
    Settings s = make_settings_baseline();
    SettingsPatch p;
    // 64 chars (one over MAX_PASS_LEN = 63).
    p.hasPass = true;
    p.pass = "abcdefghijabcdefghijabcdefghijabcdefghijabcdefghijabcdefghij1234";
    bool restart = false;
    auto r = applySettingsPatch(p, s, restart);
    TEST_ASSERT_EQUAL((int)SettingsPatchResult::PassTooLong, (int)r);
}

void test_patch_open_ap_rejected_by_default(void) {
    Settings s = make_settings_baseline();
    Settings before = s;
    SettingsPatch p;
    p.hasPass = true; p.pass = "";
    p.allowOpen = false;
    bool restart = false;
    auto r = applySettingsPatch(p, s, restart);
    TEST_ASSERT_EQUAL((int)SettingsPatchResult::OpenApNotAllowed, (int)r);
    TEST_ASSERT_EQUAL_STRING(before.pass, s.pass);
}

void test_patch_open_ap_allowed_with_explicit_flag(void) {
    Settings s = make_settings_baseline();
    SettingsPatch p;
    p.hasPass = true; p.pass = "";
    p.allowOpen = true;
    bool restart = false;
    auto r = applySettingsPatch(p, s, restart);
    TEST_ASSERT_EQUAL((int)SettingsPatchResult::Ok, (int)r);
    TEST_ASSERT_TRUE(restart);
    TEST_ASSERT_EQUAL_STRING("", s.pass);
}

void test_patch_pass_sentinel_preserves_current(void) {
    Settings s = make_settings_baseline();
    strcpy(s.pass, "MyRealPass123");
    SettingsPatch p;
    p.hasPass = true; p.pass = "***";  // redacted: leave as-is
    bool restart = true;  // should remain false (no real change)
    auto r = applySettingsPatch(p, s, restart);
    TEST_ASSERT_EQUAL((int)SettingsPatchResult::Ok, (int)r);
    TEST_ASSERT_FALSE(restart);
    TEST_ASSERT_EQUAL_STRING("MyRealPass123", s.pass);
}

void test_patch_pass_sentinel_outranks_allow_open(void) {
    // Defence-in-depth: a client that sends both pass:"***" (the
    // redacted sentinel returned by GET /api/settings) AND
    // allow_open:true must NOT degrade to an open AP. The sentinel
    // means "preserve current pass" and must always win over the
    // allow_open flag.
    Settings s = make_settings_baseline();
    strcpy(s.pass, "OriginalPass");
    SettingsPatch p;
    p.hasPass   = true; p.pass = "***";
    p.allowOpen = true;
    bool restart = false;
    auto r = applySettingsPatch(p, s, restart);
    TEST_ASSERT_EQUAL((int)SettingsPatchResult::Ok, (int)r);
    TEST_ASSERT_FALSE(restart);
    TEST_ASSERT_EQUAL_STRING("OriginalPass", s.pass);
}

void test_patch_apply_numeric_clamps(void) {
    Settings s = make_settings_baseline();
    SettingsPatch p;
    p.hasThreshold  = true; p.threshold  = 250;   // > 100
    p.hasMinWater   = true; p.minWater   = -10;   // < 0
    p.hasMaxTimeMin = true; p.maxTimeMin = 999;   // > 60
    bool restart = false;
    auto r = applySettingsPatch(p, s, restart);
    TEST_ASSERT_EQUAL((int)SettingsPatchResult::Ok, (int)r);
    TEST_ASSERT_FALSE(restart);
    TEST_ASSERT_EQUAL_UINT8(100, s.soilThresholdPct);
    TEST_ASSERT_EQUAL_UINT8(0,   s.minWaterPct);
    TEST_ASSERT_EQUAL_UINT16(60u * 60u, s.maxPumpSeconds);
}

void test_patch_apply_changes_ssid_sets_restart(void) {
    Settings s = make_settings_baseline();
    SettingsPatch p;
    p.hasSsid = true; p.ssid = "NewAccessPoint";
    bool restart = false;
    auto r = applySettingsPatch(p, s, restart);
    TEST_ASSERT_EQUAL((int)SettingsPatchResult::Ok, (int)r);
    TEST_ASSERT_TRUE(restart);
    TEST_ASSERT_EQUAL_STRING("NewAccessPoint", s.ssid);
}

void test_patch_unchanged_ssid_does_not_set_restart(void) {
    Settings s = make_settings_baseline();
    SettingsPatch p;
    p.hasSsid = true; p.ssid = DEFAULT_AP_SSID;  // identical
    bool restart = false;
    auto r = applySettingsPatch(p, s, restart);
    TEST_ASSERT_EQUAL((int)SettingsPatchResult::Ok, (int)r);
    TEST_ASSERT_FALSE(restart);
}

void test_patch_validation_runs_before_apply(void) {
    // If a patch carries a valid threshold AND an invalid pass, the
    // result must be the pass error AND the threshold must remain
    // unchanged. This is the property the rollback path relies on.
    Settings s = make_settings_baseline();
    uint8_t origThreshold = s.soilThresholdPct;
    SettingsPatch p;
    p.hasThreshold = true; p.threshold = 75;
    p.hasPass      = true; p.pass      = "abc";  // too short
    bool restart = false;
    auto r = applySettingsPatch(p, s, restart);
    TEST_ASSERT_EQUAL((int)SettingsPatchResult::PassTooShort, (int)r);
    TEST_ASSERT_EQUAL_UINT8(origThreshold, s.soilThresholdPct);
}

void test_patch_error_text_matches_api_contract(void) {
    // These exact strings are part of the public API contract; clients
    // (including the settings page JS) match on them.
    TEST_ASSERT_EQUAL_STRING("ssid_required",
        settingsPatchErrorText(SettingsPatchResult::SsidRequired));
    TEST_ASSERT_EQUAL_STRING("ssid_too_long",
        settingsPatchErrorText(SettingsPatchResult::SsidTooLong));
    TEST_ASSERT_EQUAL_STRING("open_ap_not_allowed",
        settingsPatchErrorText(SettingsPatchResult::OpenApNotAllowed));
    TEST_ASSERT_EQUAL_STRING("pass_too_short",
        settingsPatchErrorText(SettingsPatchResult::PassTooShort));
    TEST_ASSERT_EQUAL_STRING("pass_too_long",
        settingsPatchErrorText(SettingsPatchResult::PassTooLong));
    TEST_ASSERT_EQUAL_STRING("ok",
        settingsPatchErrorText(SettingsPatchResult::Ok));
}

// ---------- updateStuckStreak (ADC stuck-rail detection) ----------
//
// Safety-critical: a stuck-high tank sensor would falsely report
// "full" and let a dry pump start; a stuck-low one would lock out
// a valid pump. We must trip on a sustained rail value but tolerate
// a single noise spike at boundary readings.

void test_stuck_clean_reading_keeps_streak_zero(void) {
    uint8_t streak = 0;
    // Mid-range, well clear of either rail.
    bool err = updateStuckStreak(2048, streak);
    TEST_ASSERT_FALSE(err);
    TEST_ASSERT_EQUAL_UINT8(0, streak);
}

void test_stuck_low_rail_trips_after_threshold(void) {
    uint8_t streak = 0;
    // ADC_STUCK_STREAK = 3 → false, false, true.
    TEST_ASSERT_FALSE(updateStuckStreak(0, streak));
    TEST_ASSERT_EQUAL_UINT8(1, streak);
    TEST_ASSERT_FALSE(updateStuckStreak(0, streak));
    TEST_ASSERT_EQUAL_UINT8(2, streak);
    TEST_ASSERT_TRUE(updateStuckStreak(0, streak));
    TEST_ASSERT_EQUAL_UINT8(3, streak);
}

void test_stuck_high_rail_trips_after_threshold(void) {
    uint8_t streak = 0;
    // 4095 is the high rail; trip after ADC_STUCK_STREAK reads.
    TEST_ASSERT_FALSE(updateStuckStreak(4095, streak));
    TEST_ASSERT_FALSE(updateStuckStreak(4095, streak));
    TEST_ASSERT_TRUE (updateStuckStreak(4095, streak));
}

void test_stuck_streak_resets_on_clean_read(void) {
    // A single noise spike at the low rail must NOT trip the
    // detector if a clean read follows.
    uint8_t streak = 0;
    updateStuckStreak(10, streak);   // <= ADC_STUCK_LOW (50)
    updateStuckStreak(20, streak);
    TEST_ASSERT_EQUAL_UINT8(2, streak);
    bool err = updateStuckStreak(2000, streak);  // clean
    TEST_ASSERT_FALSE(err);
    TEST_ASSERT_EQUAL_UINT8(0, streak);
}

void test_stuck_streak_stays_latched_while_stuck(void) {
    // Once tripped, additional stuck reads keep the error asserted.
    uint8_t streak = 0;
    updateStuckStreak(0, streak);
    updateStuckStreak(0, streak);
    updateStuckStreak(0, streak);
    TEST_ASSERT_TRUE(updateStuckStreak(0, streak));
    TEST_ASSERT_TRUE(updateStuckStreak(0, streak));
    TEST_ASSERT_TRUE(updateStuckStreak(0, streak));
}

void test_stuck_streak_clears_after_recovery(void) {
    // After a sensor recovers (clean read), a single subsequent
    // stuck read must NOT immediately re-trip the error.
    uint8_t streak = 0;
    for (int i = 0; i < 10; ++i) updateStuckStreak(0, streak);
    TEST_ASSERT_TRUE(streak >= ADC_STUCK_STREAK);

    bool err = updateStuckStreak(2000, streak);  // recovery
    TEST_ASSERT_FALSE(err);
    TEST_ASSERT_EQUAL_UINT8(0, streak);

    // First post-recovery stuck read must NOT re-trip.
    err = updateStuckStreak(0, streak);
    TEST_ASSERT_FALSE(err);
    TEST_ASSERT_EQUAL_UINT8(1, streak);
}

void test_stuck_boundary_just_above_low_is_clean(void) {
    // ADC_STUCK_LOW = 50; raw=51 must be considered clean.
    uint8_t streak = 0;
    for (int i = 0; i < 5; ++i) {
        bool err = updateStuckStreak(51, streak);
        TEST_ASSERT_FALSE(err);
    }
    TEST_ASSERT_EQUAL_UINT8(0, streak);
}

void test_stuck_boundary_just_below_high_is_clean(void) {
    // ADC_STUCK_HIGH = 4045; raw=4044 must be considered clean.
    uint8_t streak = 0;
    for (int i = 0; i < 5; ++i) {
        bool err = updateStuckStreak(4044, streak);
        TEST_ASSERT_FALSE(err);
    }
    TEST_ASSERT_EQUAL_UINT8(0, streak);
}

void test_stuck_streak_saturates_at_255(void) {
    // Counter must not wrap to 0 under prolonged failure — that
    // would cause the FSM to oscillate between latched/cleared on
    // a permanently stuck sensor, masking the fault.
    uint8_t streak = 250;
    for (int i = 0; i < 20; ++i) updateStuckStreak(0, streak);
    TEST_ASSERT_EQUAL_UINT8(255, streak);
    TEST_ASSERT_TRUE(updateStuckStreak(0, streak));
    TEST_ASSERT_EQUAL_UINT8(255, streak);
}

// ---------- isLockoutActive (millis-rollover safety) ----------
//
// The runtime cooldown deadline is `now + LOCKOUT_RUNTIME_COOLDOWN_MS`.
// On a real ESP32, millis() wraps every ~49.7 days. A naive
// `now < deadline` (or `now >= deadline`) silently inverts across
// that wrap: a deadline stored just before wrap appears in the
// "past" once `now` catches up — releasing a runtime-tripped pump
// up to 49 days early — or a deadline stored just after a wrap
// can outrank a live `now` for nearly the full cycle.
//
// These tests pin the rollover-safe behaviour at the boundary
// values that production code is most likely to hit.

void test_lockout_zero_means_inactive(void) {
    // 0 is the "no cooldown" sentinel. PumpState is zero-initialised
    // before relaysBegin runs, so this case fires every boot.
    TEST_ASSERT_FALSE(isLockoutActive(0,           0));
    TEST_ASSERT_FALSE(isLockoutActive(123,         0));
    TEST_ASSERT_FALSE(isLockoutActive(0xFFFFFFFFu, 0));
}

void test_lockout_normal_active(void) {
    // No wrap involved: deadline 5 min after now.
    uint32_t now      = 100000u;
    uint32_t deadline = now + 300000u;  // LOCKOUT_RUNTIME_COOLDOWN_MS
    TEST_ASSERT_TRUE(isLockoutActive(now,           deadline));
    TEST_ASSERT_TRUE(isLockoutActive(deadline - 1,  deadline));
    // At the exact deadline, the cooldown has elapsed.
    TEST_ASSERT_FALSE(isLockoutActive(deadline,     deadline));
    TEST_ASSERT_FALSE(isLockoutActive(deadline + 1, deadline));
    TEST_ASSERT_FALSE(isLockoutActive(deadline + 100000u, deadline));
}

void test_lockout_active_when_deadline_wraps_below_now(void) {
    // The realistic failure case: trip happens just before millis()
    // wraps, so deadline = now + 300_000 wraps below now.
    uint32_t now      = 0xFFFE0000u;
    uint32_t deadline = now + 300000u;        // wraps to 0x000273E0
    TEST_ASSERT_TRUE(deadline < now);          // sanity: did wrap
    // Naive `now < deadline` would say FALSE here. The helper must
    // recognise the deadline is in the (post-wrap) future.
    TEST_ASSERT_TRUE(isLockoutActive(now, deadline));
    // Continue past now — still locked while we have not crossed
    // the wrap point.
    TEST_ASSERT_TRUE(isLockoutActive(0xFFFFF000u, deadline));
    // After the wrap, before reaching the (post-wrap) deadline.
    TEST_ASSERT_TRUE(isLockoutActive(0x00010000u, deadline));
    // Exactly at the deadline → expired.
    TEST_ASSERT_FALSE(isLockoutActive(deadline, deadline));
    // After the deadline → expired.
    TEST_ASSERT_FALSE(isLockoutActive(deadline + 1, deadline));
}

void test_lockout_expired_when_now_wraps_past_deadline(void) {
    // Deadline set just before wrap (no actual wrap when stored).
    // Then `now` advances past it normally.
    uint32_t deadline = 0xFFFFFF00u;
    TEST_ASSERT_FALSE(isLockoutActive(0xFFFFFF00u, deadline));
    TEST_ASSERT_TRUE (isLockoutActive(0xFFFFFE00u, deadline));
    // Once now wraps past deadline by less than 2^31 we must report
    // expired — not "locked for another 49 days".
    TEST_ASSERT_FALSE(isLockoutActive(0x00000001u, deadline));
    TEST_ASSERT_FALSE(isLockoutActive(0x10000000u, deadline));
}

void test_lockout_at_max_uint32(void) {
    // Boundary: deadline at the very top of uint32_t.
    uint32_t deadline = 0xFFFFFFFFu;
    TEST_ASSERT_TRUE (isLockoutActive(0xFFFFFFFEu, deadline));
    TEST_ASSERT_FALSE(isLockoutActive(0xFFFFFFFFu, deadline));
    // After wrap, the helper considers the deadline already in the
    // past (we have just passed it).
    TEST_ASSERT_FALSE(isLockoutActive(0x00000000u, deadline));
}

// ---------- computeDeadline (rollover-safe deadline construction) ----------

void test_compute_deadline_normal(void) {
    TEST_ASSERT_EQUAL_UINT32(1500u, computeDeadline(1000u, 500u));
}

void test_compute_deadline_avoids_zero_sentinel(void) {
    // 0xFFFFFFF0 + 0x10 wraps exactly to 0; helper bumps to 1 so
    // isLockoutActive does not interpret it as "no timer".
    uint32_t d = computeDeadline(0xFFFFFFF0u, 0x10u);
    TEST_ASSERT_EQUAL_UINT32(1u, d);
    TEST_ASSERT_TRUE(isLockoutActive(0xFFFFFFF0u, d));
}

void test_compute_deadline_zero_offset(void) {
    // Edge case: caller passed a 0 offset (no real timer). We still
    // produce a non-zero deadline so an immediate isLockoutActive
    // check stays consistent — but in practice callers should not
    // arm a timer with offset=0.
    uint32_t d = computeDeadline(0u, 0u);
    TEST_ASSERT_EQUAL_UINT32(1u, d);
}

// ---------- rawToPct (endpoint-aware calibration) ----------

void test_rawToPct_water_endpoints_default(void) {
    // Default water cal: dry=0, wet=4095.
    TEST_ASSERT_EQUAL_UINT8(0,   rawToPct(0,    0, 4095));
    TEST_ASSERT_EQUAL_UINT8(100, rawToPct(4095, 0, 4095));
    TEST_ASSERT_EQUAL_UINT8(50,  rawToPct(2048, 0, 4095));
}

void test_rawToPct_soil_inverted_endpoints(void) {
    // Soil cal: dry=4095, wet=0 (inverted orientation).
    TEST_ASSERT_EQUAL_UINT8(0,   rawToPct(4095, 4095, 0));
    TEST_ASSERT_EQUAL_UINT8(100, rawToPct(0,    4095, 0));
    // Midpoint: 2047 → ~50%.
    uint8_t v = rawToPct(2048, 4095, 0);
    TEST_ASSERT_TRUE(v == 49 || v == 50 || v == 51);
}

void test_rawToPct_partial_endpoints(void) {
    // Operator-calibrated tank: dry=200 (sensor reads 200 when air),
    // wet=3800. Mid-band.
    uint8_t v = rawToPct(2000, 200, 3800);
    // (2000-200) / (3800-200) = 1800/3600 = 50%.
    TEST_ASSERT_EQUAL_UINT8(50, v);
    // Below dry endpoint saturates at 0.
    TEST_ASSERT_EQUAL_UINT8(0,   rawToPct(100,  200, 3800));
    // Above wet endpoint saturates at 100.
    TEST_ASSERT_EQUAL_UINT8(100, rawToPct(4000, 200, 3800));
}

void test_rawToPct_degenerate_span_returns_zero(void) {
    // Span < MIN_CAL_SPAN: helper must NOT divide by a tiny span and
    // produce wild percentages. It returns 0 to fail safe.
    TEST_ASSERT_EQUAL_UINT8(0, rawToPct(2000, 1000, 1010));
    TEST_ASSERT_EQUAL_UINT8(0, rawToPct(2000, 2000, 2000));
}

// ---------- applyCalibrationPatch (validate-then-apply) ----------

static Settings make_cal_baseline(void) {
    Settings s;
    settingsLoadDefaults(s);
    return s;
}

void test_calpatch_empty_is_noop(void) {
    Settings s = make_cal_baseline();
    Settings before = s;
    CalibrationPatch p;
    auto r = applyCalibrationPatch(p, s);
    TEST_ASSERT_EQUAL((int)CalibrationPatchResult::Ok, (int)r);
    TEST_ASSERT_EQUAL_UINT16(before.tank1RawDry, s.tank1RawDry);
    TEST_ASSERT_EQUAL_UINT16(before.soilRawWet,  s.soilRawWet);
}

void test_calpatch_applies_full_pair(void) {
    Settings s = make_cal_baseline();
    CalibrationPatch p;
    p.hasTank1Dry = true; p.tank1Dry = 150;
    p.hasTank1Wet = true; p.tank1Wet = 3900;
    auto r = applyCalibrationPatch(p, s);
    TEST_ASSERT_EQUAL((int)CalibrationPatchResult::Ok, (int)r);
    TEST_ASSERT_EQUAL_UINT16(150,  s.tank1RawDry);
    TEST_ASSERT_EQUAL_UINT16(3900, s.tank1RawWet);
}

void test_calpatch_rejects_out_of_range(void) {
    Settings s = make_cal_baseline();
    Settings before = s;
    CalibrationPatch p;
    p.hasSoilDry = true; p.soilDry = 5000;  // > ADC_MAX_RAW
    auto r = applyCalibrationPatch(p, s);
    TEST_ASSERT_EQUAL((int)CalibrationPatchResult::OutOfRange, (int)r);
    // Untouched on rejection.
    TEST_ASSERT_EQUAL_UINT16(before.soilRawDry, s.soilRawDry);
}

void test_calpatch_rejects_small_span(void) {
    Settings s = make_cal_baseline();
    Settings before = s;
    CalibrationPatch p;
    p.hasTank2Dry = true; p.tank2Dry = 1000;
    p.hasTank2Wet = true; p.tank2Wet = 1100;  // span 100 < 200
    auto r = applyCalibrationPatch(p, s);
    TEST_ASSERT_EQUAL((int)CalibrationPatchResult::SpanTooSmall, (int)r);
    TEST_ASSERT_EQUAL_UINT16(before.tank2RawDry, s.tank2RawDry);
    TEST_ASSERT_EQUAL_UINT16(before.tank2RawWet, s.tank2RawWet);
}

void test_calpatch_rejects_partial_change_breaking_span(void) {
    // Only `dry` is patched; the resulting (new dry, existing wet)
    // pair has span < MIN_CAL_SPAN. Must be rejected so we never
    // leave the device with an unusable calibration after a
    // partial-update request.
    Settings s = make_cal_baseline();
    s.tank1RawDry = 0; s.tank1RawWet = 4095;
    Settings before = s;
    CalibrationPatch p;
    p.hasTank1Dry = true; p.tank1Dry = 4000;  // span vs 4095 = 95
    auto r = applyCalibrationPatch(p, s);
    TEST_ASSERT_EQUAL((int)CalibrationPatchResult::SpanTooSmall, (int)r);
    TEST_ASSERT_EQUAL_UINT16(before.tank1RawDry, s.tank1RawDry);
}

void test_calpatch_inverted_pair_is_legal(void) {
    // dry > wet (the soil orientation): perfectly valid as long as
    // |dry - wet| ≥ MIN_CAL_SPAN.
    Settings s = make_cal_baseline();
    CalibrationPatch p;
    p.hasSoilDry = true; p.soilDry = 4000;
    p.hasSoilWet = true; p.soilWet = 100;
    auto r = applyCalibrationPatch(p, s);
    TEST_ASSERT_EQUAL((int)CalibrationPatchResult::Ok, (int)r);
    TEST_ASSERT_EQUAL_UINT16(4000, s.soilRawDry);
    TEST_ASSERT_EQUAL_UINT16(100,  s.soilRawWet);
}

void test_calpatch_error_text_matches_api_contract(void) {
    TEST_ASSERT_EQUAL_STRING("ok",
        calibrationPatchErrorText(CalibrationPatchResult::Ok));
    TEST_ASSERT_EQUAL_STRING("raw_out_of_range",
        calibrationPatchErrorText(CalibrationPatchResult::OutOfRange));
    TEST_ASSERT_EQUAL_STRING("span_too_small",
        calibrationPatchErrorText(CalibrationPatchResult::SpanTooSmall));
}

void test_calpatch_soil2_applies(void) {
    Settings s = make_cal_baseline();
    CalibrationPatch p;
    p.hasSoil2Dry = true; p.soil2Dry = 3800;
    p.hasSoil2Wet = true; p.soil2Wet = 200;
    auto r = applyCalibrationPatch(p, s);
    TEST_ASSERT_EQUAL((int)CalibrationPatchResult::Ok, (int)r);
    TEST_ASSERT_EQUAL_UINT16(3800, s.soil2RawDry);
    TEST_ASSERT_EQUAL_UINT16(200,  s.soil2RawWet);
}

void test_calpatch_soil2_rejects_small_span(void) {
    Settings s = make_cal_baseline();
    CalibrationPatch p;
    p.hasSoil2Dry = true; p.soil2Dry = 2000;
    p.hasSoil2Wet = true; p.soil2Wet = 2100;  // span 100 < 200
    auto r = applyCalibrationPatch(p, s);
    TEST_ASSERT_EQUAL((int)CalibrationPatchResult::SpanTooSmall, (int)r);
}

// ---------- settingsClamp recovers degenerate calibration ----------

void test_clamp_resets_degenerate_calibration_to_defaults(void) {
    Settings s;
    settingsLoadDefaults(s);
    s.soilRawDry = 1000;
    s.soilRawWet = 1050;  // span 50
    bool changed = settingsClamp(s);
    TEST_ASSERT_TRUE(changed);
    TEST_ASSERT_EQUAL_UINT16(DEFAULT_SOIL_RAW_DRY, s.soilRawDry);
    TEST_ASSERT_EQUAL_UINT16(DEFAULT_SOIL_RAW_WET, s.soilRawWet);
}

void test_clamp_resets_degenerate_soil2_calibration(void) {
    Settings s;
    settingsLoadDefaults(s);
    s.soil2RawDry = 500;
    s.soil2RawWet = 600;  // span 100 < 200
    bool changed = settingsClamp(s);
    TEST_ASSERT_TRUE(changed);
    TEST_ASSERT_EQUAL_UINT16(DEFAULT_SOIL_RAW_DRY, s.soil2RawDry);
    TEST_ASSERT_EQUAL_UINT16(DEFAULT_SOIL_RAW_WET, s.soil2RawWet);
}

// ---------- Event ring buffer (Build 3 observability) ----------
//
// EventLog is the bounded tail of recent system events surfaced via
// `/api/events` and the dashboard. The tests cover three things:
//   1. Ordering — eventLogGet(0) returns the oldest, count-1 the newest.
//   2. Overwrite — past CAPACITY entries, the oldest gets evicted.
//   3. Monotonic counter — `total` keeps incrementing on every add,
//      independently of overwrite, so it can be used as a
//      `Metrics.eventCount` proxy.

void test_eventlog_init_is_empty(void) {
    EventLog log;
    eventLogInit(log);
    TEST_ASSERT_EQUAL_UINT16(0, log.count);
    TEST_ASSERT_EQUAL_UINT16(0, log.head);
    TEST_ASSERT_EQUAL_UINT32(0, log.total);
    TEST_ASSERT_NULL(eventLogGet(log, 0));
}

void test_eventlog_add_orders_oldest_first(void) {
    EventLog log;
    eventLogInit(log);
    eventLogAdd(log, 100, EventLevel::Info,  "first");
    eventLogAdd(log, 200, EventLevel::Warn,  "second");
    eventLogAdd(log, 300, EventLevel::Error, "third");
    TEST_ASSERT_EQUAL_UINT16(3, log.count);
    TEST_ASSERT_EQUAL_UINT32(3, log.total);

    const EventEntry* a = eventLogGet(log, 0);
    const EventEntry* b = eventLogGet(log, 1);
    const EventEntry* c = eventLogGet(log, 2);
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_EQUAL_STRING("first",  a->msg);
    TEST_ASSERT_EQUAL_STRING("second", b->msg);
    TEST_ASSERT_EQUAL_STRING("third",  c->msg);
    TEST_ASSERT_EQUAL_UINT32(0, a->sequence);
    TEST_ASSERT_EQUAL_UINT32(1, b->sequence);
    TEST_ASSERT_EQUAL_UINT32(2, c->sequence);
    TEST_ASSERT_EQUAL_INT((int)EventLevel::Info,  (int)a->level);
    TEST_ASSERT_EQUAL_INT((int)EventLevel::Warn,  (int)b->level);
    TEST_ASSERT_EQUAL_INT((int)EventLevel::Error, (int)c->level);
}

void test_eventlog_overflow_evicts_oldest(void) {
    EventLog log;
    eventLogInit(log);
    // Fill exactly CAPACITY entries.
    for (uint32_t i = 0; i < EVENTLOG_CAPACITY; ++i) {
        char msg[16];
        snprintf(msg, sizeof(msg), "e%u", (unsigned)i);
        eventLogAdd(log, i * 10, EventLevel::Info, msg);
    }
    TEST_ASSERT_EQUAL_UINT16(EVENTLOG_CAPACITY, log.count);
    // Newest is "e{CAPACITY-1}".
    const EventEntry* newest = eventLogGet(log, EVENTLOG_CAPACITY - 1);
    TEST_ASSERT_NOT_NULL(newest);
    char expected[16];
    snprintf(expected, sizeof(expected), "e%u",
             (unsigned)(EVENTLOG_CAPACITY - 1));
    TEST_ASSERT_EQUAL_STRING(expected, newest->msg);

    // Add 5 more — first 5 should be evicted.
    for (uint32_t i = 0; i < 5; ++i) {
        char msg[16];
        snprintf(msg, sizeof(msg), "x%u", (unsigned)i);
        eventLogAdd(log, 1000 + i, EventLevel::Warn, msg);
    }
    TEST_ASSERT_EQUAL_UINT16(EVENTLOG_CAPACITY, log.count);
    TEST_ASSERT_EQUAL_UINT32(EVENTLOG_CAPACITY + 5, log.total);

    // Oldest visible entry is now "e5" (the original "e0"–"e4"
    // were evicted by the 5 new appends).
    const EventEntry* oldest = eventLogGet(log, 0);
    TEST_ASSERT_NOT_NULL(oldest);
    TEST_ASSERT_EQUAL_STRING("e5", oldest->msg);
    // Sequence is preserved across eviction.
    TEST_ASSERT_EQUAL_UINT32(5, oldest->sequence);

    // Newest visible entry is "x4".
    const EventEntry* newestNow = eventLogGet(log, EVENTLOG_CAPACITY - 1);
    TEST_ASSERT_NOT_NULL(newestNow);
    TEST_ASSERT_EQUAL_STRING("x4", newestNow->msg);
    TEST_ASSERT_EQUAL_UINT32(EVENTLOG_CAPACITY + 5 - 1, newestNow->sequence);
}

void test_eventlog_total_is_monotonic_under_eviction(void) {
    EventLog log;
    eventLogInit(log);
    // Push 3 × CAPACITY entries; verify `total` strictly increases
    // and never wraps.
    uint32_t prevTotal = log.total;
    for (uint32_t i = 0; i < EVENTLOG_CAPACITY * 3; ++i) {
        eventLogAdd(log, i, EventLevel::Info, "x");
        TEST_ASSERT_TRUE(log.total > prevTotal);
        prevTotal = log.total;
    }
    TEST_ASSERT_EQUAL_UINT32(EVENTLOG_CAPACITY * 3, log.total);
    TEST_ASSERT_EQUAL_UINT16(EVENTLOG_CAPACITY, log.count);
}

void test_eventlog_truncates_long_message(void) {
    EventLog log;
    eventLogInit(log);
    // EVENTLOG_MSG_LEN-1 chars worth of payload, plus 50 extra.
    char big[256];
    memset(big, 'A', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    eventLogAdd(log, 1, EventLevel::Info, big);
    const EventEntry* e = eventLogGet(log, 0);
    TEST_ASSERT_NOT_NULL(e);
    // Stored string must be NUL-terminated within the buffer and
    // strictly shorter than EVENTLOG_MSG_LEN.
    TEST_ASSERT_EQUAL_size_t(EVENTLOG_MSG_LEN - 1,
                             strnlen(e->msg, EVENTLOG_MSG_LEN));
    TEST_ASSERT_EQUAL_CHAR('\0', e->msg[EVENTLOG_MSG_LEN - 1]);
}

void test_eventlog_handles_null_message(void) {
    EventLog log;
    eventLogInit(log);
    eventLogAdd(log, 1, EventLevel::Info, nullptr);
    const EventEntry* e = eventLogGet(log, 0);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_STRING("", e->msg);
}

void test_eventlog_get_out_of_range_returns_null(void) {
    EventLog log;
    eventLogInit(log);
    eventLogAdd(log, 1, EventLevel::Info, "only");
    TEST_ASSERT_NOT_NULL(eventLogGet(log, 0));
    TEST_ASSERT_NULL(eventLogGet(log, 1));
    TEST_ASSERT_NULL(eventLogGet(log, EVENTLOG_CAPACITY));
}

void test_eventlog_level_text_stable(void) {
    // These strings are part of the API contract for /api/events.
    TEST_ASSERT_EQUAL_STRING("info",  eventLevelText(EventLevel::Info));
    TEST_ASSERT_EQUAL_STRING("warn",  eventLevelText(EventLevel::Warn));
    TEST_ASSERT_EQUAL_STRING("error", eventLevelText(EventLevel::Error));
}

// /api/events caps `?limit=` at 16 entries to keep the response
// inside its 4 KB JSON document budget. Mirror the firmware's
// tail-walk: fill the ring with EVENTLOG_CAPACITY (32) max-length
// messages, then read the last 16 entries oldest-first. The test
// asserts (a) every read returns a non-null entry, (b) sequence
// numbers are contiguous and ascending, and (c) the message
// payload survives truncation at EVENTLOG_MSG_LEN-1 chars.
void test_eventlog_tail_limit16_on_full_ring(void) {
    EventLog log;
    eventLogInit(log);

    // Fill the ring with 32 max-length payloads. Each `msg` is
    // EVENTLOG_MSG_LEN-1 'A' chars + a trailing index digit, so the
    // stored entry is exactly EVENTLOG_MSG_LEN-1 chars long after
    // truncation — the worst case for the JSON budget.
    char big[EVENTLOG_MSG_LEN + 16];
    for (uint32_t i = 0; i < EVENTLOG_CAPACITY; ++i) {
        memset(big, 'A', sizeof(big) - 1);
        big[sizeof(big) - 1] = '\0';
        // Stamp the last byte with a per-entry marker so we can verify
        // the (truncated) tail. EVENTLOG_MSG_LEN-2 is the last writable
        // index before the implicit NUL terminator.
        big[EVENTLOG_MSG_LEN - 2] = (char)('a' + (i % 26));
        eventLogAdd(log, i * 10, EventLevel::Info, big);
    }
    TEST_ASSERT_EQUAL_UINT16(EVENTLOG_CAPACITY, log.count);
    TEST_ASSERT_EQUAL_UINT32(EVENTLOG_CAPACITY, log.total);

    // Walk the oldest 16-of-32 just like handleGetEvents does:
    //   startIdx = count - limit; iterate through count exclusive.
    constexpr int LIMIT = 16;
    size_t startIdx = log.count - (size_t)LIMIT;
    uint32_t expectedSeq = (uint32_t)startIdx;  // sequences are 0..count-1
    for (size_t i = startIdx; i < log.count; ++i) {
        const EventEntry* e = eventLogGet(log, i);
        TEST_ASSERT_NOT_NULL(e);
        TEST_ASSERT_EQUAL_UINT32(expectedSeq, e->sequence);
        // Truncated string: NUL-terminated within the buffer, length
        // exactly EVENTLOG_MSG_LEN-1.
        TEST_ASSERT_EQUAL_size_t(EVENTLOG_MSG_LEN - 1,
                                 strnlen(e->msg, EVENTLOG_MSG_LEN));
        TEST_ASSERT_EQUAL_CHAR('\0', e->msg[EVENTLOG_MSG_LEN - 1]);
        // Per-entry marker survived truncation.
        TEST_ASSERT_EQUAL_CHAR((char)('a' + (i % 26)),
                               e->msg[EVENTLOG_MSG_LEN - 2]);
        expectedSeq++;
    }
    // The walk must have produced exactly LIMIT readings without
    // running off the end.
    TEST_ASSERT_EQUAL_UINT32((uint32_t)log.count, expectedSeq);
}

// ---------- Metrics counters (Build 3 observability) ----------
//
// The metrics struct uses saturating uint32 counters so a long-
// running deployment never wraps a 32-bit "total runtime" or
// "request count" silently. These tests pin the saturation
// behaviour at the rail and verify monotonicity under repeated
// increments.

void test_metrics_init_zeros_everything(void) {
    Metrics m;
    metricsInit(m);
    TEST_ASSERT_EQUAL_UINT32(0, m.bootMs);
    TEST_ASSERT_EQUAL_UINT32(0, m.mainLoopTicks);
    TEST_ASSERT_EQUAL_UINT32(0, m.sensorErrorCount);
    TEST_ASSERT_EQUAL_UINT32(0, m.pumpStarts[0]);
    TEST_ASSERT_EQUAL_UINT32(0, m.pumpStarts[1]);
    TEST_ASSERT_EQUAL_UINT32(0, m.pumpRuntimeSec[0]);
    TEST_ASSERT_EQUAL_UINT32(0, m.pumpRuntimeSec[1]);
    TEST_ASSERT_EQUAL_UINT32(0, m.apRestarts);
    TEST_ASSERT_EQUAL_UINT32(0, m.httpRequests);
    TEST_ASSERT_EQUAL_UINT32(0, m.httpErrors4xx);
    TEST_ASSERT_EQUAL_UINT32(0, m.httpErrors5xx);
    TEST_ASSERT_EQUAL_UINT32(0, m.freeHeapMin);
}

void test_metrics_inc_basic_monotonic(void) {
    uint32_t c = 0;
    for (uint32_t i = 1; i <= 10; ++i) {
        uint32_t after = metricsIncSat(c, 1);
        TEST_ASSERT_EQUAL_UINT32(i, c);
        TEST_ASSERT_EQUAL_UINT32(i, after);
    }
}

void test_metrics_inc_saturates_at_max(void) {
    uint32_t c = UINT32_MAX - 2;
    metricsIncSat(c, 1);  // -> MAX-1
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX - 1, c);
    metricsIncSat(c, 1);  // -> MAX
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, c);
    // Critical: must NOT wrap to 0 here.
    metricsIncSat(c, 1);
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, c);
    metricsIncSat(c, 1000);
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, c);
}

void test_metrics_inc_saturates_when_increment_overflows(void) {
    // Pre-existing counter mid-range, but the increment itself is
    // huge enough to wrap. Saturation must still kick in.
    uint32_t c = UINT32_MAX / 2 + 100;
    metricsIncSat(c, UINT32_MAX);
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, c);
}

void test_metrics_add_sat_normal(void) {
    uint32_t c = 100;
    uint32_t after = metricsAddSat(c, 250);
    TEST_ASSERT_EQUAL_UINT32(350, c);
    TEST_ASSERT_EQUAL_UINT32(350, after);
}

void test_metrics_add_sat_saturates(void) {
    uint32_t c = UINT32_MAX - 100;
    metricsAddSat(c, 50);
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX - 50, c);
    metricsAddSat(c, 1000);  // would overflow
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, c);
}

void test_metrics_inc_zero_is_noop(void) {
    uint32_t c = 42;
    metricsIncSat(c, 0);
    TEST_ASSERT_EQUAL_UINT32(42, c);
}

// ---------- Token bucket (Build 4 rate limiter) ----------
//
// The rate limiter uses ms-based integer refill so its arithmetic is
// deterministic in unit tests. The tests pin:
//   - Initial state is FULL (first request always allowed).
//   - tryConsume decrements and rejects when empty.
//   - Refill cadence: one token every `msPerToken` of elapsed time.
//   - Sub-token elapsed time accumulates via pendingMs (no token
//     starvation under fine-grained polling).
//   - Saturation at capacity (idle bucket cannot store unlimited
//     credit).
//   - Rollover-safe: a backwards `nowMs` does not deliver a wrap-
//     around fortune of tokens.
//   - retryAfterSec: reports a non-zero, ≤3600 hint when blocked.

void test_bucket_init_is_full(void) {
    TokenBucket b;
    tokenBucketInit(b, /*capacity*/ 5, /*msPerToken*/ 1000, /*now*/ 100);
    TEST_ASSERT_EQUAL_UINT32(5,    b.capacity);
    TEST_ASSERT_EQUAL_UINT32(5,    b.tokens);
    TEST_ASSERT_EQUAL_UINT32(1000, b.msPerToken);
    TEST_ASSERT_EQUAL_UINT32(0,    b.pendingMs);
    TEST_ASSERT_EQUAL_UINT32(100,  b.lastRefillMs);
}

void test_bucket_init_clamps_zero_capacity_and_rate(void) {
    TokenBucket b;
    tokenBucketInit(b, 0, 0, 0);
    TEST_ASSERT_TRUE(b.capacity   >= 1);
    TEST_ASSERT_TRUE(b.msPerToken >= 1);
    TEST_ASSERT_EQUAL_UINT32(b.capacity, b.tokens);
}

void test_bucket_consume_decrements(void) {
    TokenBucket b;
    tokenBucketInit(b, 3, 1000, 0);
    TEST_ASSERT_TRUE (tokenBucketTryConsume(b, 1, 0));
    TEST_ASSERT_TRUE (tokenBucketTryConsume(b, 1, 0));
    TEST_ASSERT_TRUE (tokenBucketTryConsume(b, 1, 0));
    // Bucket empty now; next consume must fail.
    TEST_ASSERT_FALSE(tokenBucketTryConsume(b, 1, 0));
    TEST_ASSERT_EQUAL_UINT32(0, b.tokens);
}

void test_bucket_refill_after_one_period(void) {
    TokenBucket b;
    tokenBucketInit(b, 5, 1000, 0);
    // Drain the bucket.
    for (int i = 0; i < 5; ++i) tokenBucketTryConsume(b, 1, 0);
    TEST_ASSERT_EQUAL_UINT32(0, b.tokens);
    // 1000 ms later: exactly one token must be available.
    tokenBucketRefill(b, 1000);
    TEST_ASSERT_EQUAL_UINT32(1, b.tokens);
    TEST_ASSERT_EQUAL_UINT32(0, b.pendingMs);
}

void test_bucket_refill_partial_period_accumulates(void) {
    TokenBucket b;
    tokenBucketInit(b, 5, 1000, 0);
    // Drain.
    for (int i = 0; i < 5; ++i) tokenBucketTryConsume(b, 1, 0);
    // 700 ms later — no full token yet, but pendingMs grows.
    tokenBucketRefill(b, 700);
    TEST_ASSERT_EQUAL_UINT32(0,   b.tokens);
    TEST_ASSERT_EQUAL_UINT32(700, b.pendingMs);
    // Another 400 ms → total 1100 ms → one token + 100 ms pending.
    tokenBucketRefill(b, 1100);
    TEST_ASSERT_EQUAL_UINT32(1,   b.tokens);
    TEST_ASSERT_EQUAL_UINT32(100, b.pendingMs);
}

void test_bucket_refill_saturates_at_capacity(void) {
    TokenBucket b;
    tokenBucketInit(b, 3, 1000, 0);
    // Drain.
    for (int i = 0; i < 3; ++i) tokenBucketTryConsume(b, 1, 0);
    // 60 s of idle time would generate 60 tokens — bucket must
    // saturate at capacity, not accumulate to 60.
    tokenBucketRefill(b, 60000);
    TEST_ASSERT_EQUAL_UINT32(3, b.tokens);
}

void test_bucket_refill_rollover_safe(void) {
    TokenBucket b;
    // last = near top of uint32_t.
    tokenBucketInit(b, 5, 1000, 0xFFFFF000u);
    for (int i = 0; i < 5; ++i) tokenBucketTryConsume(b, 1, 0xFFFFF000u);
    TEST_ASSERT_EQUAL_UINT32(0, b.tokens);
    // Wrap: now = 0x00001000 (≈ 8 s after last). Signed-diff math
    // must treat this as +8000 ms, not as a huge negative or huge
    // positive value.
    tokenBucketRefill(b, 0x00001000u);
    // Elapsed: 0x00001000 - 0xFFFFF000 (uint32 wrap) = 0x2000 = 8192 ms.
    // 8192 / 1000 = 8 tokens, but capacity caps at 5.
    TEST_ASSERT_EQUAL_UINT32(5, b.tokens);
}

void test_bucket_refill_backwards_time_is_zero_elapsed(void) {
    TokenBucket b;
    tokenBucketInit(b, 5, 1000, 1000);
    for (int i = 0; i < 5; ++i) tokenBucketTryConsume(b, 1, 1000);
    // now < lastRefillMs by a small amount: signed diff is negative,
    // so we must treat as zero elapsed (no free tokens).
    tokenBucketRefill(b, 500);
    TEST_ASSERT_EQUAL_UINT32(0, b.tokens);
    // lastRefillMs advances to the new "now" so later positive deltas
    // are computed from this anchor.
    TEST_ASSERT_EQUAL_UINT32(500, b.lastRefillMs);
}

void test_bucket_consume_burst_then_refill(void) {
    TokenBucket b;
    tokenBucketInit(b, 10, 1000, 0);
    // Burst: drain 10 tokens at t=0.
    for (int i = 0; i < 10; ++i) {
        TEST_ASSERT_TRUE(tokenBucketTryConsume(b, 1, 0));
    }
    TEST_ASSERT_FALSE(tokenBucketTryConsume(b, 1, 0));
    // 5500 ms later: 5 full tokens + 500 ms pending.
    TEST_ASSERT_TRUE(tokenBucketTryConsume(b, 1, 5500));
    TEST_ASSERT_TRUE(tokenBucketTryConsume(b, 1, 5500));
    TEST_ASSERT_TRUE(tokenBucketTryConsume(b, 1, 5500));
    TEST_ASSERT_TRUE(tokenBucketTryConsume(b, 1, 5500));
    TEST_ASSERT_TRUE(tokenBucketTryConsume(b, 1, 5500));
    TEST_ASSERT_FALSE(tokenBucketTryConsume(b, 1, 5500));  // 6th — empty
}

void test_bucket_retry_after_sec_zero_when_available(void) {
    TokenBucket b;
    tokenBucketInit(b, 5, 1000, 0);
    TEST_ASSERT_EQUAL_UINT32(0, tokenBucketRetryAfterSec(b, 1));
}

void test_bucket_retry_after_sec_one_when_empty_one_per_sec(void) {
    TokenBucket b;
    tokenBucketInit(b, 1, 1000, 0);
    tokenBucketTryConsume(b, 1, 0);
    // Empty bucket, 1 token/sec refill: full token in ~1 s. Result
    // is rounded up and clamped ≥ 1.
    uint32_t s = tokenBucketRetryAfterSec(b, 1);
    TEST_ASSERT_TRUE(s >= 1 && s <= 2);
}

void test_bucket_retry_after_sec_clamped_at_3600(void) {
    TokenBucket b;
    tokenBucketInit(b, 1, 1000UL * 24UL * 3600UL, 0);  // one token / day
    tokenBucketTryConsume(b, 1, 0);
    uint32_t s = tokenBucketRetryAfterSec(b, 1);
    TEST_ASSERT_TRUE(s <= 3600);
    TEST_ASSERT_TRUE(s >= 1);
}

void test_bucket_retry_after_accounts_for_pending_ms(void) {
    TokenBucket b;
    tokenBucketInit(b, 1, 5000, 0);
    tokenBucketTryConsume(b, 1, 0);          // empty
    tokenBucketRefill(b, 2000);              // 2000 ms pending, 0 tokens
    // Cost 1: needs (5000 - 2000) = 3000 ms = 3 s.
    uint32_t s = tokenBucketRetryAfterSec(b, 1);
    TEST_ASSERT_EQUAL_UINT32(3, s);
}

void test_bucket_retry_after_for_multi_token_cost(void) {
    TokenBucket b;
    tokenBucketInit(b, 3, 1000, 0);
    for (int i = 0; i < 3; ++i) tokenBucketTryConsume(b, 1, 0);
    // Cost 3 tokens, all pending. Need ~3 s.
    uint32_t s = tokenBucketRetryAfterSec(b, 3);
    TEST_ASSERT_TRUE(s >= 3 && s <= 4);
}

int main(int /*argc*/, char** /*argv*/) {
    UNITY_BEGIN();
    RUN_TEST(test_water_zero_is_zero);
    RUN_TEST(test_water_full_is_hundred);
    RUN_TEST(test_water_midpoint);
    RUN_TEST(test_water_saturates_above_max);

    RUN_TEST(test_soil_dry_is_zero);
    RUN_TEST(test_soil_wet_is_hundred);
    RUN_TEST(test_soil_midpoint);

    RUN_TEST(test_defaults_are_sane);
    RUN_TEST(test_clamp_fixes_threshold_overflow);
    RUN_TEST(test_clamp_fixes_runtime_too_short);
    RUN_TEST(test_clamp_fixes_runtime_too_long);
    RUN_TEST(test_clamp_replaces_empty_ssid_with_default);
    RUN_TEST(test_clamp_allows_empty_password_open_ap);
    RUN_TEST(test_clamp_rejects_short_password);

    RUN_TEST(test_pump_on_granted_when_safe);
    RUN_TEST(test_pump_on_denied_dry_run);
    RUN_TEST(test_pump_on_denied_at_minimum_boundary_inclusive);
    RUN_TEST(test_pump_on_denied_when_tank_sensor_error);
    RUN_TEST(test_pump_on_denied_when_runtime_locked);
    RUN_TEST(test_pump_on_denied_when_dry_run_locked);
    RUN_TEST(test_pump_on_dry_run_takes_priority_over_runtime_lockout);
    RUN_TEST(test_pump_on_denied_when_fsm_lockout_sensor_without_live_err);
    RUN_TEST(test_pump_on_live_sensor_err_outranks_runtime_lockout);

    RUN_TEST(test_auto_demand_on_when_dry);
    RUN_TEST(test_auto_demand_off_when_well_above_threshold);
    RUN_TEST(test_auto_demand_holds_in_hysteresis_band);
    RUN_TEST(test_auto_demand_off_on_sensor_error);
    RUN_TEST(test_auto_demand_handles_high_threshold_overflow);

    RUN_TEST(test_pump_fsm_text_off);
    RUN_TEST(test_pump_fsm_text_on);
    RUN_TEST(test_pump_fsm_text_lockouts);
    RUN_TEST(test_pump_result_text_basic);
    RUN_TEST(test_pump_result_text_busy_distinct_from_invalid);

    RUN_TEST(test_compose_error_empty_when_all_clean);
    RUN_TEST(test_compose_error_sensor_takes_priority);
    RUN_TEST(test_compose_error_dry_run_above_runtime);
    RUN_TEST(test_compose_error_runtime_when_only_one);
    RUN_TEST(test_compose_error_dht_only_lowest_priority);
    RUN_TEST(test_compose_error_handles_null_buffer);

    RUN_TEST(test_auto_demand_exactly_at_threshold);
    RUN_TEST(test_auto_demand_just_below_threshold);
    RUN_TEST(test_auto_demand_zero_threshold_is_never_on);
    RUN_TEST(test_pump_evaluate_off_request_helper_unused);
    RUN_TEST(test_clamp_accepts_minimum_legal_password);
    RUN_TEST(test_clamp_accepts_maximum_legal_password);
    RUN_TEST(test_clamp_min_water_overflow);
    RUN_TEST(test_compose_error_truncates_to_buffer);

    RUN_TEST(test_patch_empty_is_noop);
    RUN_TEST(test_patch_ssid_required_rejects_empty);
    RUN_TEST(test_patch_ssid_required_rejects_null);
    RUN_TEST(test_patch_ssid_too_long);
    RUN_TEST(test_patch_pass_too_short);
    RUN_TEST(test_patch_pass_too_long);
    RUN_TEST(test_patch_open_ap_rejected_by_default);
    RUN_TEST(test_patch_open_ap_allowed_with_explicit_flag);
    RUN_TEST(test_patch_pass_sentinel_preserves_current);
    RUN_TEST(test_patch_pass_sentinel_outranks_allow_open);
    RUN_TEST(test_patch_apply_numeric_clamps);
    RUN_TEST(test_patch_apply_changes_ssid_sets_restart);
    RUN_TEST(test_patch_unchanged_ssid_does_not_set_restart);
    RUN_TEST(test_patch_validation_runs_before_apply);
    RUN_TEST(test_patch_error_text_matches_api_contract);

    RUN_TEST(test_stuck_clean_reading_keeps_streak_zero);
    RUN_TEST(test_stuck_low_rail_trips_after_threshold);
    RUN_TEST(test_stuck_high_rail_trips_after_threshold);
    RUN_TEST(test_stuck_streak_resets_on_clean_read);
    RUN_TEST(test_stuck_streak_stays_latched_while_stuck);
    RUN_TEST(test_stuck_streak_clears_after_recovery);
    RUN_TEST(test_stuck_boundary_just_above_low_is_clean);
    RUN_TEST(test_stuck_boundary_just_below_high_is_clean);
    RUN_TEST(test_stuck_streak_saturates_at_255);

    RUN_TEST(test_lockout_zero_means_inactive);
    RUN_TEST(test_lockout_normal_active);
    RUN_TEST(test_lockout_active_when_deadline_wraps_below_now);
    RUN_TEST(test_lockout_expired_when_now_wraps_past_deadline);
    RUN_TEST(test_lockout_at_max_uint32);

    RUN_TEST(test_compute_deadline_normal);
    RUN_TEST(test_compute_deadline_avoids_zero_sentinel);
    RUN_TEST(test_compute_deadline_zero_offset);

    RUN_TEST(test_rawToPct_water_endpoints_default);
    RUN_TEST(test_rawToPct_soil_inverted_endpoints);
    RUN_TEST(test_rawToPct_partial_endpoints);
    RUN_TEST(test_rawToPct_degenerate_span_returns_zero);

    RUN_TEST(test_calpatch_empty_is_noop);
    RUN_TEST(test_calpatch_applies_full_pair);
    RUN_TEST(test_calpatch_rejects_out_of_range);
    RUN_TEST(test_calpatch_rejects_small_span);
    RUN_TEST(test_calpatch_rejects_partial_change_breaking_span);
    RUN_TEST(test_calpatch_inverted_pair_is_legal);
    RUN_TEST(test_calpatch_error_text_matches_api_contract);
    RUN_TEST(test_calpatch_soil2_applies);
    RUN_TEST(test_calpatch_soil2_rejects_small_span);

    RUN_TEST(test_clamp_resets_degenerate_calibration_to_defaults);
    RUN_TEST(test_clamp_resets_degenerate_soil2_calibration);

    RUN_TEST(test_eventlog_init_is_empty);
    RUN_TEST(test_eventlog_add_orders_oldest_first);
    RUN_TEST(test_eventlog_overflow_evicts_oldest);
    RUN_TEST(test_eventlog_total_is_monotonic_under_eviction);
    RUN_TEST(test_eventlog_truncates_long_message);
    RUN_TEST(test_eventlog_handles_null_message);
    RUN_TEST(test_eventlog_get_out_of_range_returns_null);
    RUN_TEST(test_eventlog_level_text_stable);
    RUN_TEST(test_eventlog_tail_limit16_on_full_ring);

    RUN_TEST(test_metrics_init_zeros_everything);
    RUN_TEST(test_metrics_inc_basic_monotonic);
    RUN_TEST(test_metrics_inc_saturates_at_max);
    RUN_TEST(test_metrics_inc_saturates_when_increment_overflows);
    RUN_TEST(test_metrics_add_sat_normal);
    RUN_TEST(test_metrics_add_sat_saturates);
    RUN_TEST(test_metrics_inc_zero_is_noop);

    RUN_TEST(test_bucket_init_is_full);
    RUN_TEST(test_bucket_init_clamps_zero_capacity_and_rate);
    RUN_TEST(test_bucket_consume_decrements);
    RUN_TEST(test_bucket_refill_after_one_period);
    RUN_TEST(test_bucket_refill_partial_period_accumulates);
    RUN_TEST(test_bucket_refill_saturates_at_capacity);
    RUN_TEST(test_bucket_refill_rollover_safe);
    RUN_TEST(test_bucket_refill_backwards_time_is_zero_elapsed);
    RUN_TEST(test_bucket_consume_burst_then_refill);
    RUN_TEST(test_bucket_retry_after_sec_zero_when_available);
    RUN_TEST(test_bucket_retry_after_sec_one_when_empty_one_per_sec);
    RUN_TEST(test_bucket_retry_after_sec_clamped_at_3600);
    RUN_TEST(test_bucket_retry_after_accounts_for_pending_ms);
    RUN_TEST(test_bucket_retry_after_for_multi_token_cost);
    return UNITY_END();
}
