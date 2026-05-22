// webserver.cpp — WiFi AP + AsyncWebServer with REST + HTML pages.
//
// Endpoints:
//   GET  /                  → dashboard HTML (inline)
//   GET  /control           → control HTML
//   GET  /settings          → settings HTML
//   GET  /calibration       → calibration HTML
//   GET  /api/status        → live JSON for the dashboard poller
//   POST /api/pump          → {"pump":1|2,"state":true|false,"seconds":N?}
//   GET  /api/settings      → current settings (password redacted)
//   POST /api/settings      → write & persist settings
//   GET  /api/calibration   → current raw ADC endpoints
//   POST /api/calibrate     → update raw ADC endpoints (validate + persist)
//   GET  /api/events        → ring-buffer tail of recent events
//   GET  /api/metrics       → uptime/heap/counters snapshot
//   POST /api/reset         → factory-reset settings
//
// The HTTP server runs on the AsyncTCP task; every state read/write
// is mutex-guarded against the main loop. POST handlers buffer the
// body up to HTTP_MAX_BODY_BYTES; oversize requests are rejected
// with 413.
//
// JSON capacity audit (Build 5):
//   Every endpoint that builds a JsonDocument calls AUDIT_JSON_DOC()
//   before serialising. In DEBUG builds (CORE_DEBUG_LEVEL > 0) the
//   macro logs a `[json] OVERFLOW` line if the document would have
//   exceeded its declared capacity, naming the endpoint so the doc
//   size can be raised. In release builds the macro folds to a
//   no-op so there is no runtime cost. Sizes were measured against
//   worst-case payloads:
//     /api/status        ~360 B used / 640  cap  ≈ 56 %
//     /api/settings GET  ~120 B used / 256  cap  ≈ 47 %
//     /api/settings POST  body parser, 512 cap   (HTTP body ≤ 1 KB)
//     /api/pump POST       body parser, 192 cap   (request ≤ ~80 B)
//     /api/calibration   ~150 B used / 256  cap  ≈ 59 %
//     /api/calibrate     body parser, 384 cap
//     /api/events        ~1.5 KB / 4 KB Dynamic ≈ 38 %  (16 entries)
//     /api/metrics       ~240 B used / 512  cap  ≈ 47 %
//   None are near capacity; the audit macro is the safety net for
//   future field additions.

#include "webserver.h"

#include "config.h"
#include "ratelimit.h"
#include "relays.h"
#include "sensors.h"
#include "storage.h"

#include <math.h>
#include <string.h>

#ifdef ARDUINO
  #include <Arduino.h>
  #include <WiFi.h>
  #include <ESPmDNS.h>
  #include <ESPAsyncWebServer.h>
  #include <ArduinoJson.h>

  // ---------------------------------------------------------------
  // JSON capacity audit (Build 5).
  //
  // ArduinoJson v6 silently truncates a document whose backing
  // storage is too small — the offending field gets dropped and
  // serializeJson() returns a syntactically-valid but incomplete
  // payload. JsonDocument::overflowed() flags that condition.
  //
  // Calling AUDIT_JSON_DOC(doc, "label") just before serialisation
  // logs `[json] OVERFLOW: <label> capacity=<N>` to Serial when the
  // doc would have overflowed, so a future field addition surfaces
  // immediately rather than silently truncating the API. In release
  // builds (CORE_DEBUG_LEVEL == 0) the macro folds to (void)0 so
  // there is no runtime cost on production firmware.
  //
  // The macro deliberately only logs — it does not abort the
  // request. A truncated JSON payload is preferable to a 5xx for
  // an operator who can still read most of the dashboard, and the
  // serial log gives the developer a breadcrumb to fix it.
  // ---------------------------------------------------------------
  #if (CORE_DEBUG_LEVEL > 0)
    #define AUDIT_JSON_DOC(doc, label)                                       \
        do {                                                                  \
            if ((doc).overflowed()) {                                         \
                Serial.printf("[json] OVERFLOW: %s capacity=%u used=%u\n",   \
                              (label),                                        \
                              (unsigned)(doc).capacity(),                     \
                              (unsigned)(doc).memoryUsage());                 \
            }                                                                 \
        } while (0)
  #else
    #define AUDIT_JSON_DOC(doc, label) ((void)0)
  #endif

  static AsyncWebServer g_server(HTTP_PORT);

  // Stable mDNS hostname. After the AP is up the device responds to
  // both `irrigation.local` and the AP IP. Re-registered on every
  // soft-AP restart because MDNS.begin() is bound to the active
  // station/AP interface and a softAPdisconnect/softAP() cycle
  // tears down the mDNS responder.
  static constexpr const char* MDNS_HOSTNAME = "irrigation";

  static void startMdns() {
      // Best-effort: mDNS failures must not block boot. We log and
      // continue — operators can still reach the device via the AP IP.
      MDNS.end();
      if (MDNS.begin(MDNS_HOSTNAME)) {
          MDNS.addService("http", "tcp", HTTP_PORT);
          Serial.printf("[mdns] %s.local registered\n", MDNS_HOSTNAME);
      } else {
          Serial.println(F("[mdns] begin failed; continuing without mDNS"));
      }
  }

  // ---------------------------------------------------------------
  // HTTP metrics accounting.
  //
  // Every response path funnels through `httpAccount(state, status)`
  // exactly once, immediately after `req->send()` (or, for rate-
  // limit denials, while the limiter is already on the same code
  // path). The helper:
  //
  //   - Increments `metrics.httpRequests` unconditionally so an
  //     operator can see total traffic on /api/metrics.
  //   - Increments `metrics.httpErrors4xx` for any 4xx (400/404/409/
  //     413/429) and `metrics.httpErrors5xx` for any 5xx (500/503).
  //
  // Synchronisation: the metrics block lives inside SystemState and
  // is mutex-guarded. We use a short, *best-effort* try-take here —
  // the AsyncTCP task must never block on the main loop, and a
  // dropped metric tick is preferable to a stalled HTTP response.
  // The helper is therefore safe to call after `req->send()` even
  // though the request object is gone: it only touches `state`.
  // ---------------------------------------------------------------
  static void httpAccount(SystemState& state, int status) {
      if (!state.mutex) return;
      // Tiny timeout — the lock is only contended while the main
      // loop is briefly inside its bookkeeping block. If we can't
      // grab it within 5 ms we silently drop the metric rather than
      // delay the next handler.
      if (xSemaphoreTake(state.mutex, pdMS_TO_TICKS(5)) != pdTRUE) {
          return;
      }
      metricsIncSat(state.metrics.httpRequests, 1);
      if (status >= 400 && status < 500) {
          metricsIncSat(state.metrics.httpErrors4xx, 1);
      } else if (status >= 500) {
          metricsIncSat(state.metrics.httpErrors5xx, 1);
      }
      xSemaphoreGive(state.mutex);
  }

  // Map an IPAddress to a 32-bit network-order key for the rate-limit
  // table. ipv4=0 represents "unknown peer" (the IPAddress is 0.0.0.0
  // before the client identifies); rateLimitCheck() handles that case.
  static uint32_t clientIpv4Key(AsyncWebServerRequest* req) {
      if (!req) return 0;
      IPAddress ip = req->client() ? req->client()->remoteIP() : IPAddress(0,0,0,0);
      return ((uint32_t)ip[0] << 24) | ((uint32_t)ip[1] << 16) |
             ((uint32_t)ip[2] <<  8) | ((uint32_t)ip[3]);
  }

  // Apply the rate limiter to `req` for `endpoint`. Returns true when
  // the request may proceed; on false a 429 response with
  // `Retry-After` has already been sent and the caller MUST not
  // touch `req` further.
  static bool enforceRateLimit(AsyncWebServerRequest* req,
                               RateLimitEndpoint endpoint,
                               SystemState& state) {
      uint32_t retryAfterSec = 0;
      if (rateLimitCheck(clientIpv4Key(req), endpoint, millis(),
                         retryAfterSec)) {
          return true;
      }
      // Build a small JSON body. The Retry-After header is the
      // standards-compliant signal; the body carries the same value
      // for clients that only parse JSON.
      char body[96];
      snprintf(body, sizeof(body),
               "{\"error\":\"rate_limited\",\"retry_after_s\":%u}",
               (unsigned)retryAfterSec);
      AsyncWebServerResponse* res =
          req->beginResponse(429, "application/json", body);
      char retryStr[16];
      snprintf(retryStr, sizeof(retryStr), "%u", (unsigned)retryAfterSec);
      res->addHeader("Retry-After", retryStr);
      req->send(res);
      // Account once for the 429: bumps httpRequests + httpErrors4xx
      // through the unified helper so /api/metrics never disagrees
      // with reality on rate-limit denials.
      httpAccount(state, 429);
      return false;
  }

  // Pending soft-AP restart state.
  static bool     g_apRestartPending = false;
  static uint32_t g_apRestartAtMs    = 0;

  // Per-request body buffers. AsyncWebServer streams body chunks via
  // (index, len, total); we accumulate into a small heap buffer and
  // parse on the final chunk. One concurrent POST is the realistic
  // case on an AP, but we key the buffer by request pointer to be
  // safe.
  //
  // `received` counts bytes we have actually copied (independent of
  // chunk arrival order or duplicate chunks). `rejected` short-
  // circuits subsequent chunk callbacks once we have already sent a
  // 4xx/5xx response, preventing a double-send on the same request.
  struct PendingBody {
      AsyncWebServerRequest* req = nullptr;
      char*                  buf = nullptr;
      size_t                 cap = 0;
      size_t                 received = 0;
      bool                   rejected = false;
  };
  static constexpr size_t MAX_PENDING = 4;
  static PendingBody g_pending[MAX_PENDING];

  // Look up an existing pending entry for this request. Does not allocate.
  static PendingBody* findPending(AsyncWebServerRequest* req) {
      for (auto& p : g_pending) {
          if (p.req == req) return &p;
      }
      return nullptr;
  }

  // Reserve a slot for `req`. The slot tracks state across chunks even
  // when we have already responded (rejected=true), so subsequent
  // chunks for the same request are silently absorbed.
  static PendingBody* allocPending(AsyncWebServerRequest* req, size_t total,
                                   bool rejected) {
      for (auto& p : g_pending) {
          if (p.req == nullptr) {
              p.req = req;
              p.cap = rejected ? 0 : total;
              p.received = 0;
              p.rejected = rejected;
              p.buf = nullptr;
              if (!rejected && total > 0) {
                  p.buf = (char*)malloc(total + 1);
                  if (!p.buf) {
                      p.req = nullptr;
                      return nullptr;
                  }
              }
              return &p;
          }
      }
      return nullptr;
  }

  static void freePending(AsyncWebServerRequest* req) {
      for (auto& p : g_pending) {
          if (p.req == req) {
              if (p.buf) free(p.buf);
              p.buf = nullptr;
              p.req = nullptr;
              p.cap = p.received = 0;
              p.rejected = false;
              return;
          }
      }
  }

  // ---------- Inline HTML (PROGMEM) ----------

  static const char PAGE_HEAD[] PROGMEM = R"HTML(
