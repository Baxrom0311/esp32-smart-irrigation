"""Smart Irrigation AI Server.

Single-file FastAPI backend for the ESP32 Smart Irrigation project.

Responsibilities:
  * Receive sensor reports from the ESP32 every ~30s and respond with
    pump commands (existing API contract is preserved).
  * Use DeepSeek to make pump decisions and answer chat questions about
    irrigation. Falls back to local threshold rules when AI is unavailable.
  * Keep an in-memory history of the last N reports for the dashboard.
  * Allow manual pump overrides from the dashboard. Overrides are stored
    on the server and applied on every ESP32 report.
  * Allow runtime configuration of thresholds (soil low/high, tank min)
    that drive both the AI prompt and the local fallback rules.
  * Serve a self-contained dark-theme web dashboard at "/" with
    inline HTML/CSS/JS (no external CDNs) — works offline on a LAN.

Environment variables (loaded from .env):
  DEEPSEEK_API_KEY  DeepSeek API key. If empty/placeholder, AI is skipped
                    and local rules are used.
  DEVICE_API_KEY    Optional shared secret. If set, the ESP32 must send
                    it in the Authorization header.
"""

from __future__ import annotations

import asyncio
import json
import os
import time
from collections import deque
from typing import Any, Deque

import httpx
from dotenv import load_dotenv
from fastapi import FastAPI, HTTPException, Request
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import HTMLResponse, JSONResponse
from pydantic import BaseModel, Field

load_dotenv()

DEEPSEEK_API_KEY = os.getenv("DEEPSEEK_API_KEY", "")
DEVICE_API_KEY = os.getenv("DEVICE_API_KEY", "")
DEEPSEEK_URL = "https://api.deepseek.com/chat/completions"

# Treat the placeholder as "unset".
AI_ENABLED = bool(DEEPSEEK_API_KEY) and DEEPSEEK_API_KEY != "your-deepseek-api-key-here"

HISTORY_MAX = 100
ONLINE_WINDOW_SECONDS = 90  # ESP32 reports every ~30s; allow 3 misses.
AI_MIN_INTERVAL_SECONDS = 25  # Re-use last AI decision if called more often.

app = FastAPI(title="Smart Irrigation AI Server", version="1.0.0")
app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_methods=["*"],
    allow_headers=["*"],
)


# ---------------------------------------------------------------------------
# In-memory state
# ---------------------------------------------------------------------------

# Default thresholds — used by both the AI prompt and the local fallback.
settings: dict[str, Any] = {
    "soil_low": 35,        # below this => start watering
    "soil_high": 60,       # above this => stop watering
    "tank_min": 20,        # below this => never run pump (dry-run protection)
    "max_pump_minutes": 30,
}

# pump_overrides[i] in {"auto", "on", "off"} — auto means AI decides.
pump_overrides: dict[int, str] = {1: "auto", 2: "auto"}

latest_data: dict[str, Any] = {}
last_report_time: float = 0.0
last_decision: dict[str, Any] = {
    "pump1": False,
    "pump2": False,
    "reason": "no data yet",
    "source": "init",      # "ai" | "rules" | "init"
    "overrides": {"1": "auto", "2": "auto"},
    "ts": 0.0,
}
history: Deque[dict[str, Any]] = deque(maxlen=HISTORY_MAX)
chat_history: Deque[dict[str, str]] = deque(maxlen=20)

# Lock around AI calls so we don't fire many in parallel for the same report.
_ai_lock = asyncio.Lock()
_last_ai_call_ts: float = 0.0
_last_ai_result: dict[str, Any] | None = None


# ---------------------------------------------------------------------------
# Pydantic models
# ---------------------------------------------------------------------------

class SensorReport(BaseModel):
    soil1: int = 0
    soil2: int = 0
    tank1: int = 0
    tank2: int = 0
    temp: float | None = None
    hum: float | None = None
    tank1_err: bool = False
    tank2_err: bool = False
    soil1_err: bool = False
    soil2_err: bool = False
    pump1: bool = False
    pump2: bool = False


class ChatRequest(BaseModel):
    message: str = Field(min_length=1, max_length=500)


class OverrideRequest(BaseModel):
    pump: int = Field(ge=1, le=2)
    mode: str = Field(pattern="^(auto|on|off)$")


class SettingsRequest(BaseModel):
    soil_low: int | None = Field(default=None, ge=0, le=100)
    soil_high: int | None = Field(default=None, ge=0, le=100)
    tank_min: int | None = Field(default=None, ge=0, le=100)
    max_pump_minutes: int | None = Field(default=None, ge=1, le=240)


# ---------------------------------------------------------------------------
# DeepSeek helper
# ---------------------------------------------------------------------------

