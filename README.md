# ESP32 Smart Irrigation Controller

Autonomous, internet-free irrigation controller for an ESP32 DevKit. Reads two
water-tank levels, soil moisture and a DHT22, drives two pumps with safety
lockouts, renders a TFT UI, and hosts a WiFi AP with a small web app.

See `PROJECT_BRIEF.md` for the full scope.

## Build

PlatformIO with the standard Arduino-ESP32 toolchain:

```sh
pio run -e esp32dev          # firmware build
pio run -e esp32dev -t upload
pio test -e native           # host-side unit tests
pio run -e esp32dev -t erase # wipe NVS (forgotten WiFi password recovery)
```

All TFT_eSPI pin mappings are defined via `build_flags` in `platformio.ini`,
so a clean checkout builds without editing files inside `.pio/libdeps`.

## Hardware (defaults)

| Function            | GPIO | Notes                              |
|---------------------|------|------------------------------------|
| Water tank 1 (ADC)  | 34   | ADC1 ch6                           |
| Water tank 2 (ADC)  | 35   | ADC1 ch7                           |
| Soil moisture (ADC) | 32   | ADC1 ch4 (inverted: dry = high V)  |
| DHT22               |  4   | with 4.7 kΩ pull-up                |
| Pump 1 relay        | 26   | active LOW                         |
| Pump 2 relay        | 27   | active LOW                         |
| TFT MOSI/MISO/SCK   | 23/19/18 | SPI                            |
| TFT CS/DC/RST       | 5/2/15 | strapping pins — see notes        |

**Strapping pins.** GPIO2 (TFT DC) and GPIO15 (TFT RST) are ESP32 strapping
pins. The TFT module must not pull these in a way that prevents flash boot;
if the board fails to boot after wiring, disconnect the TFT and verify
upload succeeds first.

## WiFi access

On first boot the device starts an AP:

- SSID: `SmartIrrigation`
- Password: `12345678`
- Web UI: `http://192.168.4.1` or `http://irrigation.local` (mDNS)

⚠ **Change the default password before deploying.** The dashboard renders
a red banner whenever `state.settings.pass` still matches the documented
default; `/api/status.default_password` is the JSON flag that drives it.
Anyone within range of the AP can otherwise control the pumps.

Change the credentials from the **Settings** page; the AP restarts
automatically and mDNS re-registers on the new interface. If the password
is forgotten, run `pio run -e esp32dev -t erase` to wipe NVS.

## Web API

| Method | Path             | Notes                                                                                          |
|--------|------------------|------------------------------------------------------------------------------------------------|
| GET    | `/`              | Dashboard HTML                                                                                 |
| GET    | `/control`       | Manual pump control HTML                                                                       |
| GET    | `/settings`      | Settings form HTML                                                                             |
| GET    | `/calibration`   | Calibration form HTML                                                                          |
| GET    | `/api/status`    | `{tank1, tank2, soil, temp, hum, tank1Err, tank2Err, soilErr, dhtErr, pump1, pump2, pump1_state, pump2_state, pump1_remain_s, pump2_remain_s, mode, default_password, last_update_ms, now_ms, error}` |
| POST   | `/api/pump`      | `{"pump":1\|2,"state":true\|false,"seconds":1..3600?}` — 409 if auto mode active or safety lockout                |
| GET    | `/api/settings`  | password redacted as `"***"`; 503 if state busy                                                |
| POST   | `/api/settings`  | `{threshold, min_water, max_time, auto_mode, ssid, pass, allow_open?}` — `max_time` is **minutes** (1–60). Response: `{result, restart_ap, clamped}`. |
| GET    | `/api/calibration` | `{tank1:{dry,wet}, tank2:{dry,wet}, soil:{dry,wet}}` — current raw ADC endpoints. |
| POST   | `/api/calibrate` | Same shape as GET; sub-objects and dry/wet fields are individually optional. Validates ranges and minimum span. |
| GET    | `/api/events`    | `{now_ms, total, events:[{t, seq, level, msg}, …]}` — bounded ring-buffer tail (max 32 entries). Optional `?limit=N` (1..32) caps the response, default 16. Newest entry is last. |
| GET    | `/api/metrics`   | `{uptime_s, now_ms, free_heap, free_heap_min, station_count, main_loop_ticks, sensor_error_count, pump1_starts, pump2_starts, pump1_runtime_s, pump2_runtime_s, ap_restarts, http_requests, http_4xx, http_5xx, ap_unhealthy_ms}` — saturating counters; safe to delta. |
| POST   | `/api/reset`     | factory reset                                                                                  |