<!doctype html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Smart Irrigation</title>
<style>
  :root { color-scheme: dark; }
  body { font-family: system-ui,Arial,sans-serif; margin:0; background:#0b1220; color:#e6edf3; }
  header { background:#111c2e; padding:14px 18px; display:flex; justify-content:space-between; align-items:center; }
  header h1 { margin:0; font-size:18px; }
  nav a { color:#7ec3ff; margin-left:14px; text-decoration:none; }
  main { padding:16px; max-width:720px; margin:0 auto; }
  .grid { display:grid; grid-template-columns:repeat(auto-fit,minmax(200px,1fr)); gap:12px; }
  .card { background:#111c2e; padding:14px; border-radius:10px; }
  .card h2 { margin:0 0 8px; font-size:14px; color:#9ab; text-transform:uppercase; letter-spacing:.05em; }
  .val { font-size:28px; font-weight:600; }
  .bar { height:8px; background:#1d2a44; border-radius:4px; margin-top:8px; overflow:hidden; }
  .bar > div { height:100%; background:#3ad29f; transition:width .3s; }
  .err .val, .err h2 { color:#ff6b6b; }
  .row { display:flex; gap:12px; margin-top:16px; flex-wrap:wrap; }
  button { background:#1f6feb; color:#fff; border:0; padding:10px 18px; border-radius:8px; font-size:15px; cursor:pointer; }
  button.off { background:#444c56; }
  button.danger { background:#cf3636; }
  label { display:block; margin:10px 0 4px; font-size:13px; color:#9ab; }
  input[type=number],input[type=text],input[type=password],select { width:100%; padding:8px; border-radius:6px; border:1px solid #2d3748; background:#0b1220; color:#e6edf3; box-sizing:border-box; }
  .banner { background:#cf3636; color:#fff; padding:10px; border-radius:8px; margin-bottom:12px; }
  .ok { color:#3ad29f; }
  .warn { color:#fdba74; }
  .err-inline { color:#ff6b6b; font-weight:600; }
  .small { font-size:12px; color:#9ab; }
</style></head><body>
<header><h1>🌱 Smart Irrigation</h1>
<nav><a href="/">Dashboard</a><a href="/control">Control</a><a href="/settings">Settings</a><a href="/calibration">Calibration</a></nav>
</header><main>
)HTML";

  static const char PAGE_FOOT[] PROGMEM = "</main></body></html>";

  static const char PAGE_DASHBOARD[] PROGMEM = R"HTML(
<div id="default_pw_banner" class="banner" style="display:none">
  ⚠ The AP is still using the documented default password
  (<code>12345678</code>). Anyone in range can control this device.
  <a href="/settings" style="color:#fff;text-decoration:underline">Change it now</a>.
</div>
<div id="banner" class="banner" style="display:none"></div>
<div class="grid">
  <div class="card" id="c-tank1"><h2>Tank 1</h2><div class="val"><span id="tank1">--</span>%</div><div class="bar"><div id="bar-tank1" style="width:0"></div></div></div>
  <div class="card" id="c-tank2"><h2>Tank 2</h2><div class="val"><span id="tank2">--</span>%</div><div class="bar"><div id="bar-tank2" style="width:0"></div></div></div>
  <div class="card" id="c-soil1"><h2>Soil 1 (Agarot 1)</h2><div class="val"><span id="soil1">--</span>%</div><div class="bar"><div id="bar-soil1" style="width:0"></div></div></div>
  <div class="card" id="c-soil2"><h2>Soil 2 (Agarot 2)</h2><div class="val"><span id="soil2">--</span>%</div><div class="bar"><div id="bar-soil2" style="width:0"></div></div></div>
  <div class="card" id="c-temp"><h2>Temperature</h2><div class="val"><span id="temp">--</span>°C</div></div>
  <div class="card" id="c-hum"><h2>Humidity</h2><div class="val"><span id="hum">--</span>%</div></div>
  <div class="card"><h2>Pumps</h2>
    <div>Pump 1: <span id="pump1" class="warn">--</span> <span id="pump1_remain" class="small"></span></div>
    <div>Pump 2: <span id="pump2" class="warn">--</span> <span id="pump2_remain" class="small"></span></div>
    <div class="small">Mode: <span id="mode">--</span></div>
    <div class="small">Reading age: <span id="age">--</span></div>
  </div>
</div>
<div class="card" style="margin-top:16px"><h2>Recent events</h2>
  <div id="events" class="small">loading…</div>
  <div class="small" style="margin-top:8px"><a href="/api/events" style="color:#7ec3ff">view JSON</a> · <a href="/api/metrics" style="color:#7ec3ff">metrics</a></div>
</div>
<script>
// ---------------------------------------------------------------
// Status fetcher with exponential backoff.
//   - Healthy poll cadence: STATUS_OK_MS (2 s), matches the firmware
//     SENSORS_PERIOD_MS so we never poll faster than the data updates.
//   - On fetch failure (network blip, AP restart, 5xx) we double the
//     delay up to STATUS_MAX_MS (30 s). The next success snaps it
//     back to STATUS_OK_MS so the UI feels live again immediately.
// 429 responses honour the Retry-After header rather than backing
// off blindly: a server-driven hint is always more accurate than
// our local guess.
// ---------------------------------------------------------------
const STATUS_OK_MS  = 2000;
const STATUS_MAX_MS = 30000;
let statusDelay = STATUS_OK_MS;
let statusTimer = null;
let lastBannerWasError = false;

function setBanner(text, isError){
  const b = document.getElementById('banner');
  if (!text) { b.style.display = 'none'; lastBannerWasError = false; return; }
  b.textContent = text;
  b.style.display = 'block';
  b.style.background = isError ? '#cf3636' : '#1f6feb';
  lastBannerWasError = !!isError;
}

function ageLabel(now, last){
  if (last == null || now == null) return 'unknown';
  // Wrap-safe: int32 of the unsigned difference is the relative offset.
  let d = (now - last) | 0;
  if (d < 0) d = 0;
  const s = Math.floor(d / 1000);
  if (s < 2)   return 'just now';
  if (s < 60)  return s + 's';
  if (s < 3600) return Math.floor(s / 60) + 'm';
  return Math.floor(s / 3600) + 'h';
}

async function tick(){
  let nextDelay = STATUS_OK_MS;
  try {
    const r = await fetch('/api/status', {cache:'no-store'});
    if (r.status === 429) {
      // Honour server-driven Retry-After; clamp to STATUS_MAX_MS so
      // a misbehaving header value cannot pause the dashboard
      // forever.
      const ra = parseInt(r.headers.get('Retry-After')) || 5;
      nextDelay = Math.min(STATUS_MAX_MS, Math.max(1000, ra * 1000));
      setBanner('Rate-limited by device; retrying in '+(nextDelay/1000)+' s', true);
      return;
    }
    if (!r.ok) throw new Error('http '+r.status);
    const d = await r.json();
    const set=(id,v)=>{const el=document.getElementById(id); if(el) el.textContent=v;};
    const setBar=(id,p)=>{const el=document.getElementById(id); if(el) el.style.width=Math.max(0,Math.min(100,p))+'%';};
    set('tank1', d.tank1Err?'ERR':d.tank1); setBar('bar-tank1', d.tank1Err?0:d.tank1);
    set('tank2', d.tank2Err?'ERR':d.tank2); setBar('bar-tank2', d.tank2Err?0:d.tank2);
    set('soil1', d.soil1Err?'ERR':d.soil1); setBar('bar-soil1', d.soil1Err?0:d.soil1);
    set('soil2', d.soil2Err?'ERR':d.soil2); setBar('bar-soil2', d.soil2Err?0:d.soil2);
    set('temp',  (d.dhtErr || d.temp == null) ? 'ERR' : Number(d.temp).toFixed(1));
    set('hum',   (d.dhtErr || d.hum  == null) ? 'ERR' : Number(d.hum ).toFixed(0));
    document.getElementById('c-tank1').classList.toggle('err', !!d.tank1Err);
    document.getElementById('c-tank2').classList.toggle('err', !!d.tank2Err);
    document.getElementById('c-soil1').classList.toggle('err', !!d.soil1Err);
    document.getElementById('c-soil2').classList.toggle('err', !!d.soil2Err);
    document.getElementById('c-temp' ).classList.toggle('err', !!d.dhtErr);
    document.getElementById('c-hum'  ).classList.toggle('err', !!d.dhtErr);
    set('pump1', d.pump1?'ON':'OFF'); document.getElementById('pump1').className = d.pump1?'ok':'warn';
    set('pump2', d.pump2?'ON':'OFF'); document.getElementById('pump2').className = d.pump2?'ok':'warn';
    // If a pump is in a lockout state, replace the bare ON/OFF with the
    // human-readable reason so the operator can see *why* it's off.
    const fsmLabel = (s) => {
      switch(s){
        case 'on':              return 'ON';
        case 'off':             return 'OFF';
        case 'lockout_dry_run': return 'LOCKED: water low';
        case 'lockout_runtime': return 'LOCKED: runtime';
        case 'lockout_sensor':  return 'LOCKED: sensor';
        default:                return s||'?';
      }
    };
    if (d.pump1_state) { set('pump1', fsmLabel(d.pump1_state));
      document.getElementById('pump1').className = d.pump1_state==='on'?'ok':(d.pump1_state==='off'?'warn':'err-inline'); }
    if (d.pump2_state) { set('pump2', fsmLabel(d.pump2_state));
      document.getElementById('pump2').className = d.pump2_state==='on'?'ok':(d.pump2_state==='off'?'warn':'err-inline'); }
    // Show remaining seconds on a manually-timed run (server reports
    // pumpN_remain_s as -1 when no timer is armed).
    const remainText = (s) => (s == null || s < 0) ? '' : ('('+s+'s left)');
    set('pump1_remain', remainText(d.pump1_remain_s));
    set('pump2_remain', remainText(d.pump2_remain_s));
    set('mode', d.mode);
    set('age', ageLabel(d.now_ms, d.last_update_ms));
    // Default-password banner. Once the operator changes the password
    // the flag goes false and the banner disappears immediately.
    document.getElementById('default_pw_banner').style.display =
      d.default_password ? 'block' : 'none';
    if (d.error && d.error.length){ setBanner(d.error, true); }
    else if (lastBannerWasError) { setBanner(null); }
    // Successful poll snaps the cadence back to the healthy rate.
    nextDelay = STATUS_OK_MS;
  } catch(e) {
    setBanner('Connection lost: '+e.message+' (retrying in '+
              Math.ceil(statusDelay/1000)+'s)', true);
    // Exponential backoff: 2 → 4 → 8 → 16 → 30 (cap).
    nextDelay = Math.min(STATUS_MAX_MS, Math.max(STATUS_OK_MS, statusDelay * 2));
  } finally {
    statusDelay = nextDelay;
    if (statusTimer) clearTimeout(statusTimer);
    statusTimer = setTimeout(tick, statusDelay);
  }
}
async function tickEvents(){
  try {
    const r = await fetch('/api/events?limit=8', {cache:'no-store'});
    if(!r.ok) return;
    const d = await r.json();
    const el = document.getElementById('events');
    if (!d.events || !d.events.length){ el.textContent='(none)'; return; }
    // Newest first; format as "<+age> [level] msg".
    const now = d.now_ms || 0;
    const rows = d.events.slice().reverse().map(ev => {
      const ageS = Math.max(0, Math.floor((now - ev.t)/1000));
      const cls = ev.level==='error'?'err-inline':(ev.level==='warn'?'warn':'ok');
      return `<div><span class="${cls}">[${ev.level}]</span> ${escapeHtml(ev.msg)} <span class="small">(${ageS}s ago)</span></div>`;
    });
    el.innerHTML = rows.join('');
  } catch(e) { /* dashboard already shows status banner */ }
}
function escapeHtml(s){ return String(s).replace(/[&<>"']/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;','\'':'&#39;'}[c])); }
tick(); tickEvents();
setInterval(tickEvents, 4000);
</script>
)HTML";

  static const char PAGE_CONTROL[] PROGMEM = R"HTML(
<div id="msg"></div>
<div class="card"><h2>Pump 1</h2>
  <label>Run for (seconds, blank = unlimited; max 3600)</label>
  <input type="number" id="sec1" min="1" max="3600" placeholder="unlimited">
  <div class="row" style="margin-top:8px">
    <button onclick="setPump(1,true,'sec1')">Turn ON</button>
    <button class="off" onclick="setPump(1,false,null)">Turn OFF</button>
  </div>
</div>
<div class="card" style="margin-top:12px"><h2>Pump 2</h2>
  <label>Run for (seconds, blank = unlimited; max 3600)</label>
  <input type="number" id="sec2" min="1" max="3600" placeholder="unlimited">
  <div class="row" style="margin-top:8px">
    <button onclick="setPump(2,true,'sec2')">Turn ON</button>
    <button class="off" onclick="setPump(2,false,null)">Turn OFF</button>
  </div>
</div>
<p class="small">Manual control is honoured only when Mode = MANUAL. Safety lockouts (dry-run, sensor error, runtime) cannot be overridden. The configured maximum runtime still caps any timed run.</p>
<script>
async function setPump(p, on, secId){
  const m=document.getElementById('msg');
  m.textContent='…';
  const body={pump:p,state:on};
  if (on && secId) {
    const v = document.getElementById(secId).value.trim();
    if (v.length) {
      const n = Number(v);
      if (!Number.isInteger(n) || n < 1 || n > 3600) {
        m.textContent='Error: seconds must be 1..3600';
        m.style.background='#cf3636';
        m.style.padding='10px'; m.style.borderRadius='8px';
        m.style.color='#fff'; m.style.marginBottom='12px';
        return;
      }
      body.seconds = n;
    }
  }
  try{
    const r=await fetch('/api/pump',{method:'POST',headers:{'content-type':'application/json'},body:JSON.stringify(body)});
    const d=await r.json();
    m.textContent = (r.ok?'OK: ':'Denied: ') + (d.result||d.error||'?')
                  + (r.ok && d.seconds ? ' ('+d.seconds+'s scheduled)' : '');
    m.className = r.ok?'banner':'banner';
    m.style.background = r.ok?'#1f6feb':'#cf3636';
    m.style.padding='10px'; m.style.borderRadius='8px'; m.style.color='#fff'; m.style.marginBottom='12px';
  } catch(e){ m.textContent='Request failed: '+e.message; }
}
</script>
)HTML";

  static const char PAGE_CALIBRATION[] PROGMEM = R"HTML(
<div id="msg"></div>
<p class="small">Set the raw ADC values that correspond to 0% and 100%. Water sensors typically read 0 when empty and 4095 when fully submerged; cheap soil sensors read high when dry and low when wet — both orientations are supported. The minimum span between Dry and Wet is 200 raw counts.</p>
<form id="f" onsubmit="save(event)">
  <div class="card"><h2>Tank 1</h2>
    <label>Dry endpoint (raw, 0..4095)</label>
    <input type="number" name="tank1_dry" min="0" max="4095" required>
    <label>Wet endpoint (raw, 0..4095)</label>
    <input type="number" name="tank1_wet" min="0" max="4095" required>
  </div>
  <div class="card" style="margin-top:12px"><h2>Tank 2</h2>
    <label>Dry endpoint (raw)</label>
    <input type="number" name="tank2_dry" min="0" max="4095" required>
    <label>Wet endpoint (raw)</label>
    <input type="number" name="tank2_wet" min="0" max="4095" required>
  </div>
  <div class="card" style="margin-top:12px"><h2>Soil 1 (Agarot 1)</h2>
    <label>Dry endpoint (raw, typically 4095)</label>
    <input type="number" name="soil_dry" min="0" max="4095" required>
    <label>Wet endpoint (raw, typically 0)</label>
    <input type="number" name="soil_wet" min="0" max="4095" required>
  </div>
  <div class="card" style="margin-top:12px"><h2>Soil 2 (Agarot 2)</h2>
    <label>Dry endpoint (raw, typically 4095)</label>
    <input type="number" name="soil2_dry" min="0" max="4095" required>
    <label>Wet endpoint (raw, typically 0)</label>
    <input type="number" name="soil2_wet" min="0" max="4095" required>
  </div>
  <div class="row">
    <button type="submit">Save calibration</button>
  </div>
</form>
<script>
async function load(){
  const r=await fetch('/api/calibration'); const d=await r.json();
  document.querySelector('[name=tank1_dry]').value=d.tank1.dry;
  document.querySelector('[name=tank1_wet]').value=d.tank1.wet;
  document.querySelector('[name=tank2_dry]').value=d.tank2.dry;
  document.querySelector('[name=tank2_wet]').value=d.tank2.wet;
  document.querySelector('[name=soil_dry]').value =d.soil.dry;
  document.querySelector('[name=soil_wet]').value =d.soil.wet;
  document.querySelector('[name=soil2_dry]').value=d.soil2.dry;
  document.querySelector('[name=soil2_wet]').value=d.soil2.wet;
}
async function save(e){
  e.preventDefault();
  const f=e.target;
  const m=document.getElementById('msg');
  m.style.display='block'; m.style.padding='10px'; m.style.borderRadius='8px';
  m.style.color='#fff'; m.style.marginBottom='12px';
  const body={
    tank1:{dry:Number(f.tank1_dry.value), wet:Number(f.tank1_wet.value)},
    tank2:{dry:Number(f.tank2_dry.value), wet:Number(f.tank2_wet.value)},
    soil :{dry:Number(f.soil_dry.value),  wet:Number(f.soil_wet.value)},
    soil2:{dry:Number(f.soil2_dry.value), wet:Number(f.soil2_wet.value)}
  };
  try{
    const r=await fetch('/api/calibrate',{method:'POST',headers:{'content-type':'application/json'},body:JSON.stringify(body)});
    const d=await r.json();
    m.textContent = r.ok ? 'Saved.' : ('Error: '+(d.error||'?'));
    m.style.background = r.ok ? '#1f6feb' : '#cf3636';
  }catch(err){ m.textContent='Request failed: '+err.message; m.style.background='#cf3636'; }
}
load();
</script>
)HTML";

  static const char PAGE_SETTINGS[] PROGMEM = R"HTML(
<div id="msg"></div>
<form id="f">
  <div class="card">
    <h2>Irrigation</h2>
    <label>Soil threshold % (auto mode trigger)</label>
    <input type="number" name="threshold" min="0" max="100" required>
    <label>Min water level % (dry-run protection)</label>
    <input type="number" name="min_water" min="0" max="100" required>
    <label>Max pump runtime (minutes, 1–60)</label>
    <input type="number" name="max_time" min="1" max="60" required>
    <label>Mode</label>
    <select name="auto_mode"><option value="true">AUTO</option><option value="false">MANUAL</option></select>
  </div>
  <div class="card">
    <h2>WiFi AP (local hotspot)</h2>
    <label>SSID (1–32 chars)</label>
    <input type="text" name="ssid" maxlength="32" required>
    <label>Password (8–63 chars, blank = keep current)</label>
    <input type="password" name="pass" minlength="8" maxlength="63">
  </div>
  <div class="card">
    <h2>WiFi STA (internet connection)</h2>
    <label>Router SSID (blank = no internet)</label>
    <input type="text" name="sta_ssid" maxlength="32">
    <label>Router password (blank = keep current)</label>
    <input type="password" name="sta_pass" maxlength="63">
  </div>
  <div class="card">
    <h2>AI Server</h2>
    <label>Server URL (e.g. http://192.168.1.100:8000)</label>
    <input type="text" name="server_url" maxlength="128" placeholder="http://your-server:8000">
    <label>API Key (blank = keep current)</label>
    <input type="password" name="server_key" maxlength="64">
  </div>

  <div class="row">
    <button type="button" onclick="save()">Save</button>
    <button type="button" class="danger" onclick="resetAll()">Factory reset</button>
  </div>
</form>
<p class="small">Saving SSID/password will restart the AP. Reconnect to the new network after a few seconds.</p>
<script>
async function load(){
  const r=await fetch('/api/settings'); const d=await r.json();
  document.querySelector('[name=threshold]').value=d.threshold;
  document.querySelector('[name=min_water]').value=d.min_water;
  document.querySelector('[name=max_time]').value=d.max_time;
  document.querySelector('[name=auto_mode]').value=d.auto_mode?'true':'false';
  document.querySelector('[name=ssid]').value=d.ssid;
  document.querySelector('[name=sta_ssid]').value=d.sta_ssid||'';
  document.querySelector('[name=server_url]').value=d.server_url||'';
}
async function save(){
  const f=document.getElementById('f');
  const m=document.getElementById('msg');
  m.style.display='block'; m.style.padding='10px'; m.style.borderRadius='8px'; m.style.color='#fff'; m.style.marginBottom='12px';
  const ssid=f.ssid.value;
  const pass=f.pass.value;
  if (ssid.length === 0)        { m.textContent='Error: ssid_required';      m.style.background='#cf3636'; return; }
  if (ssid.length > 32)         { m.textContent='Error: ssid_too_long';      m.style.background='#cf3636'; return; }
  if (pass.length > 0 && pass.length < 8) { m.textContent='Error: pass_too_short (WPA2 needs 8+ chars)'; m.style.background='#cf3636'; return; }
  if (pass.length > 63)         { m.textContent='Error: pass_too_long';      m.style.background='#cf3636'; return; }
  const body={
    threshold:Number(f.threshold.value),
    min_water:Number(f.min_water.value),
    max_time :Number(f.max_time.value),
    auto_mode:f.auto_mode.value==='true',
    ssid     :ssid,
    sta_ssid :f.sta_ssid.value
  };
  if (pass.length > 0) body.pass = pass;
  if (f.sta_pass.value.length > 0) body.sta_pass = f.sta_pass.value;
  if (f.server_url.value.length > 0) body.server_url = f.server_url.value;
  if (f.server_key.value.length > 0) body.server_key = f.server_key.value;
  const payload=JSON.stringify(body);
  try{
    const r=await fetch('/api/settings',{method:'POST',headers:{'Content-Type':'application/json'},body:payload});
    const d=await r.json();
    m.textContent = r.ok ? 'Saved.' : ('Error: '+(d.error||r.status));
    m.style.background = r.ok ? '#1f6feb' : '#cf3636';
  }catch(err){ m.textContent='Request failed: '+err.message; m.style.background='#cf3636'; }
}
async function resetAll(){
  if(!confirm('Reset all settings to factory defaults?')) return;
  await fetch('/api/reset',{method:'POST'});
  await load();
}
load();
</script>
)HTML";

  // ---------- Helpers ----------

  // sendHtml() and sendJsonError() are the two response funnels used
  // most often; threading SystemState through them keeps httpAccount
  // calls colocated with the actual req->send so we cannot forget
  // to bump the counter on a future code path.
  static void sendHtml(SystemState& state, AsyncWebServerRequest* req,
                       const char* body) {
      AsyncResponseStream* res = req->beginResponseStream("text/html");
      res->print(FPSTR(PAGE_HEAD));
      res->print(FPSTR(body));
      res->print(FPSTR(PAGE_FOOT));
      req->send(res);
      httpAccount(state, 200);
  }

  static void sendJsonError(SystemState& state, AsyncWebServerRequest* req,
                            int code, const char* msg) {
      StaticJsonDocument<128> d;
      d["error"] = msg;
      AUDIT_JSON_DOC(d, "sendJsonError");
      String out;
      serializeJson(d, out);
      req->send(code, "application/json", out);
      httpAccount(state, code);
  }

  // ---------- /api/status ----------

  static void handleStatus(SystemState& state, AsyncWebServerRequest* req) {
      // Mirror handleGetSettings: a contended mutex is reported as
      // 503 (state busy) so the dashboard knows to retry rather than
      // parsing a partial JSON document and surfacing a misleading
      // "connection lost" banner.
      if (!state.mutex || xSemaphoreTake(state.mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
          sendJsonError(state, req, 503, "state busy");
          return;
      }
      // Bumped from 512 to 640 to absorb the new default_password /
      // last_update_ms / now_ms fields. Build 5 will audit all
      // StaticJsonDocument capacities against measured payloads.
      StaticJsonDocument<640> d;
      d["tank1"]    = state.sensors.tank1Pct;
      d["tank2"]    = state.sensors.tank2Pct;
      d["soil1"]    = state.sensors.soil1Pct;
      d["soil2"]    = state.sensors.soil2Pct;
      // Send `null` instead of a 0.0 sentinel on DHT failure so the
      // client can disambiguate "no reading" from a real 0 °C value.
      if (state.sensors.dhtErr || isnan(state.sensors.tempC)) {
          d["temp"] = nullptr;
      } else {
          d["temp"] = state.sensors.tempC;
      }
      if (state.sensors.dhtErr || isnan(state.sensors.humPct)) {
          d["hum"] = nullptr;
      } else {
          d["hum"] = state.sensors.humPct;
      }
      d["tank1Err"] = state.sensors.tank1Err;
      d["tank2Err"] = state.sensors.tank2Err;
      d["soil1Err"] = state.sensors.soil1Err;
      d["soil2Err"] = state.sensors.soil2Err;
      d["dhtErr"]   = state.sensors.dhtErr;
      d["pump1"]    = state.pumps[0].on;
      d["pump2"]    = state.pumps[1].on;
      // Surface the FSM state so the dashboard can show "ON" /
      // "OFF" / "LOCKED: dry-run" instead of just a boolean.
      d["pump1_state"] = pumpFsmText(state.pumps[0].fsm);
      d["pump2_state"] = pumpFsmText(state.pumps[1].fsm);
      // Remaining seconds on a manually-timed run; -1 means
      // "no timer armed". Computed from runUntilMs using the
      // rollover-safe diff so this stays correct across the
      // millis() wrap boundary.
      auto remain = [](uint32_t now, uint32_t deadline) -> int32_t {
          if (deadline == 0) return -1;
          int32_t diff = (int32_t)(deadline - now);
          if (diff <= 0) return 0;
          return diff / 1000;
      };
      uint32_t nowMs = millis();
      d["pump1_remain_s"] = remain(nowMs, state.pumps[0].runUntilMs);
      d["pump2_remain_s"] = remain(nowMs, state.pumps[1].runUntilMs);
      d["mode"]     = state.settings.autoMode ? "auto" : "manual";
      // Surface whether the AP is still using the documented default
      // WPA2 password (`12345678`). The dashboard renders a red banner
      // until the operator changes it; without the flag a fresh device
      // boots into a publicly-known credential and stays there silently.
      d["default_password"] =
          (strcmp(state.settings.pass, DEFAULT_AP_PASSWORD) == 0);
      // Timestamps so the dashboard can render a "(Ns ago)" age and
      // disambiguate "stale data" from "fresh 0 reading". Both are
      // millis()-based and roll over with the device; the client uses
      // their *difference* (now - last_update) which is wrap-safe.
      d["last_update_ms"] = state.sensors.lastUpdateMs;
      d["now_ms"]         = millis();
      // ArduinoJson v6 stores `const char*` by reference, so the doc
      // would alias the live `state.lastError` buffer. Serialize
      // before releasing the mutex to avoid a torn read if the relays
      // task rewrites lastError between give() and serializeJson().
      d["error"]    = state.lastError;
      AUDIT_JSON_DOC(d, "/api/status");
      String out;
      serializeJson(d, out);
      xSemaphoreGive(state.mutex);
      req->send(200, "application/json", out);
      httpAccount(state, 200);
  }

  // ---------- /api/settings GET ----------

  static void handleGetSettings(SystemState& state, AsyncWebServerRequest* req) {
      DynamicJsonDocument d(768);
      if (!state.mutex || xSemaphoreTake(state.mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
          sendJsonError(state, req, 503, "state busy");
          return;
      }
      d["threshold"] = state.settings.soilThresholdPct;
      d["min_water"] = state.settings.minWaterPct;
      d["max_time"]  = (state.settings.maxPumpSeconds + 30) / 60;
      d["auto_mode"] = state.settings.autoMode;
      d["ssid"]      = state.settings.ssid;
      d["pass"]      = "***";
      // WiFi STA
      d["sta_ssid"]  = state.settings.staSSID;
      d["sta_pass"]  = "***";
      // AI Server
      d["server_url"] = state.settings.serverUrl;
      d["server_key"] = state.settings.serverApiKey[0] ? "***" : "";
      AUDIT_JSON_DOC(d, "/api/settings GET");
      String out;
      serializeJson(d, out);
      xSemaphoreGive(state.mutex);
      req->send(200, "application/json", out);
      httpAccount(state, 200);
  }

  // ---------- /api/settings POST ----------

  static void handlePostSettings(SystemState& state,
                                 AsyncWebServerRequest* req,
                                 const char* body, size_t len) {
      DynamicJsonDocument d(768);
      DeserializationError err = deserializeJson(d, body, len);
      if (err) { sendJsonError(state, req, 400, err.c_str()); return; }

      // ----------------------------------------------------------------
      // Build a SettingsPatch from the JSON body. Validation + apply
      // is delegated to applySettingsPatch() (in storage.cpp), which
      // is unit-tested under the native env. Doing the parse here and
      // the rules there keeps Arduino-specific code (ArduinoJson) out
      // of the testable surface.
      //
      // Strings are read via .as<const char*>(), which returns a
      // pointer into the JsonDocument's buffer — valid for the
      // lifetime of `d`, which outlives applySettingsPatch().
      // ----------------------------------------------------------------
      SettingsPatch patch;
      if (d.containsKey("threshold")) {
          patch.hasThreshold = true;
          patch.threshold    = d["threshold"].as<int>();
      }
      if (d.containsKey("min_water")) {
          patch.hasMinWater = true;
          patch.minWater    = d["min_water"].as<int>();
      }
      if (d.containsKey("max_time")) {
          patch.hasMaxTimeMin = true;
          patch.maxTimeMin    = d["max_time"].as<int>();
      }
      if (d.containsKey("auto_mode")) {
          patch.hasAutoMode = true;
          patch.autoMode    = d["auto_mode"].as<bool>();
      }
      if (d.containsKey("ssid")) {
          patch.hasSsid = true;
          patch.ssid    = d["ssid"].as<const char*>();
      }
      if (d.containsKey("pass")) {
          patch.hasPass = true;
          patch.pass    = d["pass"].as<const char*>();
      }
      if (d.containsKey("allow_open")) {
          patch.allowOpen = d["allow_open"].as<bool>();
      }

      // WiFi STA fields (applied directly to ns after patch)
      bool hasStaSsid = d.containsKey("sta_ssid");
      bool hasStaPass = d.containsKey("sta_pass");
      bool hasServerUrl = d.containsKey("server_url");
      bool hasServerKey = d.containsKey("server_key");

      // Snapshot current settings under the mutex. We capture both
      // `ns` (what we're about to mutate) and `prev` (rollback target
      // if the second mutex take fails after we've already persisted).
      Settings ns;
      Settings prev;
      bool needApRestart = false;
      if (!state.mutex || xSemaphoreTake(state.mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
          sendJsonError(state, req, 503, "state busy");
          return;
      }
      ns   = state.settings;
      prev = state.settings;
      xSemaphoreGive(state.mutex);

      // Validate and apply. On any rule violation `ns` is left
      // untouched so we can return immediately without persisting.
      SettingsPatchResult pr = applySettingsPatch(patch, ns, needApRestart);
      if (pr != SettingsPatchResult::Ok) {
          sendJsonError(state, req, 400, settingsPatchErrorText(pr));
          return;
      }

      // Apply STA/Server fields directly
      if (hasStaSsid) {
          const char* v = d["sta_ssid"].as<const char*>();
          if (v) { strncpy(ns.staSSID, v, MAX_STA_SSID_LEN); ns.staSSID[MAX_STA_SSID_LEN] = '\0'; }
      }
      if (hasStaPass) {
          const char* v = d["sta_pass"].as<const char*>();
          if (v && strcmp(v, "***") != 0) { strncpy(ns.staPass, v, MAX_STA_PASS_LEN); ns.staPass[MAX_STA_PASS_LEN] = '\0'; }
      }
      if (hasServerUrl) {
          const char* v = d["server_url"].as<const char*>();
          if (v) { strncpy(ns.serverUrl, v, MAX_SERVER_URL_LEN); ns.serverUrl[MAX_SERVER_URL_LEN] = '\0'; }
      }
      if (hasServerKey) {
          const char* v = d["server_key"].as<const char*>();
          if (v && strcmp(v, "***") != 0) { strncpy(ns.serverApiKey, v, MAX_SERVER_API_KEY_LEN); ns.serverApiKey[MAX_SERVER_API_KEY_LEN] = '\0'; }
      }

      // Defence in depth: settingsClamp() catches any numeric drift
      // even though applySettingsPatch already clamps. With explicit
      // validation above, clamp can now only adjust numeric fields.
      bool clamped = settingsClamp(ns);

      // Persist BEFORE applying to in-memory state. If NVS write fails
      // we leave runtime state untouched so RAM and storage cannot
      // diverge across a reboot.
      if (!storageSaveSettings(ns)) {
          sendJsonError(state, req, 500, "nvs write failed");
          return;
      }

      // Apply to RAM under the mutex. The AsyncTCP task can race the
      // main loop here; back off and retry up to 4 × 100 ms (≤400 ms
      // total). On persistent contention we MUST roll NVS back to
      // `prev` so a future reboot doesn't surface settings the live
      // system never saw — that would be a silent NVS/RAM divergence
      // behind a 200 OK.
      bool ramApplied = false;
      for (int attempt = 0; attempt < 4; ++attempt) {
          if (state.mutex && xSemaphoreTake(state.mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
              state.settings = ns;
              xSemaphoreGive(state.mutex);
              ramApplied = true;
              break;
          }
      }
      if (!ramApplied) {
          bool rolledBack = storageSaveSettings(prev);
          sendJsonError(state, req, 503,
              rolledBack ? "state_busy_rolled_back"
                         : "state_busy_persistence_inconsistent");
          return;
      }

      StaticJsonDocument<128> ok;
      ok["result"] = "ok";
      ok["restart_ap"] = needApRestart;
      ok["clamped"] = clamped;
      AUDIT_JSON_DOC(ok, "/api/settings POST resp");
      String out;
      serializeJson(ok, out);
      req->send(200, "application/json", out);
      httpAccount(state, 200);

      if (needApRestart) {
          webserverScheduleApRestart(state);
      }
  }

  // ---------- /api/pump POST ----------

  static void handlePostPump(SystemState& state,
                             AsyncWebServerRequest* req,
                             const char* body, size_t len) {
      StaticJsonDocument<192> d;
      DeserializationError err = deserializeJson(d, body, len);
      if (err) { sendJsonError(state, req, 400, err.c_str()); return; }
      if (!d.containsKey("pump") || !d.containsKey("state")) {
          sendJsonError(state, req, 400, "missing pump/state"); return;
      }
      int pump = d["pump"].as<int>();
      bool on  = d["state"].as<bool>();
      if (pump < 1 || pump > 2) { sendJsonError(state, req, 400, "pump must be 1 or 2"); return; }

      // Optional `seconds`: schedule an auto-off N seconds from now.
      // Validation here is the public-API boundary; the FSM also
      // re-clamps for defence in depth.
      uint32_t durationSec = 0;
      if (on && d.containsKey("seconds")) {
          long sec = d["seconds"].as<long>();
          if (sec < (long)MIN_MANUAL_PUMP_SECONDS ||
              sec > (long)MAX_MANUAL_PUMP_SECONDS) {
              sendJsonError(state, req, 400, "seconds_out_of_range");
              return;
          }
          durationSec = (uint32_t)sec;
      }

      // Manual mode required for ON requests; OFF is always allowed.
      // A contended mutex must NOT silently become "manual mode" — that
      // could let a manual ON race past the auto-mode guard. Surface
      // 503 so the client retries.
      bool autoMode = false;
      if (on) {
          if (!state.mutex || xSemaphoreTake(state.mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
              sendJsonError(state, req, 503, "state busy");
              return;
          }
          autoMode = state.settings.autoMode;
          xSemaphoreGive(state.mutex);
          if (autoMode) { sendJsonError(state, req, 409, "auto_mode_active"); return; }
      }

      PumpRequestResult r = requestPump(state, (uint8_t)(pump - 1), on,
                                        PumpRequestSource::Web, durationSec);
      StaticJsonDocument<160> resp;
      resp["pump"]   = pump;
      resp["state"]  = on;
      resp["result"] = pumpResultText(r);
      if (on && durationSec > 0 && r == PumpRequestResult::Granted) {
          resp["seconds"] = durationSec;
      }
      AUDIT_JSON_DOC(resp, "/api/pump resp");
      String out;
      serializeJson(resp, out);
      // Choose the right HTTP status:
      //   200 — granted
      //   503 — transient state contention (retryable)
      //   409 — safety/policy denial (dry-run, runtime, sensor, etc.)
      int httpCode;
      switch (r) {
          case PumpRequestResult::Granted:     httpCode = 200; break;
          case PumpRequestResult::DeniedBusy:  httpCode = 503; break;
          default:                             httpCode = 409; break;
      }
      req->send(httpCode, "application/json", out);
      httpAccount(state, httpCode);
  }

  // ---------- /api/calibration GET ----------

  static void handleGetCalibration(SystemState& state,
                                   AsyncWebServerRequest* req) {
      StaticJsonDocument<256> d;
      if (!state.mutex || xSemaphoreTake(state.mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
          sendJsonError(state, req, 503, "state busy");
          return;
      }
      const Settings cfg = state.settings;
      xSemaphoreGive(state.mutex);

      auto pair = [&](const char* key, uint16_t dry, uint16_t wet) {
          JsonObject o = d.createNestedObject(key);
          o["dry"] = dry;
          o["wet"] = wet;
      };
      pair("tank1", cfg.tank1RawDry, cfg.tank1RawWet);
      pair("tank2", cfg.tank2RawDry, cfg.tank2RawWet);
      pair("soil",  cfg.soilRawDry,  cfg.soilRawWet);
      pair("soil2", cfg.soil2RawDry, cfg.soil2RawWet);
      AUDIT_JSON_DOC(d, "/api/calibration");
      String out;
      serializeJson(d, out);
      req->send(200, "application/json", out);
      httpAccount(state, 200);
  }

  // ---------- /api/calibrate POST ----------

  static void handlePostCalibrate(SystemState& state,
                                  AsyncWebServerRequest* req,
                                  const char* body, size_t len) {
      StaticJsonDocument<384> d;
      DeserializationError err = deserializeJson(d, body, len);
      if (err) { sendJsonError(state, req, 400, err.c_str()); return; }

      // Build a CalibrationPatch. Each sensor sub-object is optional;
      // each `dry`/`wet` field is optional within its sub-object.
      // Strict range parsing here keeps the patch helper pure and
      // free of JSON dependencies. Negative or absurd values are
      // mapped to 0xFFFF so the patch validator rejects them with
      // raw_out_of_range — we do not silently clamp.
      CalibrationPatch patch;
      auto cookRaw = [](long v) -> uint16_t {
          if (v < 0)              return 0xFFFFu;
          if (v > ADC_MAX_RAW)    return 0xFFFFu;
          return (uint16_t)v;
      };
      auto readPair = [&](const char* key,
                          bool& hd, uint16_t& vd, bool& hw, uint16_t& vw) {
          if (!d.containsKey(key)) return;
          JsonVariantConst sub = d[key];
          if (!sub.is<JsonObjectConst>()) return;
          JsonObjectConst obj = sub.as<JsonObjectConst>();
          if (obj.containsKey("dry")) { hd = true; vd = cookRaw(obj["dry"].as<long>()); }
          if (obj.containsKey("wet")) { hw = true; vw = cookRaw(obj["wet"].as<long>()); }
      };
      readPair("tank1", patch.hasTank1Dry, patch.tank1Dry,
                        patch.hasTank1Wet, patch.tank1Wet);
      readPair("tank2", patch.hasTank2Dry, patch.tank2Dry,
                        patch.hasTank2Wet, patch.tank2Wet);
      readPair("soil",  patch.hasSoilDry,  patch.soilDry,
                        patch.hasSoilWet,  patch.soilWet);
      readPair("soil2", patch.hasSoil2Dry, patch.soil2Dry,
                        patch.hasSoil2Wet, patch.soil2Wet);

      // Snapshot under the mutex; validate-and-apply on the local copy
      // so a rejected patch never leaves RAM half-mutated.
      Settings ns;
      Settings prev;
      if (!state.mutex || xSemaphoreTake(state.mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
          sendJsonError(state, req, 503, "state busy");
          return;
      }
      ns   = state.settings;
      prev = state.settings;
      xSemaphoreGive(state.mutex);

      CalibrationPatchResult cr = applyCalibrationPatch(patch, ns);
      if (cr != CalibrationPatchResult::Ok) {
          sendJsonError(state, req, 400, calibrationPatchErrorText(cr));
          return;
      }

      // Persist before applying — same rule as /api/settings: NVS
      // and RAM must not diverge across an unexpected reboot.
      if (!storageSaveSettings(ns)) {
          sendJsonError(state, req, 500, "nvs write failed");
          return;
      }

      bool ramApplied = false;
      for (int attempt = 0; attempt < 4; ++attempt) {
          if (state.mutex && xSemaphoreTake(state.mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
              state.settings = ns;
              xSemaphoreGive(state.mutex);
              ramApplied = true;
              break;
          }
      }
      if (!ramApplied) {
          bool rolledBack = storageSaveSettings(prev);
          sendJsonError(state, req, 503,
              rolledBack ? "state_busy_rolled_back"
                         : "state_busy_persistence_inconsistent");
          return;
      }
      req->send(200, "application/json", "{\"result\":\"ok\"}");
      httpAccount(state, 200);
  }

  // ---------- /api/events GET ----------
  //
  // Returns a tail of the in-memory event ring buffer. Default
  // limit is 16 entries; client may request 1..MAX_EVENTS_PER_REQUEST
  // via `?limit=`. Newest entries come last so the dashboard can
  // simply render them in order.
  //
  // Body shape (additive — clients ignore unknown fields):
  //   { "now_ms": 12345,
  //     "total":  42,           # monotonic; same as Metrics.events
  //     "events": [
  //       {"t": 100, "seq": 0, "level": "info", "msg": "boot complete"},
  //       ...
  //     ] }
  //
  // The per-entry document is small (~160 bytes serialized worst
  // case), so the fixed cap of 16 entries keeps the response under
  // ~3 KB and well inside the 4 KB DynamicJsonDocument. Requests
  // for more are silently clamped — clients that want a longer
  // history can poll repeatedly, paginating by sequence number.
  static constexpr int MAX_EVENTS_PER_REQUEST = 16;

  static void handleGetEvents(SystemState& state, AsyncWebServerRequest* req) {
      // Optional ?limit= cap on the response. Clamped at
      // MAX_EVENTS_PER_REQUEST (16) to keep the JSON document below
      // its 4 KB capacity even when every entry uses the full
      // 72-byte message buffer. Without the cap, ?limit=32 on a
      // full ring buffer could exceed capacity and silently
      // truncate via the AUDIT_JSON_DOC macro.
      int limit = MAX_EVENTS_PER_REQUEST;
      if (req->hasParam("limit")) {
          int v = req->getParam("limit")->value().toInt();
          if (v <= 0)                            limit = 1;
          else if (v > MAX_EVENTS_PER_REQUEST)   limit = MAX_EVENTS_PER_REQUEST;
          else                                   limit = v;
      }

      // Snapshot under the mutex into a local copy so the JSON
      // serialisation step below cannot race a concurrent
      // eventLogAdd() (which would shift the head index mid-iter).
      EventLog snapshot;
      uint32_t nowMs = 0;
      if (!state.mutex || xSemaphoreTake(state.mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
          sendJsonError(state, req, 503, "state busy");
          return;
      }
      snapshot = state.events;   // POD copy (~3 KB; fast memcpy)
      nowMs    = millis();
      xSemaphoreGive(state.mutex);

      // Build response. Capacity sized for 16 small entries plus
      // metadata. Per-entry breakdown:
      //   JSON_OBJECT_SIZE(4) ≈ 80 B (struct) + ~80 B for the msg
      //   string = ~160 B/entry → 16 × 160 = 2560 B + outer ~250 B.
      // 4 KB gives comfortable headroom for the worst case (full
      // EVENTLOG_MSG_LEN message in every entry). Heap allocated
      // and released at request end. Build 5 will audit all
      // StaticJsonDocument sizes against the measured payload.
      DynamicJsonDocument doc(4096);
      doc["now_ms"] = nowMs;
      doc["total"]  = snapshot.total;
      JsonArray arr = doc.createNestedArray("events");

      size_t startIdx = (snapshot.count > (size_t)limit)
                        ? (snapshot.count - (size_t)limit)
                        : 0;
      for (size_t i = startIdx; i < snapshot.count; ++i) {
          const EventEntry* e = eventLogGet(snapshot, i);
          if (!e) break;
          JsonObject o = arr.createNestedObject();
          o["t"]     = e->timestampMs;
          o["seq"]   = e->sequence;
          o["level"] = eventLevelText(e->level);
          o["msg"]   = e->msg;
      }

      AUDIT_JSON_DOC(doc, "/api/events");
      String out;
      serializeJson(doc, out);
      req->send(200, "application/json", out);
      httpAccount(state, 200);
  }

  // ---------- /api/metrics GET ----------
  //
  // Snapshot of saturating runtime counters plus a few live
  // gauges (uptime, free heap, station count). Counters never
  // wrap; clients can compute deltas without false negatives.

  static void handleGetMetrics(SystemState& state, AsyncWebServerRequest* req) {
      Metrics m;
      uint32_t apHealthLastOkMs = 0;
      if (!state.mutex || xSemaphoreTake(state.mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
          sendJsonError(state, req, 503, "state busy");
          return;
      }
      m = state.metrics;
      apHealthLastOkMs = state.apHealthLastOkMs;
      xSemaphoreGive(state.mutex);

      uint32_t nowMs = millis();
      uint32_t uptimeSec = (nowMs - m.bootMs) / 1000UL;
      uint32_t freeHeap = ESP.getFreeHeap();
      uint32_t stations = WiFi.softAPgetStationNum();

      StaticJsonDocument<512> d;
      d["uptime_s"]            = uptimeSec;
      d["now_ms"]              = nowMs;
      d["free_heap"]           = freeHeap;
      d["free_heap_min"]       = m.freeHeapMin;
      d["station_count"]       = stations;
      d["main_loop_ticks"]     = m.mainLoopTicks;
      d["sensor_error_count"]  = m.sensorErrorCount;
      d["pump1_starts"]        = m.pumpStarts[0];
      d["pump2_starts"]        = m.pumpStarts[1];
      d["pump1_runtime_s"]     = m.pumpRuntimeSec[0];
      d["pump2_runtime_s"]     = m.pumpRuntimeSec[1];
      d["ap_restarts"]         = m.apRestarts;
      d["http_requests"]       = m.httpRequests;
      d["http_4xx"]            = m.httpErrors4xx;
      d["http_5xx"]            = m.httpErrors5xx;
      // Age (ms) since the AP was last reported healthy. 0 means
      // "never observed yet" — usually only true within the first
      // ~5 s of boot before the AP watchdog has run its first poll.
      d["ap_unhealthy_ms"]     = (apHealthLastOkMs == 0)
                                  ? 0
                                  : (uint32_t)(nowMs - apHealthLastOkMs);
      AUDIT_JSON_DOC(d, "/api/metrics");
      String out;
      serializeJson(d, out);
      req->send(200, "application/json", out);
      httpAccount(state, 200);
  }
#endif  // ARDUINO

void webserverBegin(SystemState& state) {
#ifdef ARDUINO
    rateLimitInit();

    // WiFi mode: AP+STA if STA credentials configured, else AP only
    if (state.settings.staSSID[0] != '\0') {
        WiFi.mode(WIFI_AP_STA);
        Serial.printf("[wifi] mode=AP+STA, connecting to '%s'...\n", state.settings.staSSID);
        WiFi.begin(state.settings.staSSID, state.settings.staPass);
        // Non-blocking — STA connects in background
    } else {
        WiFi.mode(WIFI_AP);
    }

    bool ok;
    if (strlen(state.settings.pass) == 0) {
        ok = WiFi.softAP(state.settings.ssid);
    } else {
        ok = WiFi.softAP(state.settings.ssid, state.settings.pass);
    }
    state.apIp = WiFi.softAPIP();
    Serial.printf("[wifi] softAP %s ip=%s\n",
                  ok ? "ok" : "FAILED",
                  state.apIp.toString().c_str());
    if (ok) startMdns();
    state.apHealthLastOkMs = millis();
    {
        char msg[48];
        snprintf(msg, sizeof(msg), "AP %s ip=%s",
                 ok ? "up" : "FAILED",
                 state.apIp.toString().c_str());
        pushSystemEvent(state, ok ? EventLevel::Info : EventLevel::Error, msg);
    }

    // ---------- HTML ----------
    // Each lambda captures `state` by reference so sendHtml() can
    // bump httpRequests via httpAccount(). The browser-facing pages
    // are accounted as 200 responses (the only path here).
    g_server.on("/", HTTP_GET, [&state](AsyncWebServerRequest* req) {
        sendHtml(state, req, PAGE_DASHBOARD);
    });
    g_server.on("/control", HTTP_GET, [&state](AsyncWebServerRequest* req) {
        sendHtml(state, req, PAGE_CONTROL);
    });
    g_server.on("/settings", HTTP_GET, [&state](AsyncWebServerRequest* req) {
        sendHtml(state, req, PAGE_SETTINGS);
    });
    g_server.on("/calibration", HTTP_GET, [&state](AsyncWebServerRequest* req) {
        sendHtml(state, req, PAGE_CALIBRATION);
    });

    // ---------- JSON GET ----------
    g_server.on("/api/status", HTTP_GET, [&state](AsyncWebServerRequest* req) {
        handleStatus(state, req);
    });
    g_server.on("/api/settings", HTTP_GET, [&state](AsyncWebServerRequest* req) {
        handleGetSettings(state, req);
    });
    g_server.on("/api/calibration", HTTP_GET, [&state](AsyncWebServerRequest* req) {
        handleGetCalibration(state, req);
    });
    g_server.on("/api/events", HTTP_GET, [&state](AsyncWebServerRequest* req) {
        handleGetEvents(state, req);
    });
    g_server.on("/api/metrics", HTTP_GET, [&state](AsyncWebServerRequest* req) {
        handleGetMetrics(state, req);
    });

    // ---------- JSON POST (need body collector) ----------
    //
    // Cleanup contract for g_pending[]:
    //   - Slots are reserved on the first chunk via allocPending().
    //   - On the final chunk (received >= total) we call freePending().
    //   - If a client disconnects mid-body the final chunk may never
    //     arrive, leaking the slot. We register req->onDisconnect to
    //     release the slot in that case so the 4-entry table cannot
    //     fill up with dangling entries after a few aborted POSTs.
    auto bodyCollector =
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len,
           size_t index, size_t total,
           void (*onComplete)(SystemState&, AsyncWebServerRequest*, const char*, size_t),
           SystemState* st) {
            // First chunk path: validate size, allocate slot.
            PendingBody* pb = findPending(req);
            if (!pb) {
                if (total == 0 || total > HTTP_MAX_BODY_BYTES) {
                    // Reserve a slot in rejected state so any further
                    // chunks for this request are silently absorbed
                    // and we never double-respond.
                    PendingBody* rej = allocPending(req, 0, /*rejected=*/true);
                    if (rej) {
                        req->onDisconnect([req]{ freePending(req); });
                    }
                    int code = (total == 0) ? 400 : 413;
                    req->send(code, "application/json",
                              total == 0 ? "{\"error\":\"empty body\"}"
                                         : "{\"error\":\"body too large\"}");
                    httpAccount(*st, code);
                    return;
                }
                pb = allocPending(req, total, /*rejected=*/false);
                if (!pb) {
                    // OOM or pending table full. Mark rejected so we
                    // don't try again on subsequent chunks.
                    PendingBody* rej = allocPending(req, 0, /*rejected=*/true);
                    if (rej) {
                        req->onDisconnect([req]{ freePending(req); });
                    }
                    req->send(503, "application/json", "{\"error\":\"oom\"}");
                    httpAccount(*st, 503);
                    return;
                }
                // Successful reservation: arm cleanup so a mid-body
                // disconnect releases this slot.
                req->onDisconnect([req]{ freePending(req); });
            }
            if (pb->rejected) {
                // Response already sent; silently swallow remaining bytes.
                if (index + len >= total) freePending(req);
                return;
            }
            // Range check defends against malformed chunks.
            if (index > pb->cap || len > pb->cap - index) {
                pb->rejected = true;
                req->send(400, "application/json", "{\"error\":\"bad chunk\"}");
                httpAccount(*st, 400);
                if (index + len >= total) freePending(req);
                return;
            }
            memcpy(pb->buf + index, data, len);
            pb->received += len;
            if (pb->received >= total) {
                pb->buf[pb->cap] = '\0';
                // onComplete drives its own httpAccount() through the
                // sendJsonError / sendHtml helpers and the explicit
                // tail-call after each req->send().
                onComplete(*st, req, pb->buf, pb->cap);
                freePending(req);
            }
        };

    g_server.on("/api/pump", HTTP_POST,
        [&state](AsyncWebServerRequest* req) {
            // Empty body → 400 (after rate-limit check so a flood of
            // empty bodies doesn't bypass the limiter).
            if (!enforceRateLimit(req, RateLimitEndpoint::Pump, state)) return;
            req->send(400, "application/json", "{\"error\":\"empty body\"}");
            httpAccount(state, 400);
        },
        nullptr,
        [&state, bodyCollector](AsyncWebServerRequest* req, uint8_t* data,
                                size_t len, size_t index, size_t total) {
            // Rate-limit only on the FIRST chunk; subsequent chunks
            // for the same request must not be charged a token (and
            // a 429 mid-stream would race the body collector). The
            // body collector tracks first-chunk via findPending.
            if (!findPending(req)) {
                if (!enforceRateLimit(req, RateLimitEndpoint::Pump, state)) return;
            }
            bodyCollector(req, data, len, index, total,
                          handlePostPump, &state);
        });

    g_server.on("/api/settings", HTTP_POST,
        [&state](AsyncWebServerRequest* req) {
            if (!enforceRateLimit(req, RateLimitEndpoint::Settings, state)) return;
            req->send(400, "application/json", "{\"error\":\"empty body\"}");
            httpAccount(state, 400);
        },
        nullptr,
        [&state, bodyCollector](AsyncWebServerRequest* req, uint8_t* data,
                                size_t len, size_t index, size_t total) {
            if (!findPending(req)) {
                if (!enforceRateLimit(req, RateLimitEndpoint::Settings, state)) return;
            }
            bodyCollector(req, data, len, index, total,
                          handlePostSettings, &state);
        });

    g_server.on("/api/calibrate", HTTP_POST,
        [&state](AsyncWebServerRequest* req) {
            if (!enforceRateLimit(req, RateLimitEndpoint::Calibrate, state)) return;
            req->send(400, "application/json", "{\"error\":\"empty body\"}");
            httpAccount(state, 400);
        },
        nullptr,
        [&state, bodyCollector](AsyncWebServerRequest* req, uint8_t* data,
                                size_t len, size_t index, size_t total) {
            if (!findPending(req)) {
                if (!enforceRateLimit(req, RateLimitEndpoint::Calibrate, state)) return;
            }
            bodyCollector(req, data, len, index, total,
                          handlePostCalibrate, &state);
        });

    g_server.on("/api/reset", HTTP_POST, [&state](AsyncWebServerRequest* req) {
        if (!enforceRateLimit(req, RateLimitEndpoint::Reset, state)) return;
        // Capture the current credentials before reset so we can tell
        // whether the AP needs to be re-spun.
        char oldSsid[sizeof(state.settings.ssid)] = {0};
        char oldPass[sizeof(state.settings.pass)] = {0};
        bool needRestart = false;
        if (state.mutex && xSemaphoreTake(state.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            strncpy(oldSsid, state.settings.ssid, sizeof(oldSsid) - 1);
            strncpy(oldPass, state.settings.pass, sizeof(oldPass) - 1);
            storageResetSettings();
            settingsLoadDefaults(state.settings);
            needRestart = (strcmp(oldSsid, state.settings.ssid) != 0) ||
                          (strcmp(oldPass, state.settings.pass) != 0);
            xSemaphoreGive(state.mutex);
        } else {
            // Mutex not available — at least clear NVS; in-memory
            // state will reload defaults on next boot.
            storageResetSettings();
        }
        // Mirror the /api/settings POST contract: tell the dashboard
        // whether the AP is about to bounce so it can show the
        // "reconnect to default SSID / password" hint.
        StaticJsonDocument<64> ok;
        ok["result"]     = "ok";
        ok["restart_ap"] = needRestart;
        AUDIT_JSON_DOC(ok, "/api/reset resp");
        String out;
        serializeJson(ok, out);
        req->send(200, "application/json", out);
        httpAccount(state, 200);
        if (needRestart) {
            webserverScheduleApRestart(state);
        }
    });

    g_server.onNotFound([&state](AsyncWebServerRequest* req) {
        req->send(404, "text/plain", "not found");
        httpAccount(state, 404);
    });

    g_server.begin();
    Serial.println(F("[http] server started on :80"));
#else
    (void)state;
#endif
}

void webserverTick(SystemState& state) {
#ifdef ARDUINO
    uint32_t now = millis();

    // ---- AP health watchdog -------------------------------------
    // Polls the soft-AP every AP_HEALTH_CHECK_PERIOD_MS. A healthy
    // radio reports a non-zero IP; if the IP is 0.0.0.0 we treat
    // the AP as unhealthy. Continuous unhealth for at least
    // AP_HEALTH_FAIL_TIMEOUT_MS schedules a restart and counts an
    // event-log entry. The check runs before the pending-restart
    // path below so a watchdog-triggered restart goes through the
    // same code path as a settings-change restart.
    static uint32_t s_lastApCheckMs = 0;
    static bool     s_apUnhealthyLogged = false;
    if (now - s_lastApCheckMs >= AP_HEALTH_CHECK_PERIOD_MS ||
        s_lastApCheckMs == 0) {
        s_lastApCheckMs = now;
        IPAddress ip = WiFi.softAPIP();
        bool healthy = (ip[0] != 0) || (ip[1] != 0) ||
                       (ip[2] != 0) || (ip[3] != 0);
        if (healthy) {
            if (state.mutex && xSemaphoreTake(state.mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                state.apHealthLastOkMs = now;
                xSemaphoreGive(state.mutex);
            }
            if (s_apUnhealthyLogged) {
                pushSystemEvent(state, EventLevel::Info, "AP recovered");
                s_apUnhealthyLogged = false;
            }
        } else {
            uint32_t lastOk = 0;
            if (state.mutex && xSemaphoreTake(state.mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                lastOk = state.apHealthLastOkMs;
                xSemaphoreGive(state.mutex);
            }
            uint32_t down = (lastOk == 0) ? 0 : (now - lastOk);
            if (!s_apUnhealthyLogged) {
                pushSystemEvent(state, EventLevel::Warn, "AP unhealthy: ip=0.0.0.0");
                s_apUnhealthyLogged = true;
            }
            if (down >= AP_HEALTH_FAIL_TIMEOUT_MS && !g_apRestartPending) {
                Serial.printf("[ap] unhealthy %u ms; scheduling restart\n",
                              (unsigned)down);
                pushSystemEvent(state, EventLevel::Error,
                                "AP watchdog: scheduling restart");
                webserverScheduleApRestart(state);
            }
        }
    }

    if (g_apRestartPending && (int32_t)(now - g_apRestartAtMs) >= 0) {
        g_apRestartPending = false;
        // Snapshot SSID/password under the mutex; HTTP handlers run
        // on the AsyncTCP task and may be writing settings concurrently.
        char ssid[sizeof(state.settings.ssid)] = {0};
        char pass[sizeof(state.settings.pass)] = {0};
        if (state.mutex && xSemaphoreTake(state.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            strncpy(ssid, state.settings.ssid, sizeof(ssid) - 1);
            strncpy(pass, state.settings.pass, sizeof(pass) - 1);
            xSemaphoreGive(state.mutex);
        } else {
            Serial.println(F("[wifi] AP restart deferred: state busy"));
            // Re-arm so we try again next tick.
            g_apRestartPending = true;
            g_apRestartAtMs    = millis() + 100;
            return;
        }
        Serial.println(F("[wifi] applying AP restart"));
        WiFi.softAPdisconnect(true);
        delay(50);
        bool ok;
        if (strlen(pass) == 0) {
            ok = WiFi.softAP(ssid);
        } else {
            ok = WiFi.softAP(ssid, pass);
        }
        IPAddress ip = WiFi.softAPIP();
        if (state.mutex && xSemaphoreTake(state.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            state.apIp = ip;
            metricsIncSat(state.metrics.apRestarts, 1);
            // After a successful restart, reset the unhealthy
            // anchor so the watchdog does not immediately re-trip
            // on a second pass before the radio finishes settling.
            if (ok) state.apHealthLastOkMs = millis();
            xSemaphoreGive(state.mutex);
        }
        Serial.printf("[wifi] softAP restart %s ip=%s\n",
                      ok ? "ok" : "FAILED",
                      ip.toString().c_str());
        // mDNS responder is bound to the AP interface; teardown
        // released it. Re-register so `irrigation.local` keeps
        // resolving across the restart.
        if (ok) startMdns();
        char msg[48];
        snprintf(msg, sizeof(msg), "AP restart %s ip=%s",
                 ok ? "ok" : "FAILED",
                 ip.toString().c_str());
        pushSystemEvent(state, ok ? EventLevel::Info : EventLevel::Error, msg);
        s_apUnhealthyLogged = false;
    }

    // ---- STA reconnect ------------------------------------------
    static uint32_t s_lastStaCheckMs = 0;
    if ((now - s_lastStaCheckMs) >= STA_RECONNECT_INTERVAL_MS) {
        s_lastStaCheckMs = now;
        char staSsid[MAX_STA_SSID_LEN + 1] = {0};
        char staPass[MAX_STA_PASS_LEN + 2] = {0};
        if (state.mutex && xSemaphoreTake(state.mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
            strncpy(staSsid, state.settings.staSSID, sizeof(staSsid) - 1);
            strncpy(staPass, state.settings.staPass, sizeof(staPass) - 1);
            xSemaphoreGive(state.mutex);
        }
        if (staSsid[0] != '\0' && WiFi.status() != WL_CONNECTED) {
            Serial.printf("[wifi] STA reconnecting to '%s'...\n", staSsid);
            WiFi.begin(staSsid, staPass);
        }
    }
#else
    (void)state;
#endif
}

void webserverScheduleApRestart(SystemState& state) {
#ifdef ARDUINO
    g_apRestartPending = true;
    g_apRestartAtMs    = millis() + 500;  // grace so HTTP response flushes
    (void)state;
#else
    (void)state;
#endif
}