async def ask_deepseek(
    system_prompt: str,
    user_msg: str,
    *,
    temperature: float = 0.3,
    max_tokens: int = 250,
) -> str:
    """Call DeepSeek chat-completions. Return content string or "" on failure."""
    if not AI_ENABLED:
        return ""
    try:
        async with httpx.AsyncClient(timeout=20) as client:
            resp = await client.post(
                DEEPSEEK_URL,
                headers={
                    "Authorization": f"Bearer {DEEPSEEK_API_KEY}",
                    "Content-Type": "application/json",
                },
                json={
                    "model": "deepseek-chat",
                    "messages": [
                        {"role": "system", "content": system_prompt},
                        {"role": "user", "content": user_msg},
                    ],
                    "temperature": temperature,
                    "max_tokens": max_tokens,
                },
            )
            if resp.status_code == 200:
                return resp.json()["choices"][0]["message"]["content"]
            print(f"[deepseek] HTTP {resp.status_code}: {resp.text[:200]}")
    except Exception as exc:  # noqa: BLE001
        print(f"[deepseek] error: {exc}")
    return ""


# ---------------------------------------------------------------------------
# Decision engine
# ---------------------------------------------------------------------------

def _build_system_prompt() -> str:
    s = settings
    return (
        "You are an irrigation AI controller for a 2-zone smart greenhouse. "
        "Each zone has its own soil moisture sensor, water tank, and pump.\n\n"
        "Decision rules:\n"
        f"- If soil moisture < {s['soil_low']}%, the zone needs water (pump ON).\n"
        f"- If soil moisture > {s['soil_high']}%, the zone is wet enough (pump OFF).\n"
        f"- If a tank is below {s['tank_min']}% NEVER run that zone's pump (dry-run protection).\n"
        "- If a sensor has an error, keep that zone's pump OFF (safe default).\n"
        "- Consider air temperature/humidity: hot+dry weather increases need.\n"
        "- Pump 1 is tied to soil1 + tank1. Pump 2 is tied to soil2 + tank2. "
        "Decide each zone independently.\n\n"
        'Respond ONLY with compact JSON of the form '
        '{"pump1": true|false, "pump2": true|false, "reason": "<=120 chars"}. '
        "No markdown, no extra text."
    )


def _local_rules(data: SensorReport) -> tuple[bool, bool, str]:
    """Threshold-based fallback when AI is unavailable."""
    s = settings
    pump1 = (
        not data.soil1_err
        and not data.tank1_err
        and data.soil1 < s["soil_low"]
        and data.tank1 >= s["tank_min"]
    )
    pump2 = (
        not data.soil2_err
        and not data.tank2_err
        and data.soil2 < s["soil_low"]
        and data.tank2 >= s["tank_min"]
    )
    reason = (
        f"local rules: soil1={data.soil1}<{s['soil_low']}? -> pump1={pump1}; "
        f"soil2={data.soil2}<{s['soil_low']}? -> pump2={pump2}"
    )
    return pump1, pump2, reason


async def _ai_decision(data: SensorReport) -> dict[str, Any] | None:
    """Ask DeepSeek for a decision. Cached briefly to avoid quota burn."""
    global _last_ai_call_ts, _last_ai_result

    async with _ai_lock:
        now = time.time()
        if (
            _last_ai_result is not None
            and now - _last_ai_call_ts < AI_MIN_INTERVAL_SECONDS
        ):
            return _last_ai_result

        user_msg = (
            f"Sensor reading:\n"
            f"  zone 1: soil={data.soil1}% tank={data.tank1}%"
            f" soil_err={data.soil1_err} tank_err={data.tank1_err}\n"
            f"  zone 2: soil={data.soil2}% tank={data.tank2}%"
            f" soil_err={data.soil2_err} tank_err={data.tank2_err}\n"
            f"  air: temp={data.temp}C hum={data.hum}%\n"
            "Decide pump states now."
        )
        text = await ask_deepseek(_build_system_prompt(), user_msg)
        if not text:
            return None
        try:
            # DeepSeek sometimes wraps JSON in ``` fences; strip defensively.
            stripped = text.strip()
            if stripped.startswith("```"):
                stripped = stripped.strip("`")
                if stripped.lower().startswith("json"):
                    stripped = stripped[4:]
                stripped = stripped.strip()
            parsed = json.loads(stripped)
            result = {
                "pump1": bool(parsed.get("pump1", False)),
                "pump2": bool(parsed.get("pump2", False)),
                "reason": str(parsed.get("reason", "AI decision"))[:160],
            }
            _last_ai_result = result
            _last_ai_call_ts = now
            return result
        except (json.JSONDecodeError, KeyError, TypeError) as exc:
            print(f"[ai] failed to parse: {exc}; raw={text[:200]!r}")
            return None


def _apply_safety_and_overrides(
    pump1: bool,
    pump2: bool,
    data: SensorReport,
) -> tuple[bool, bool, list[str]]:
    """Final guardrails. Returns (pump1, pump2, notes)."""
    notes: list[str] = []
    s = settings

    # Manual override beats AI/rules but never beats safety.
    o1 = pump_overrides.get(1, "auto")
    o2 = pump_overrides.get(2, "auto")
    if o1 == "on":
        pump1 = True
        notes.append("override:pump1=on")
    elif o1 == "off":
        pump1 = False
        notes.append("override:pump1=off")
    if o2 == "on":
        pump2 = True
        notes.append("override:pump2=on")
    elif o2 == "off":
        pump2 = False
        notes.append("override:pump2=off")

    # Hard safety: dry-run + sensor error => force OFF.
    # BUT: Force ON override bypasses safety (operator knows best).
    if o1 != "on":
        if data.tank1_err or data.tank1 < s["tank_min"]:
            if pump1:
                notes.append(f"safety:pump1 off (tank1={data.tank1}% err={data.tank1_err})")
            pump1 = False
    if o2 != "on":
        if data.tank2_err or data.tank2 < s["tank_min"]:
            if pump2:
                notes.append(f"safety:pump2 off (tank2={data.tank2}% err={data.tank2_err})")
            pump2 = False
    if data.soil1_err and pump1 and o1 != "on":
        notes.append("safety:pump1 off (soil1 sensor error)")
        pump1 = False
    if data.soil2_err and pump2 and o2 != "on":
        notes.append("safety:pump2 off (soil2 sensor error)")
        pump2 = False

    return pump1, pump2, notes