POST bodies are limited to 1 KB; oversize requests return 413 and any
remaining chunks are silently absorbed (no double-response). Empty bodies
return 400. Inputs are clamped server-side; sending `"pass":"***"` keeps
the existing password. The `clamped` field in the POST response is `true`
when at least one input value was rounded into the legal range; the saved
value is what `GET /api/settings` will report.

### Settings POST validation

`POST /api/settings` rejects malformed credentials with 400 *before* any
mutation. Possible `error` values:

| error                  | meaning                                                          |
|------------------------|------------------------------------------------------------------|
| `ssid_required`        | `ssid` field present but empty                                   |
| `ssid_too_long`        | `ssid` length > 32                                               |
| `pass_too_short`       | `pass` length 1–7 (WPA2 minimum is 8)                            |
| `pass_too_long`        | `pass` length > 63                                               |
| `open_ap_not_allowed`  | `pass` is empty without explicit `"allow_open": true`            |

To intentionally configure an unencrypted (open) AP — strongly discouraged —
send `{"pass": "", "allow_open": true}`. The redacted sentinel `"***"` always
wins over `allow_open`: the password is preserved, never downgraded.

If the device is processing another mutation when the POST arrives, the
handler retries the in-RAM apply for up to 400 ms. On persistent contention
NVS is rolled back to the previous values and the response is 503 with
`error: "state_busy_rolled_back"` (or `state_busy_persistence_inconsistent`
if the rollback itself failed) so the operator knows whether to retry.

`pump1_state` / `pump2_state` reflect the safety FSM and surface lockout
reasons to the operator. Possible values: `off`, `on`, `lockout_dry_run`,
`lockout_runtime`, `lockout_sensor`. The `error` field carries a one-line
human-readable summary of the worst current condition (also shown on the
TFT banner).

`pump1_remain_s` / `pump2_remain_s` report the seconds remaining on a
manual timed run scheduled via `POST /api/pump` `{"seconds": N}`. The
field is `-1` when no timer is armed, `0` once the timer has elapsed
but the FSM has not yet driven OFF, otherwise the floor of remaining
seconds. Hard lockouts (dry-run, sensor) drive the pump OFF without
clearing the timer, so once conditions recover and the FSM would
normally restart the pump, an already-expired timer prevents restart
rather than letting it run unbounded.

### Manual run duration (`POST /api/pump`)

`seconds` is optional; values must be in `[1, 3600]`. When omitted the
pump runs until OFF, until a safety lockout fires, or until the
`maxPumpSeconds` runtime trip — whichever comes first. The runtime
trip still bounds any timed run, so a 1-hour `seconds: 3600` request
followed by a `maxPumpSeconds: 600` configuration runs for 10 minutes
and then enters the 5-minute cooldown.

### Calibration workflow (`/api/calibrate`)

The water-level and soil-moisture sensors are linear over (dry → wet)
raw ADC endpoints. Defaults match the wiring in `PROJECT_BRIEF.md`
(water: dry=0, wet=4095; soil: dry=4095, wet=0). To re-calibrate in
the field:

1. **Capture raw readings.** Drop the water-level board into an empty
   tank, note `tank1` / `tank2` from `/api/status` — but those report
   percentage, not raw, so a quick way is to read the calibration
   values from `/api/calibration` and walk them. (A future revision
   may expose live raw values; for now use a multimeter or known
   reference points.)
2. **Submit endpoints.** `POST /api/calibrate` with any subset:
   ```json
   {"tank1": {"dry": 150, "wet": 3950}, "soil": {"dry": 3800, "wet": 350}}
   ```
3. **Validation.** Each raw value must be in `[0, 4095]`; every
   sensor whose pair is touched must end with `|dry - wet| ≥ 200`
   raw counts. On rejection: 400 with `error` ∈
   `{raw_out_of_range, span_too_small}`. Settings are not mutated.
4. **Persisted on success.** Calibration writes to NVS first, then
   to RAM under the same rollback contract as `/api/settings`. A
   reboot will surface the new endpoints.

## Safety properties (cannot be overridden via web)

- Pump cannot turn ON when tank level is below `minWaterPct` (dry-run).
- Pump cannot turn ON for a tank whose level sensor is reporting errors.
- Continuous pump runtime is capped at `maxPumpSeconds`; on trip the pump
  enters a 5-minute cooldown.
- 5% hysteresis on soil and water thresholds prevents thrash near the bound.
- Task watchdog resets the device after 30 s of unresponsiveness; relays
  are initialised OFF on boot, so a reset is fail-safe.

## Rate limits