# ---------------------------------------------------------------------------
# Endpoints — device API
# ---------------------------------------------------------------------------

@app.post("/api/device/report")
async def device_report(data: SensorReport, request: Request):
    """ESP32 sends sensor data here, gets pump commands back."""
    global latest_data, last_report_time, last_decision

    auth = request.headers.get("Authorization", "")
    if DEVICE_API_KEY and auth != DEVICE_API_KEY:
        raise HTTPException(status_code=401, detail="unauthorized")

    now = time.time()
    latest_data = data.model_dump()
    last_report_time = now

    # Append to ring buffer for dashboard chart.
    history.append({"ts": now, **latest_data})

    # Decide pumps.
    ai_result = await _ai_decision(data)
    if ai_result is not None:
        pump1 = ai_result["pump1"]
        pump2 = ai_result["pump2"]
        reason = ai_result["reason"]
        source = "ai"
    else:
        pump1, pump2, reason = _local_rules(data)
        source = "rules"

    pump1, pump2, notes = _apply_safety_and_overrides(pump1, pump2, data)
    if notes:
        reason = f"{reason} | {'; '.join(notes)}"

    last_decision = {
        "pump1": pump1,
        "pump2": pump2,
        "reason": reason,
        "source": source,
        "overrides": {str(k): v for k, v in pump_overrides.items()},
        "ts": now,
    }

    return {"pump1": pump1, "pump2": pump2, "reason": reason}


# ---------------------------------------------------------------------------
# Endpoints — dashboard API
# ---------------------------------------------------------------------------

@app.get("/api/status")
async def status():
    age = time.time() - last_report_time if last_report_time else None
    return {
        "data": latest_data,
        "last_report": last_report_time,
        "age_seconds": round(age, 1) if age is not None else None,
        "online": age is not None and age < ONLINE_WINDOW_SECONDS,
        "decision": last_decision,
        "overrides": {str(k): v for k, v in pump_overrides.items()},
        "settings": settings,
        "ai_enabled": AI_ENABLED,
    }


@app.get("/api/history")
async def get_history(limit: int = HISTORY_MAX):
    limit = max(1, min(limit, HISTORY_MAX))
    return {"items": list(history)[-limit:], "count": len(history)}


@app.post("/api/override")
async def post_override(body: OverrideRequest):
    pump_overrides[body.pump] = body.mode
    return {"ok": True, "overrides": {str(k): v for k, v in pump_overrides.items()}}


@app.get("/api/settings")
async def get_settings():
    return settings


@app.post("/api/settings")
async def post_settings(body: SettingsRequest):
    payload = body.model_dump(exclude_none=True)
    if not payload:
        raise HTTPException(status_code=400, detail="no settings provided")

    new_low = payload.get("soil_low", settings["soil_low"])
    new_high = payload.get("soil_high", settings["soil_high"])
    if new_low >= new_high:
        raise HTTPException(
            status_code=400,
            detail="soil_low must be less than soil_high",
        )

    settings.update(payload)
    # Drop AI cache so the new prompt takes effect on next report.
    global _last_ai_result
    _last_ai_result = None
    return {"ok": True, "settings": settings}


# ---------------------------------------------------------------------------
# Endpoints — AI chat
# ---------------------------------------------------------------------------

CHAT_SYSTEM_PROMPT = (
    "You are a friendly smart-irrigation assistant for a small greenhouse with "
    "two independent zones (soil sensor + water tank + pump per zone). "
    "You speak Uzbek, Russian, and English fluently — reply in the same "
    "language the user used. Be concise, practical, and use emoji for status: "
    "✅ normal, ⚠️ warning, 🔴 critical. "
    "Avoid lengthy disclaimers; give the user actionable advice."
)


def _chat_context() -> str:
    """Snapshot of current state injected into every chat call."""
    s = settings
    parts = [
        f"Thresholds: soil_low={s['soil_low']}%, soil_high={s['soil_high']}%, "
        f"tank_min={s['tank_min']}%, max_pump_minutes={s['max_pump_minutes']}.",
    ]
    if latest_data:
        d = latest_data
        parts.append(
            f"Latest sensors: soil1={d.get('soil1')}%, soil2={d.get('soil2')}%, "
            f"tank1={d.get('tank1')}%, tank2={d.get('tank2')}%, "
            f"temp={d.get('temp')}C, hum={d.get('hum')}%."
        )
        errs = [k for k in ("tank1_err", "tank2_err", "soil1_err", "soil2_err") if d.get(k)]
        if errs:
            parts.append(f"Sensor errors: {', '.join(errs)}.")
    if last_decision and last_decision.get("ts"):
        parts.append(
            f"Last pump decision: pump1={last_decision['pump1']}, "
            f"pump2={last_decision['pump2']} ({last_decision['source']}). "
            f"Reason: {last_decision['reason']}"
        )
    return "\n".join(parts)