Mutating endpoints are protected by a per-IP token-bucket limiter. On
rejection the server returns **HTTP 429** with a `Retry-After` header
(seconds) and a JSON body
`{"error":"rate_limited","retry_after_s":N}`.

| Endpoint           | Capacity | Steady-state refill |
|--------------------|---------:|---------------------|
| `POST /api/pump`     | 10       | 1 token / second    |
| `POST /api/settings` | 5        | 1 token / 5 seconds |
| `POST /api/calibrate`| 5        | 1 token / 5 seconds |
| `POST /api/reset`    | 2        | 1 token / 60 seconds|

The bucket fills to capacity at boot, so an operator can burst-click
without triggering 429. Buckets are keyed per (client IPv4, endpoint);
read-only endpoints (`/api/status`, `/api/events`, `/api/metrics`,
`/api/settings` GET, `/api/calibration`) are not rate-limited. The
table holds 16 buckets with LRU eviction — far more than typical AP
client concurrency, so legitimate clients never starve.

The dashboard honours `Retry-After` directly: it pauses status polling
for the requested number of seconds and resumes at 2 s cadence on
recovery. On any other failure (5xx, network drop) it applies
exponential backoff: 2 s → 4 s → 8 s → 16 s → 30 s, snapping back to
2 s on the first successful poll.

## Security notes

- AP traffic is unauthenticated beyond WPA2; anyone on the AP can control
  pumps. Keep the WPA2 password private.
- The dashboard surfaces a red banner whenever the configured password
  matches the documented default `12345678`. The same condition is also
  exposed as `/api/status.default_password` (boolean) for external tools.
- `/api/settings` GET redacts the password.
- All mutating endpoints (`/api/pump`, `/api/settings`, `/api/calibrate`,
  `/api/reset`) are rate-limited per client IP (see the **Rate limits**
  table). The 429 path returns `Retry-After` so dashboards and scripts
  can back off cleanly.
- mDNS responder advertises the device as `irrigation.local`; this is a
  link-local convenience only — the responder is bound to the AP
  interface and re-registered after every soft-AP restart.
- No outbound network calls; the device never leaves the AP.

## Observability

- **Events.** Lockouts, AP restarts, sensor faults and recoveries are
  written to a 32-entry in-memory ring buffer. The dashboard renders
  the last 8 in the **Recent events** card (newest first). Full tail
  is at `GET /api/events?limit=N` where `N` is 1..32. Each entry has
  `{t, seq, level, msg}`; `seq` is a monotonic counter that survives
  ring-buffer eviction so a client can detect dropped entries by gap
  detection. The buffer is bounded — once it's full, the oldest entry
  is overwritten silently.
- **Metrics.** `GET /api/metrics` returns a JSON snapshot of saturating
  uint32 counters: `uptime_s`, `free_heap` / `free_heap_min`, `station_count`,
  per-pump `pump{1,2}_starts` / `pump{1,2}_runtime_s`, `ap_restarts`,
  `http_requests`, `http_4xx`, `http_5xx`, `sensor_error_count`,
  `main_loop_ticks`, `ap_unhealthy_ms`. Counters never wrap; deltas
  between two samples are always non-negative.
- **AP health watchdog.** Polls `WiFi.softAPIP()` every 5 s. Continuous
  unhealth (IP = 0.0.0.0) for 30 s schedules a soft-AP restart through
  the same code path as a credential change; an event is logged on
  every transition.
- **JSON capacity audit.** In DEBUG builds (`CORE_DEBUG_LEVEL > 0`,
  the platformio.ini default), every JSON-producing endpoint runs an
  `overflowed()` check before sending the payload and logs
  `[json] OVERFLOW: <endpoint> capacity=...` to Serial if a future
  field addition exceeds the doc capacity. Release builds fold the
  check to a no-op.

## Project layout

```
src/
  main.cpp          setup() + loop()
  config.h          pins, defaults, tunables
  state.h           SystemState shape (single source of truth)
  sensors.cpp/.h    ADC + DHT22, error detection
  relays.cpp/.h     pump safety FSM (pure helpers in header)
  display.cpp/.h    TFT UI with partial-redraw cache
  webserver.cpp/.h  WiFi AP + AsyncWebServer + JSON API + mDNS
  ratelimit.cpp/.h  per-IP token-bucket rate limiter (pure helpers)
  storage.cpp/.h    NVS-backed settings persistence
  eventlog.cpp/.h   bounded ring-buffer event log
  metrics.cpp/.h    saturating runtime counters
test/test_native/   Unity tests for pure helpers
```