@app.post("/api/ai/chat")
async def ai_chat(body: ChatRequest):
    user_msg = body.message.strip()
    chat_history.append({"role": "user", "content": user_msg})

    if not AI_ENABLED:
        reply = (
            "🔴 AI server sozlanmagan. .env faylida DEEPSEEK_API_KEY ni to‘ldiring "
            "va serverni qayta ishga tushiring."
        )
        chat_history.append({"role": "assistant", "content": reply})
        return {"reply": reply, "ai_enabled": False}

    full_prompt = CHAT_SYSTEM_PROMPT + "\n\nContext:\n" + _chat_context()
    reply = await ask_deepseek(full_prompt, user_msg, temperature=0.5, max_tokens=400)
    if not reply:
        reply = "⚠️ AI bilan aloqa o‘rnatib bo‘lmadi. Keyinroq qayta urinib ko‘ring."
    chat_history.append({"role": "assistant", "content": reply})
    return {"reply": reply, "ai_enabled": True}


# ---------------------------------------------------------------------------
# Health
# ---------------------------------------------------------------------------

@app.get("/healthz")
async def healthz():
    return JSONResponse({"ok": True, "ai": AI_ENABLED, "history": len(history)})


# ---------------------------------------------------------------------------
# Web dashboard
# ---------------------------------------------------------------------------

@app.get("/", response_class=HTMLResponse)
async def dashboard():
    return DASHBOARD_HTML


DASHBOARD_HTML = r"""<!DOCTYPE html>
<html lang="uz">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>🌱 Smart Irrigation AI</title>
<style>
:root{
  color-scheme:dark;
  --bg:#0b1220;
  --card:#111c2e;
  --card2:#152339;
  --line:#1d2a44;
  --txt:#e6edf3;
  --muted:#8aa0b8;
  --ok:#3ad29f;
  --warn:#fdba74;
  --err:#ff6b6b;
  --accent:#1f6feb;
}
*{box-sizing:border-box}
html,body{margin:0;padding:0}
body{
  font-family:system-ui,-apple-system,Segoe UI,Roboto,sans-serif;
  background:var(--bg);
  color:var(--txt);
  min-height:100vh;
}
header{
  background:var(--card);
  padding:14px 20px;
  display:flex;
  justify-content:space-between;
  align-items:center;
  border-bottom:1px solid var(--line);
  position:sticky;top:0;z-index:10;
}
header h1{margin:0;font-size:18px;font-weight:600}
header .pill{
  font-size:12px;padding:5px 10px;border-radius:999px;
  background:var(--card2);border:1px solid var(--line);
}
main{padding:18px;max-width:1200px;margin:0 auto;display:grid;gap:18px}
section{background:var(--card);border-radius:14px;padding:16px;border:1px solid var(--line)}
section h2{margin:0 0 12px;font-size:14px;color:var(--muted);text-transform:uppercase;letter-spacing:.5px;font-weight:600}
.row{display:grid;gap:12px}
.cards{grid-template-columns:repeat(auto-fit,minmax(170px,1fr))}
.card{
  background:var(--card2);border-radius:10px;padding:14px;
  border:1px solid var(--line);position:relative;overflow:hidden;
}
.card .label{font-size:12px;color:var(--muted);text-transform:uppercase;letter-spacing:.5px}
.card .val{font-size:28px;font-weight:600;margin-top:4px;line-height:1}
.card .val .unit{font-size:14px;color:var(--muted);margin-left:3px}
.bar{height:6px;background:var(--line);border-radius:3px;margin-top:10px;overflow:hidden}
.bar>div{height:100%;background:var(--ok);transition:width .4s ease, background-color .3s}
.dot{display:inline-block;width:9px;height:9px;border-radius:50%;margin-right:6px;background:var(--muted);vertical-align:middle}
.dot.ok{background:var(--ok)}.dot.warn{background:var(--warn)}.dot.err{background:var(--err)}
.ok{color:var(--ok)}.warn{color:var(--warn)}.err{color:var(--err)}.muted{color:var(--muted)}
.pumps{grid-template-columns:repeat(auto-fit,minmax(280px,1fr))}
.pump{
  background:var(--card2);border-radius:10px;padding:14px;border:1px solid var(--line);
}
.pump-head{display:flex;justify-content:space-between;align-items:center}
.pump-state{font-size:22px;font-weight:600}
.pump-state.on{color:var(--ok)}.pump-state.off{color:var(--muted)}
.pump-state.pending{color:var(--warn);animation:blink 0.8s infinite}
@keyframes blink{0%,100%{opacity:1}50%{opacity:0.3}}
.pump-meta{font-size:12px;color:var(--muted);margin-top:4px}
.btns{display:flex;gap:6px;margin-top:10px}
.btn{
  flex:1;padding:8px 10px;border-radius:8px;border:1px solid var(--line);
  background:var(--card);color:var(--txt);cursor:pointer;font-size:13px;
  transition:background .15s, border-color .15s;
}
.btn:hover{background:#1a2a44}
.btn.active{background:var(--accent);border-color:var(--accent)}
.btn.on.active{background:var(--ok);border-color:var(--ok);color:#0b1220}
.btn.off.active{background:var(--err);border-color:var(--err);color:#fff}
.statusbar{
  display:flex;flex-wrap:wrap;gap:14px;align-items:center;
  background:var(--card2);padding:10px 14px;border-radius:10px;border:1px solid var(--line);
  font-size:13px;
}
.statusbar .item{display:flex;align-items:center;gap:6px}
#chart-wrap{position:relative;width:100%;height:240px}
#chart{width:100%;height:100%;display:block}
.legend{display:flex;flex-wrap:wrap;gap:14px;margin-top:8px;font-size:12px;color:var(--muted)}
.legend span{display:inline-flex;align-items:center;gap:6px}
.legend i{width:14px;height:3px;border-radius:2px;display:inline-block}
.chat{display:flex;flex-direction:column;gap:10px}
#msgs{
  max-height:340px;min-height:140px;overflow-y:auto;display:flex;flex-direction:column;gap:8px;
  background:var(--bg);border:1px solid var(--line);border-radius:10px;padding:10px;
}
.msg{padding:8px 12px;border-radius:10px;max-width:85%;white-space:pre-wrap;word-wrap:break-word;line-height:1.4;font-size:14px}
.msg.user{background:var(--accent);align-self:flex-end;color:#fff}
.msg.ai{background:var(--card2);align-self:flex-start;border:1px solid var(--line)}
.msg.sys{background:transparent;color:var(--muted);font-size:12px;align-self:center;text-align:center}
.chat-input{display:flex;gap:8px}
.chat-input input,.chat-input textarea{
  flex:1;padding:10px 12px;border-radius:10px;border:1px solid var(--line);
  background:var(--bg);color:var(--txt);font-family:inherit;font-size:14px;
}
.chat-input button{
  background:var(--accent);color:#fff;border:0;padding:10px 18px;border-radius:10px;
  cursor:pointer;font-weight:600;
}
.chat-input button:disabled{opacity:.5;cursor:not-allowed}
.settings-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:12px}
.field{display:flex;flex-direction:column;gap:4px}
.field label{font-size:12px;color:var(--muted);text-transform:uppercase;letter-spacing:.4px}
.field input{
  padding:9px 10px;border-radius:8px;border:1px solid var(--line);
  background:var(--bg);color:var(--txt);font-size:14px;
}
.settings-actions{display:flex;justify-content:flex-end;margin-top:12px;gap:8px}
.toast{
  position:fixed;bottom:18px;right:18px;background:var(--card2);
  border:1px solid var(--line);padding:10px 14px;border-radius:10px;
  font-size:13px;display:none;max-width:320px;
}
.toast.show{display:block}
.toast.ok{border-color:var(--ok)}.toast.err{border-color:var(--err)}
@media (max-width:600px){
  header h1{font-size:16px}
  .card .val{font-size:22px}
  main{padding:12px;gap:12px}
  section{padding:12px}
}
</style>
</head>
<body>
<header>
  <h1>🌱 Smart Irrigation AI</h1>
  <span id="conn" class="pill"><span class="dot"></span><span id="conn-text">connecting…</span></span>
</header>

<main>
  <div class="statusbar">
    <div class="item"><strong>Device:</strong> <span id="device-state" class="muted">unknown</span></div>
    <div class="item"><strong>Last seen:</strong> <span id="last-seen" class="muted">—</span></div>
    <div class="item"><strong>AI:</strong> <span id="ai-state" class="muted">—</span></div>
    <div class="item"><strong>Decision:</strong> <span id="decision-source" class="muted">—</span></div>
  </div>

  <section>
    <h2>Sensors</h2>
    <div class="row cards">
      <div class="card"><div class="label">Soil 1 (Zone 1)</div><div class="val"><span id="s1">--</span><span class="unit">%</span></div><div class="bar"><div id="b-s1" style="width:0"></div></div></div>
      <div class="card"><div class="label">Soil 2 (Zone 2)</div><div class="val"><span id="s2">--</span><span class="unit">%</span></div><div class="bar"><div id="b-s2" style="width:0"></div></div></div>
      <div class="card"><div class="label">Tank 1</div><div class="val"><span id="t1">--</span><span class="unit">%</span></div><div class="bar"><div id="b-t1" style="width:0"></div></div></div>
      <div class="card"><div class="label">Tank 2</div><div class="val"><span id="t2">--</span><span class="unit">%</span></div><div class="bar"><div id="b-t2" style="width:0"></div></div></div>
      <div class="card"><div class="label">Temperature</div><div class="val"><span id="temp">--</span><span class="unit">°C</span></div><div class="bar"><div id="b-temp" style="width:0"></div></div></div>
      <div class="card"><div class="label">Air humidity</div><div class="val"><span id="hum">--</span><span class="unit">%</span></div><div class="bar"><div id="b-hum" style="width:0"></div></div></div>
    </div>
  </section>

  <section>
    <h2>Pumps</h2>
    <div class="row pumps">
      <div class="pump">
        <div class="pump-head">
          <div><div class="label muted" style="font-size:12px">Pump 1 (Zone 1)</div><div id="p1-state" class="pump-state off">OFF</div></div>
          <div class="muted" style="font-size:12px;text-align:right" id="p1-mode">auto</div>
        </div>
        <div class="pump-meta" id="p1-reason">—</div>
        <div class="btns">
          <button class="btn" data-pump="1" data-mode="auto">Auto</button>
          <button class="btn on" data-pump="1" data-mode="on">Force ON</button>
          <button class="btn off" data-pump="1" data-mode="off">Force OFF</button>
        </div>
      </div>
      <div class="pump">
        <div class="pump-head">
          <div><div class="label muted" style="font-size:12px">Pump 2 (Zone 2)</div><div id="p2-state" class="pump-state off">OFF</div></div>
          <div class="muted" style="font-size:12px;text-align:right" id="p2-mode">auto</div>
        </div>
        <div class="pump-meta" id="p2-reason">—</div>
        <div class="btns">
          <button class="btn" data-pump="2" data-mode="auto">Auto</button>
          <button class="btn on" data-pump="2" data-mode="on">Force ON</button>
          <button class="btn off" data-pump="2" data-mode="off">Force OFF</button>
        </div>
      </div>
    </div>
  </section>

  <section>
    <h2>History (last 100 reports)</h2>
    <div id="chart-wrap">
      <svg id="chart" viewBox="0 0 800 240" preserveAspectRatio="none"></svg>
    </div>
    <div class="legend">
      <span><i style="background:#3ad29f"></i> Soil 1</span>
      <span><i style="background:#5ac8fa"></i> Soil 2</span>
      <span><i style="background:#fdba74"></i> Tank 1</span>
      <span><i style="background:#c084fc"></i> Tank 2</span>
      <span class="muted" id="hist-count">0 points</span>
    </div>
  </section>

  <section>
    <h2>🤖 AI Assistant</h2>
    <div class="chat">
      <div id="msgs">
        <div class="msg sys">Salom! Sug‘orish bo‘yicha savol bering. Masalan: «Bugun pomidorni sug‘orsam bo‘ladimi?»</div>
      </div>
      <div class="chat-input">
        <input id="inp" placeholder="Savol bering…" autocomplete="off" onkeydown="if(event.key==='Enter')sendMsg()">
        <button id="send-btn" onclick="sendMsg()">→</button>
      </div>
    </div>
  </section>

  <section>
    <h2>⚙️ Settings (thresholds)</h2>
    <div class="settings-grid">
      <div class="field"><label>Soil low (water below)</label><input type="number" id="set-soil-low" min="0" max="100"></div>
      <div class="field"><label>Soil high (stop above)</label><input type="number" id="set-soil-high" min="0" max="100"></div>
      <div class="field"><label>Tank min (dry-run)</label><input type="number" id="set-tank-min" min="0" max="100"></div>
      <div class="field"><label>Max pump minutes</label><input type="number" id="set-max-min" min="1" max="240"></div>
    </div>
    <div class="settings-actions">
      <button class="btn" style="flex:0 0 auto;padding:9px 18px" onclick="saveSettings()">Save</button>
    </div>
  </section>
</main>

<div id="toast" class="toast"></div>

<script>
const $ = id => document.getElementById(id);
let lastSettings = null;

// ---------- helpers ----------
function fmtAge(sec){
  if(sec==null) return 'never';
  if(sec<60) return Math.round(sec)+'s ago';
  if(sec<3600) return Math.round(sec/60)+'m ago';
  return Math.round(sec/3600)+'h ago';
}
function setBar(id,pct,kind){
  const el = $(id); if(!el) return;
  pct = Math.max(0, Math.min(100, pct||0));
  el.style.width = pct+'%';
  let color = 'var(--ok)';
  if(kind==='soil'){
    if(pct < (lastSettings?.soil_low ?? 35)) color = 'var(--err)';
    else if(pct > (lastSettings?.soil_high ?? 60)) color = 'var(--warn)';
  } else if(kind==='tank'){
    if(pct < (lastSettings?.tank_min ?? 20)) color = 'var(--err)';
    else if(pct < 40) color = 'var(--warn)';
  } else if(kind==='temp'){
    color = (pct<10||pct>35) ? 'var(--err)' : (pct<15||pct>30) ? 'var(--warn)' : 'var(--ok)';
  } else if(kind==='hum'){
    color = (pct<25||pct>85) ? 'var(--warn)' : 'var(--ok)';
  }
  el.style.background = color;
}
function showToast(msg, kind){
  const t = $('toast');
  t.textContent = msg;
  t.className = 'toast show ' + (kind||'');
  clearTimeout(showToast._t);
  showToast._t = setTimeout(()=>{ t.className='toast'; }, 2500);
}

// ---------- status polling ----------
async function poll(){
  try{
    const r = await fetch('/api/status');
    if(!r.ok) throw new Error('http '+r.status);
    const d = await r.json();
    lastSettings = d.settings || lastSettings;

    // header pill
    const online = !!d.online;
    $('conn').firstElementChild.className = 'dot ' + (online?'ok':'err');
    $('conn-text').textContent = online ? 'online' : 'offline';
    $('device-state').textContent = online ? 'online' : 'offline';
    $('device-state').className = online ? 'ok' : 'err';
    $('last-seen').textContent = fmtAge(d.age_seconds);
    $('ai-state').textContent = d.ai_enabled ? 'enabled' : 'disabled';
    $('ai-state').className = d.ai_enabled ? 'ok' : 'warn';

    // sensor cards
    const s = d.data || {};
    const set = (id, v, decimals) => {
      const el = $(id);
      if(v==null || v===undefined){ el.textContent='--'; return; }
      el.textContent = (decimals!=null) ? Number(v).toFixed(decimals) : v;
    };
    set('s1', s.soil1); setBar('b-s1', s.soil1, 'soil');
    set('s2', s.soil2); setBar('b-s2', s.soil2, 'soil');
    set('t1', s.tank1); setBar('b-t1', s.tank1, 'tank');
    set('t2', s.tank2); setBar('b-t2', s.tank2, 'tank');
    set('temp', s.temp, 1); setBar('b-temp', ((s.temp ?? 0) / 50) * 100, 'temp');
    set('hum', s.hum, 0); setBar('b-hum', s.hum, 'hum');

    // sensor-error coloring (override)
    if(s.soil1_err){ $('s1').textContent='ERR'; $('s1').className='err'; } else $('s1').className='';
    if(s.soil2_err){ $('s2').textContent='ERR'; $('s2').className='err'; } else $('s2').className='';
    if(s.tank1_err){ $('t1').textContent='ERR'; $('t1').className='err'; } else $('t1').className='';
    if(s.tank2_err){ $('t2').textContent='ERR'; $('t2').className='err'; } else $('t2').className='';

    // pumps
    const dec = d.decision || {};
    const ovr = d.overrides || {'1':'auto','2':'auto'};
    function paintPump(num){
      const on = !!dec['pump'+num];
      const el = $('p'+num+'-state');
      el.textContent = on ? 'ON' : 'OFF';
      el.className = 'pump-state ' + (on?'on':'off');
      $('p'+num+'-reason').textContent = dec.reason || '—';
      $('p'+num+'-mode').textContent = 'mode: ' + (ovr[String(num)]||'auto');
      // highlight active override button
      document.querySelectorAll('.btn[data-pump="'+num+'"]').forEach(b=>{
        b.classList.toggle('active', b.dataset.mode === (ovr[String(num)]||'auto'));
      });
    }
    paintPump(1); paintPump(2);

    $('decision-source').textContent = dec.source || '—';
    $('decision-source').className = dec.source==='ai' ? 'ok' : (dec.source==='rules' ? 'warn' : 'muted');

    // settings (only fill if user is not currently editing)
    if(lastSettings && document.activeElement.tagName !== 'INPUT'){
      $('set-soil-low').value  = lastSettings.soil_low;
      $('set-soil-high').value = lastSettings.soil_high;
      $('set-tank-min').value  = lastSettings.tank_min;
      $('set-max-min').value   = lastSettings.max_pump_minutes;
    }
  } catch(e){
    $('conn').firstElementChild.className = 'dot err';
    $('conn-text').textContent = 'server error';
  }
}

// ---------- history chart (vanilla SVG) ----------
async function refreshChart(){
  try{
    const r = await fetch('/api/history?limit=100');
    const d = await r.json();
    const pts = d.items || [];
    $('hist-count').textContent = pts.length + ' points';
    drawChart(pts);
  } catch(e){ /* ignore */ }
}
function drawChart(pts){
  const svg = $('chart');
  const W = 800, H = 240, pad = 24;
  svg.innerHTML = '';
  // background grid
  for(let i=0;i<=4;i++){
    const y = pad + i*(H-2*pad)/4;
    const ln = document.createElementNS('http://www.w3.org/2000/svg','line');
    ln.setAttribute('x1',pad); ln.setAttribute('x2',W-pad);
    ln.setAttribute('y1',y); ln.setAttribute('y2',y);
    ln.setAttribute('stroke','#1d2a44'); ln.setAttribute('stroke-width','1');
    svg.appendChild(ln);
    const txt = document.createElementNS('http://www.w3.org/2000/svg','text');
    txt.setAttribute('x',4); txt.setAttribute('y',y+3);
    txt.setAttribute('fill','#8aa0b8'); txt.setAttribute('font-size','10');
    txt.textContent = (100-i*25)+'%';
    svg.appendChild(txt);
  }
  if(pts.length<2){
    const t = document.createElementNS('http://www.w3.org/2000/svg','text');
    t.setAttribute('x',W/2); t.setAttribute('y',H/2);
    t.setAttribute('fill','#8aa0b8'); t.setAttribute('text-anchor','middle');
    t.setAttribute('font-size','13'); t.textContent = 'Waiting for sensor data…';
    svg.appendChild(t);
    return;
  }
  const series = [
    {key:'soil1', color:'#3ad29f'},
    {key:'soil2', color:'#5ac8fa'},
    {key:'tank1', color:'#fdba74'},
    {key:'tank2', color:'#c084fc'},
  ];
  const n = pts.length;
  series.forEach(s=>{
    let path = '';
    pts.forEach((p,i)=>{
      const v = p[s.key];
      if(v==null || isNaN(v)) return;
      const x = pad + (i/(n-1))*(W-2*pad);
      const y = pad + (1 - (Math.max(0,Math.min(100,v))/100))*(H-2*pad);
      path += (path ? ' L' : 'M') + x.toFixed(1) + ' ' + y.toFixed(1);
    });
    if(path){
      const el = document.createElementNS('http://www.w3.org/2000/svg','path');
      el.setAttribute('d', path);
      el.setAttribute('stroke', s.color);
      el.setAttribute('stroke-width','2');
      el.setAttribute('fill','none');
      el.setAttribute('stroke-linejoin','round');
      el.setAttribute('stroke-linecap','round');
      svg.appendChild(el);
    }
  });
}

// ---------- override buttons ----------
// State-based flow: button pressed → disabled + loading → wait for ESP32 confirmation
const pendingPumps = {1: false, 2: false};

document.querySelectorAll('.btn[data-pump]').forEach(b=>{
  b.addEventListener('click', async ()=>{
    const pump = parseInt(b.dataset.pump,10);
    const mode = b.dataset.mode;
    if(pendingPumps[pump]) return; // already waiting

    // Disable all buttons for this pump
    pendingPumps[pump] = true;
    const pumpBtns = document.querySelectorAll(`.btn[data-pump="${pump}"]`);
    pumpBtns.forEach(btn=>{ btn.disabled=true; btn.style.opacity='0.5'; });

    // Show loading state
    const stateEl = $('p'+pump+'-state');
    const prevText = stateEl.textContent;
    stateEl.textContent = mode==='on' ? 'YOQILMOQDA...' : mode==='off' ? 'O`CHIRILMOQDA...' : 'KUTILMOQDA...';
    stateEl.className = 'pump-state pending';

    try{
      const r = await fetch('/api/override',{
        method:'POST',
        headers:{'content-type':'application/json'},
        body:JSON.stringify({pump,mode}),
      });
      if(!r.ok) throw new Error('http '+r.status);

      // Wait for ESP32 to confirm (poll until state changes, max 45s)
      const wantOn = mode==='on';
      const wantOff = mode==='off';
      let confirmed = false;
      for(let i=0; i<15; i++){
        await new Promise(ok=>setTimeout(ok,3000));
        const sr = await fetch('/api/status');
        const sd = await sr.json();
        const actual = pump===1 ? sd.data.pump1 : sd.data.pump2;
        if(mode==='auto' || (wantOn && actual) || (wantOff && !actual)){
          confirmed = true;
          break;
        }
      }
      if(confirmed){
        showToast('Pump '+pump+' → '+mode+' ✅', 'ok');
      } else {
        showToast('Pump '+pump+': buyruq yuborildi, ESP32 javob kutilmoqda', 'warn');
      }
    }catch(e){
      showToast('Override failed: '+e.message, 'err');
      stateEl.textContent = prevText;
    }finally{
      pendingPumps[pump] = false;
      pumpBtns.forEach(btn=>{ btn.disabled=false; btn.style.opacity='1'; });
      poll();
    }
  });
});

// ---------- settings ----------
async function saveSettings(){
  const body = {
    soil_low:  parseInt($('set-soil-low').value,10),
    soil_high: parseInt($('set-soil-high').value,10),
    tank_min:  parseInt($('set-tank-min').value,10),
    max_pump_minutes: parseInt($('set-max-min').value,10),
  };
  try{
    const r = await fetch('/api/settings',{
      method:'POST',
      headers:{'content-type':'application/json'},
      body:JSON.stringify(body),
    });
    const d = await r.json();
    if(!r.ok) throw new Error(d.detail || 'save failed');
    lastSettings = d.settings;
    showToast('Settings saved', 'ok');
  }catch(e){ showToast('Save failed: '+e.message, 'err'); }
}

// ---------- chat ----------
function addMsg(role, text){
  const d = document.createElement('div');
  d.className = 'msg ' + role;
  d.textContent = text;
  $('msgs').appendChild(d);
  $('msgs').scrollTop = $('msgs').scrollHeight;
  return d;
}
async function sendMsg(){
  const inp = $('inp'); const m = inp.value.trim(); if(!m) return;
  inp.value = '';
  addMsg('user', m);
  const loading = addMsg('ai', '…');
  $('send-btn').disabled = true;
  try{
    const r = await fetch('/api/ai/chat',{
      method:'POST',
      headers:{'content-type':'application/json'},
      body:JSON.stringify({message:m}),
    });
    const d = await r.json();
    loading.textContent = d.reply || 'Bo‘sh javob';
  }catch(e){
    loading.textContent = "Server bilan aloqa yuq";
  }finally{
    $('send-btn').disabled = false;
    inp.focus();
  }
}

// ---------- boot ----------
poll();
refreshChart();
setInterval(poll, 3000);
setInterval(refreshChart, 15000);
</script>
</body>
</html>
"""


if __name__ == "__main__":
    import uvicorn

    uvicorn.run(app, host="0.0.0.0", port=8000)
