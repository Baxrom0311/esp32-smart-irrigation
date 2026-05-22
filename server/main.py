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
import pathlib
import re
import time
from contextlib import asynccontextmanager
from typing import Any

import aiosqlite
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

HISTORY_DEFAULT_LIMIT = 200       # default for GET /api/history
HISTORY_MAX_LIMIT = 1000          # hard cap on GET /api/history?limit=
ONLINE_WINDOW_SECONDS = 90        # ESP32 reports every ~30s; allow 3 misses.
AI_MIN_INTERVAL_SECONDS = 25      # Re-use last AI decision if called more often.
HISTORY_RETENTION_DAYS = 30       # delete sensor_history rows older than this
CLEANUP_INTERVAL_SECONDS = 3600   # background cleanup cadence

# SQLite database lives next to this file; auto-created on first run.
DB_PATH = pathlib.Path(__file__).resolve().parent / "irrigation.db"


# ---------------------------------------------------------------------------
# In-memory state (only what's needed for fast /api/status — everything else
# is persisted in SQLite via the helpers below).
# ---------------------------------------------------------------------------

# Default thresholds — used by both the AI prompt and the local fallback.
# These are loaded from / saved to the `settings` table at runtime.
settings: dict[str, Any] = {
    "soil_low": 35,        # below this => start watering
    "soil_high": 60,       # above this => stop watering
    "tank_min": 20,        # below this => never run pump (dry-run protection)
    "max_pump_minutes": 30,
}

# pump_overrides[i] in {"auto", "on", "off"} — auto means AI decides.
# Loaded from / saved to the `overrides` table at runtime.
pump_overrides: dict[int, str] = {1: "auto", 2: "auto"}

# Cached for fast /api/status responses (no DB query needed for polling).
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

# Lock around AI calls so we don't fire many in parallel for the same report.
_ai_lock = asyncio.Lock()
_last_ai_call_ts: float = 0.0
_last_ai_result: dict[str, Any] | None = None


# ---------------------------------------------------------------------------
# SQLite layer (raw SQL, single shared aiosqlite connection)
# ---------------------------------------------------------------------------

# A single shared async connection. aiosqlite serializes operations on its
# internal worker thread, so concurrent FastAPI request handlers are safe.
_db: aiosqlite.Connection | None = None
_cleanup_task: asyncio.Task[None] | None = None

_SCHEMA_SQL = """
CREATE TABLE IF NOT EXISTS sensor_history (
    id        INTEGER PRIMARY KEY AUTOINCREMENT,
    ts        REAL    NOT NULL,
    soil1     INTEGER,
    soil2     INTEGER,
    tank1     INTEGER,
    tank2     INTEGER,
    temp      REAL,
    hum       REAL,
    tank1_err INTEGER,
    tank2_err INTEGER,
    soil1_err INTEGER,
    soil2_err INTEGER,
    pump1     INTEGER,
    pump2     INTEGER
);
CREATE INDEX IF NOT EXISTS idx_sensor_history_ts ON sensor_history(ts);

CREATE TABLE IF NOT EXISTS settings (
    key   TEXT PRIMARY KEY,
    value TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS chat_history (
    id      INTEGER PRIMARY KEY AUTOINCREMENT,
    ts      REAL    NOT NULL,
    role    TEXT    NOT NULL,
    content TEXT    NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_chat_history_ts ON chat_history(ts);

CREATE TABLE IF NOT EXISTS overrides (
    pump INTEGER PRIMARY KEY,
    mode TEXT    NOT NULL
);
"""


async def _get_db() -> aiosqlite.Connection:
    """Return the shared connection, opening it lazily."""
    global _db
    if _db is None:
        _db = await aiosqlite.connect(str(DB_PATH))
        _db.row_factory = aiosqlite.Row
    return _db


async def _init_db() -> None:
    db = await _get_db()
    await db.executescript(_SCHEMA_SQL)
    await db.commit()


async def _load_settings_from_db() -> None:
    """Populate the in-memory `settings` dict from the DB."""
    db = await _get_db()
    async with db.execute("SELECT key, value FROM settings") as cur:
        rows = await cur.fetchall()
    for row in rows:
        try:
            settings[row["key"]] = json.loads(row["value"])
        except (json.JSONDecodeError, TypeError) as exc:
            print(f"[db] invalid settings row {row['key']!r}: {exc}")


async def _save_settings_to_db(payload: dict[str, Any]) -> None:
    """Upsert each provided setting key/value."""
    if not payload:
        return
    db = await _get_db()
    await db.executemany(
        "INSERT INTO settings(key, value) VALUES(?, ?) "
        "ON CONFLICT(key) DO UPDATE SET value = excluded.value",
        [(k, json.dumps(v)) for k, v in payload.items()],
    )
    await db.commit()


async def _load_overrides_from_db() -> None:
    db = await _get_db()
    async with db.execute("SELECT pump, mode FROM overrides") as cur:
        rows = await cur.fetchall()
    for row in rows:
        pump = int(row["pump"])
        mode = str(row["mode"])
        if pump in (1, 2) and mode in ("auto", "on", "off"):
            pump_overrides[pump] = mode


async def _save_override_to_db(pump: int, mode: str) -> None:
    db = await _get_db()
    await db.execute(
        "INSERT INTO overrides(pump, mode) VALUES(?, ?) "
        "ON CONFLICT(pump) DO UPDATE SET mode = excluded.mode",
        (pump, mode),
    )
    await db.commit()


async def _insert_sensor_history(ts: float, data: dict[str, Any]) -> None:
    db = await _get_db()
    await db.execute(
        "INSERT INTO sensor_history "
        "(ts, soil1, soil2, tank1, tank2, temp, hum, "
        " tank1_err, tank2_err, soil1_err, soil2_err, pump1, pump2) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
        (
            ts,
            data.get("soil1"),
            data.get("soil2"),
            data.get("tank1"),
            data.get("tank2"),
            data.get("temp"),
            data.get("hum"),
            int(bool(data.get("tank1_err"))),
            int(bool(data.get("tank2_err"))),
            int(bool(data.get("soil1_err"))),
            int(bool(data.get("soil2_err"))),
            int(bool(data.get("pump1"))),
            int(bool(data.get("pump2"))),
        ),
    )
    await db.commit()


async def _insert_chat_message(role: str, content: str) -> None:
    db = await _get_db()
    await db.execute(
        "INSERT INTO chat_history(ts, role, content) VALUES(?, ?, ?)",
        (time.time(), role, content),
    )
    await db.commit()


async def _fetch_history(limit: int) -> list[dict[str, Any]]:
    """Return the most recent `limit` rows in chronological order (oldest first).

    The chart code expects ascending time order, so we query DESC + LIMIT and
    reverse the result.
    """
    db = await _get_db()
    async with db.execute(
        "SELECT ts, soil1, soil2, tank1, tank2, temp, hum, "
        "       tank1_err, tank2_err, soil1_err, soil2_err, pump1, pump2 "
        "FROM sensor_history ORDER BY ts DESC LIMIT ?",
        (limit,),
    ) as cur:
        rows = await cur.fetchall()
    items: list[dict[str, Any]] = []
    for row in reversed(rows):
        items.append({
            "ts": row["ts"],
            "soil1": row["soil1"],
            "soil2": row["soil2"],
            "tank1": row["tank1"],
            "tank2": row["tank2"],
            "temp": row["temp"],
            "hum": row["hum"],
            "tank1_err": bool(row["tank1_err"]) if row["tank1_err"] is not None else False,
            "tank2_err": bool(row["tank2_err"]) if row["tank2_err"] is not None else False,
            "soil1_err": bool(row["soil1_err"]) if row["soil1_err"] is not None else False,
            "soil2_err": bool(row["soil2_err"]) if row["soil2_err"] is not None else False,
            "pump1": bool(row["pump1"]) if row["pump1"] is not None else False,
            "pump2": bool(row["pump2"]) if row["pump2"] is not None else False,
        })
    return items


async def _count_history() -> int:
    db = await _get_db()
    async with db.execute("SELECT COUNT(*) AS n FROM sensor_history") as cur:
        row = await cur.fetchone()
    return int(row["n"]) if row is not None else 0


async def _cleanup_old_history() -> int:
    """Delete sensor_history rows older than HISTORY_RETENTION_DAYS. Returns deleted row count."""
    cutoff = time.time() - HISTORY_RETENTION_DAYS * 24 * 3600
    db = await _get_db()
    cur = await db.execute("DELETE FROM sensor_history WHERE ts < ?", (cutoff,))
    deleted = cur.rowcount or 0
    await cur.close()
    await db.commit()
    return deleted


async def _cleanup_loop() -> None:
    """Background task: prune old history every CLEANUP_INTERVAL_SECONDS."""
    while True:
        try:
            await asyncio.sleep(CLEANUP_INTERVAL_SECONDS)
            deleted = await _cleanup_old_history()
            if deleted:
                print(f"[db] cleanup: deleted {deleted} old sensor_history rows")
        except asyncio.CancelledError:
            raise
        except Exception as exc:  # noqa: BLE001
            print(f"[db] cleanup error: {exc}")


# ---------------------------------------------------------------------------
# FastAPI lifespan — DB init, settings/override hydration, cleanup task
# ---------------------------------------------------------------------------

@asynccontextmanager
async def lifespan(app: FastAPI):  # noqa: ARG001
    global _cleanup_task
    await _init_db()
    await _load_settings_from_db()
    await _load_overrides_from_db()
    # Run a cleanup pass on startup so a long-stopped server doesn't keep stale rows.
    try:
        deleted = await _cleanup_old_history()
        if deleted:
            print(f"[db] startup cleanup: deleted {deleted} old sensor_history rows")
    except Exception as exc:  # noqa: BLE001
        print(f"[db] startup cleanup error: {exc}")
    _cleanup_task = asyncio.create_task(_cleanup_loop())
    try:
        yield
    finally:
        if _cleanup_task is not None:
            _cleanup_task.cancel()
            try:
                await _cleanup_task
            except (asyncio.CancelledError, Exception):  # noqa: BLE001
                pass
        global _db
        if _db is not None:
            try:
                await _db.close()
            except Exception:  # noqa: BLE001
                pass
            _db = None


app = FastAPI(title="Smart Irrigation AI Server", version="1.0.0", lifespan=lifespan)
app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_methods=["*"],
    allow_headers=["*"],
)


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

    # Persist this report to the sensor_history table (used by /api/history
    # and the dashboard chart). Failures are logged but don't break the
    # device-control loop — the ESP32 still gets its pump command back.
    try:
        await _insert_sensor_history(now, latest_data)
    except Exception as exc:  # noqa: BLE001
        print(f"[db] insert sensor_history failed: {exc}")

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
async def get_history(limit: int = HISTORY_DEFAULT_LIMIT):
    limit = max(1, min(limit, HISTORY_MAX_LIMIT))
    items = await _fetch_history(limit)
    total = await _count_history()
    return {"items": items, "count": total}


@app.post("/api/override")
async def post_override(body: OverrideRequest):
    pump_overrides[body.pump] = body.mode
    try:
        await _save_override_to_db(body.pump, body.mode)
    except Exception as exc:  # noqa: BLE001
        print(f"[db] save override failed: {exc}")
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
    try:
        await _save_settings_to_db(payload)
    except Exception as exc:  # noqa: BLE001
        print(f"[db] save settings failed: {exc}")
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
    "Reply in the SAME language the user writes in (Uzbek, Russian, or English). "
    "Be concise, practical, and use emoji for status: "
    "✅ normal, ⚠️ warning, 🔴 critical.\n\n"
    "You CAN control pumps. The server detects pump on/off commands "
    "(e.g. \"pump 1 ni yoq\", \"nasos 2 ni o'chir\", \"включи насос 1\", "
    "\"turn on pump 2\") via keyword matching BEFORE calling you, "
    "executes the override, and replies on your behalf — so when YOU are "
    "called, the user is asking something else (advice, status, explanation). "
    "If the user mentions a pump but the request is ambiguous, suggest the "
    "exact phrase to use (e.g. \"1-nasosni yoq\" or \"2-nasosni o'chir\"). "
    "Avoid lengthy disclaimers; give actionable advice.\n\n"
    "Format your response with markdown: use **bold** for important values, "
    "use bullet points with - for lists, use \\n for line breaks."
)


# --- Pump command keyword detection ---------------------------------------

# Apostrophe variants users might type for o'chir / to'xtat / etc.
_APOSTROPHES = "'`ʼʻ‘’\u02bb\u02bc"


def _normalize_apostrophes(text: str) -> str:
    """Map all curly/back-tick apostrophes to a single ASCII apostrophe."""
    out = text
    for ch in _APOSTROPHES:
        if ch != "'":
            out = out.replace(ch, "'")
    return out


def _detect_pump_command(text: str) -> tuple[str | None, int | None]:
    """Detect explicit pump on/off commands in Uzbek/English/Russian.

    Returns (mode, pump) where:
      mode: "on", "off", or None if no command detected.
      pump: 1, 2, or None if pump not specified.

    Matching is intentionally simple keyword-based; OFF tokens are checked
    first because "o'chir" is more specific than "och...".
    """
    t = _normalize_apostrophes(text.lower().strip())

    # OFF: explicit Uzbek/English shut-down verbs
    off_substrings = ["o'chir", "to'xtat", "toxtat"]
    off_words = [r"\boff\b", r"\bstop\b", r"\bвыключ\w*\b", r"\bотключ\w*\b"]

    mode: str | None = None
    if any(s in t for s in off_substrings) or any(re.search(p, t) for p in off_words):
        mode = "off"

    if mode is None:
        # ON: yoq/yondir/ishlat/ochir (without apostrophe)/on/turn on
        on_words = [
            r"\byoq\w*\b",
            r"\byondir\w*\b",
            r"\bishlat\w*\b",
            r"\bochir\w*\b",
            r"\boch\b",
            r"\bturn\s+on\b",
            r"\bon\b",
            r"\bвключ\w*\b",
        ]
        if any(re.search(p, t) for p in on_words):
            mode = "on"

    if mode is None:
        return (None, None)

    # Pump number: 2 first because "1" can appear as part of timestamps etc.
    pump: int | None = None
    pump2_patterns = [
        r"\b2\b", r"\bikkinchi\b", r"\bikki\b",
        r"pump\s*2", r"nasos\s*2", r"2[-\s]*nasos", r"\bвторой\b", r"\bвтором\b",
    ]
    pump1_patterns = [
        r"\b1\b", r"\bbirinchi\b", r"\bbir\b",
        r"pump\s*1", r"nasos\s*1", r"1[-\s]*nasos", r"\bпервый\b", r"\bпервом\b",
    ]
    if any(re.search(p, t) for p in pump2_patterns):
        pump = 2
    elif any(re.search(p, t) for p in pump1_patterns):
        pump = 1

    return (mode, pump)


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


async def _safe_chat_log(role: str, content: str) -> None:
    """Persist a chat message; log + swallow DB errors so chat keeps working."""
    try:
        await _insert_chat_message(role, content)
    except Exception as exc:  # noqa: BLE001
        print(f"[db] chat log failed: {exc}")


@app.post("/api/ai/chat")
async def ai_chat(body: ChatRequest):
    user_msg = body.message.strip()
    await _safe_chat_log("user", user_msg)

    # ----- Keyword pump control (no AI call needed) -----
    mode, pump_num = _detect_pump_command(user_msg)
    if mode is not None:
        if pump_num is None:
            reply = (
                "🤔 Qaysi nasos? Iltimos, raqamini ayting.\n"
                "Misol: \"1-nasosni yoq\" yoki \"2-nasosni o'chir\"."
            )
            await _safe_chat_log("assistant", reply)
            return {
                "reply": reply,
                "ai_enabled": AI_ENABLED,
                "action": "ask_pump",
            }

        # Apply override (same effect as Force ON/OFF buttons)
        pump_overrides[pump_num] = mode
        try:
            await _save_override_to_db(pump_num, mode)
        except Exception as exc:  # noqa: BLE001
            print(f"[db] save override failed: {exc}")
        if mode == "on":
            reply = (
                f"✅ Nasos {pump_num} ga *yoqish* buyrug'i berildi. "
                f"Qurilma keyingi aloqa paytida (~30 soniya ichida) "
                f"nasosni ishga tushiradi.\n"
                f"⚠️ Eslatma: agar tank suv darajasi {settings['tank_min']}% "
                f"dan past bo'lsa yoki sensor xato bersa, xavfsizlik tizimi "
                f"nasosni baribir ishlatmaydi."
            )
        else:
            reply = (
                f"🛑 Nasos {pump_num} ga *o'chirish* buyrug'i berildi. "
                f"Qurilma keyingi aloqa paytida (~30 soniya ichida) "
                f"nasosni to'xtatadi.\n"
                f"Avto rejimga qaytarish uchun \"Avto\" tugmasini bosing."
            )
        await _safe_chat_log("assistant", reply)
        return {
            "reply": reply,
            "ai_enabled": AI_ENABLED,
            "action": "override",
            "pump": pump_num,
            "mode": mode,
        }

    # ----- Otherwise, fall through to AI chat -----
    if not AI_ENABLED:
        reply = (
            "🔴 AI server sozlanmagan. .env faylida DEEPSEEK_API_KEY ni to'ldiring "
            "va serverni qayta ishga tushiring."
        )
        await _safe_chat_log("assistant", reply)
        return {"reply": reply, "ai_enabled": False}

    full_prompt = CHAT_SYSTEM_PROMPT + "\n\nContext:\n" + _chat_context()
    reply = await ask_deepseek(full_prompt, user_msg, temperature=0.5, max_tokens=400)
    if not reply:
        reply = "⚠️ AI bilan aloqa o'rnatib bo'lmadi. Keyinroq qayta urinib ko'ring."
    await _safe_chat_log("assistant", reply)
    return {"reply": reply, "ai_enabled": True}


# ---------------------------------------------------------------------------
# Health
# ---------------------------------------------------------------------------

@app.get("/healthz")
async def healthz():
    try:
        hist_count = await _count_history()
    except Exception as exc:  # noqa: BLE001
        print(f"[db] healthz count failed: {exc}")
        hist_count = -1
    return JSONResponse({"ok": True, "ai": AI_ENABLED, "history": hist_count})


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
  gap:10px;
  flex-wrap:wrap;
}
header h1{margin:0;font-size:18px;font-weight:600}
.header-right{display:flex;align-items:center;gap:10px;flex-wrap:wrap;justify-content:flex-end}
.lang-switch{display:flex;gap:4px}
.lang-btn{
  background:var(--card2);border:1px solid var(--line);color:var(--muted);
  padding:4px 9px;border-radius:6px;font-size:11px;font-weight:700;
  cursor:pointer;letter-spacing:.5px;transition:all .15s;
}
.lang-btn:hover{color:var(--txt);border-color:var(--accent)}
.lang-btn.active{background:var(--accent);color:#fff;border-color:var(--accent)}
header .pill{
  font-size:12px;padding:5px 10px;border-radius:999px;
  background:var(--card2);border:1px solid var(--line);
}
.refresh-btn{
  background:var(--card);border:1px solid var(--line);color:var(--muted);
  width:24px;height:24px;border-radius:6px;font-size:12px;
  cursor:pointer;padding:0;display:inline-flex;align-items:center;justify-content:center;
  transition:color .15s, border-color .15s;
}
.refresh-btn:hover{color:var(--txt);border-color:var(--accent)}
.refresh-btn.spin{animation:rspin .6s linear}
@keyframes rspin{from{transform:rotate(0deg)}to{transform:rotate(360deg)}}
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
#chart-wrap{position:relative;width:100%;height:300px}
#chart{width:100%;height:100%;display:block}
.legend{display:flex;flex-wrap:wrap;gap:8px;margin-top:10px;font-size:12px;color:var(--muted)}
.legend .lg-item{
  display:inline-flex;align-items:center;gap:6px;
  cursor:pointer;user-select:none;padding:3px 8px;border-radius:6px;
  transition:opacity .15s, color .15s, background .15s;
}
.legend .lg-item:hover{color:var(--txt);background:rgba(255,255,255,.05)}
.legend .lg-item.hidden{opacity:.4;text-decoration:line-through}
.legend i{width:14px;height:3px;border-radius:2px;display:inline-block}
.last-updated{font-size:11px;color:var(--muted);margin-top:8px;text-align:right;font-style:italic}
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
.btn-save{flex:0 0 auto;padding:9px 18px;min-width:140px;transition:background .25s, color .25s, border-color .25s}
.btn-save.saved{background:var(--ok);border-color:var(--ok);color:#0b1220}
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
  #chart-wrap{height:240px}
}
</style>
</head>
<body>
<header>
  <h1>🌱 Smart Irrigation AI</h1>
  <div class="header-right">
    <div class="lang-switch">
      <button type="button" class="lang-btn" data-lang="uz" onclick="setLang('uz')">UZ</button>
      <button type="button" class="lang-btn" data-lang="ru" onclick="setLang('ru')">RU</button>
      <button type="button" class="lang-btn" data-lang="en" onclick="setLang('en')">EN</button>
    </div>
    <span id="conn" class="pill"><span class="dot"></span><span id="conn-text">…</span></span>
  </div>
</header>

<main>
  <div class="statusbar">
    <div class="item"><strong data-i18n="device">Qurilma:</strong> <span id="device-state" class="muted">—</span></div>
    <div class="item"><strong data-i18n="last_seen">Oxirgi aloqa:</strong> <span id="last-seen" class="muted">—</span> <button id="refresh-btn" type="button" class="refresh-btn" onclick="forceRefresh()" title="Refresh">🔄</button></div>
    <div class="item"><strong data-i18n="ai_label">AI:</strong> <span id="ai-state" class="muted">—</span></div>
    <div class="item"><strong data-i18n="decision_label">Qaror:</strong> <span id="decision-source" class="muted">—</span></div>
  </div>

  <section>
    <h2 data-i18n="sensors_title">Sensorlar</h2>
    <div class="row cards">
      <div class="card"><div class="label">Soil 1 (<span data-i18n="zone1">Zona 1</span>)</div><div class="val"><span id="s1">--</span><span class="unit">%</span></div><div class="bar"><div id="b-s1" style="width:0"></div></div></div>
      <div class="card"><div class="label">Soil 2 (<span data-i18n="zone2">Zona 2</span>)</div><div class="val"><span id="s2">--</span><span class="unit">%</span></div><div class="bar"><div id="b-s2" style="width:0"></div></div></div>
      <div class="card"><div class="label">Tank 1</div><div class="val"><span id="t1">--</span><span class="unit">%</span></div><div class="bar"><div id="b-t1" style="width:0"></div></div></div>
      <div class="card"><div class="label">Tank 2</div><div class="val"><span id="t2">--</span><span class="unit">%</span></div><div class="bar"><div id="b-t2" style="width:0"></div></div></div>
      <div class="card"><div class="label" data-i18n="temperature">Harorat</div><div class="val"><span id="temp">--</span><span class="unit">°C</span></div><div class="bar"><div id="b-temp" style="width:0"></div></div></div>
      <div class="card"><div class="label" data-i18n="air_humidity">Havo namligi</div><div class="val"><span id="hum">--</span><span class="unit">%</span></div><div class="bar"><div id="b-hum" style="width:0"></div></div></div>
    </div>
  </section>

  <section>
    <h2 data-i18n="pumps_title">Nasoslar</h2>
    <div class="row pumps">
      <div class="pump">
        <div class="pump-head">
          <div><div class="label muted" style="font-size:12px"><span data-i18n="pump1_name">Nasos 1</span> (<span data-i18n="zone1">Zona 1</span>)</div><div id="p1-state" class="pump-state off">OFF</div></div>
          <div class="muted" style="font-size:12px;text-align:right" id="p1-mode">—</div>
        </div>
        <div class="pump-meta" id="p1-reason">—</div>
        <div class="btns">
          <button type="button" class="btn" data-pump="1" data-mode="auto" data-i18n="btn_auto">Avto</button>
          <button type="button" class="btn on" data-pump="1" data-mode="on" data-i18n="btn_on">Yoqish</button>
          <button type="button" class="btn off" data-pump="1" data-mode="off" data-i18n="btn_off">O'chirish</button>
        </div>
      </div>
      <div class="pump">
        <div class="pump-head">
          <div><div class="label muted" style="font-size:12px"><span data-i18n="pump2_name">Nasos 2</span> (<span data-i18n="zone2">Zona 2</span>)</div><div id="p2-state" class="pump-state off">OFF</div></div>
          <div class="muted" style="font-size:12px;text-align:right" id="p2-mode">—</div>
        </div>
        <div class="pump-meta" id="p2-reason">—</div>
        <div class="btns">
          <button type="button" class="btn" data-pump="2" data-mode="auto" data-i18n="btn_auto">Avto</button>
          <button type="button" class="btn on" data-pump="2" data-mode="on" data-i18n="btn_on">Yoqish</button>
          <button type="button" class="btn off" data-pump="2" data-mode="off" data-i18n="btn_off">O'chirish</button>
        </div>
      </div>
    </div>
  </section>

  <section>
    <h2 data-i18n="history_title">Tarix (oxirgi 100 yozuv)</h2>
    <div id="chart-wrap">
      <svg id="chart" viewBox="0 0 800 300" preserveAspectRatio="none"></svg>
    </div>
    <div class="legend" id="legend"></div>
    <div class="last-updated"><span data-i18n="last_updated_label">Oxirgi yangilanish:</span> <span id="last-updated-ts">—</span></div>
  </section>

  <section>
    <h2 data-i18n="ai_assistant_title">🤖 AI Yordamchi</h2>
    <div class="chat">
      <div id="msgs">
        <div class="msg sys" id="chat-greeting" data-i18n="chat_greeting">Salom! Sug'orish bo'yicha savol bering yoki nasoslarni boshqaring. Masalan: «1-nasosni yoq», «2-nasosni o'chir» yoki «Bugun pomidorni sug'orsam bo'ladimi?»</div>
      </div>
      <div class="chat-input">
        <input id="inp" placeholder="Savol bering yoki buyruq yozing…" autocomplete="off" onkeydown="if(event.key==='Enter')sendMsg()">
        <button id="send-btn" type="button" onclick="sendMsg()">→</button>
      </div>
    </div>
  </section>

  <section>
    <h2 data-i18n="settings_title">⚙️ Sozlamalar (chegaralar)</h2>
    <div class="settings-grid">
      <div class="field"><label data-i18n="soil_low_label">Tuproq quyi chegarasi</label><input type="number" id="set-soil-low" min="0" max="100"></div>
      <div class="field"><label data-i18n="soil_high_label">Tuproq yuqori chegarasi</label><input type="number" id="set-soil-high" min="0" max="100"></div>
      <div class="field"><label data-i18n="tank_min_label">Tank minimal</label><input type="number" id="set-tank-min" min="0" max="100"></div>
      <div class="field"><label data-i18n="max_min_label">Nasos max vaqti (min)</label><input type="number" id="set-max-min" min="1" max="240"></div>
    </div>
    <div class="settings-actions">
      <button id="save-btn" type="button" class="btn btn-save" onclick="saveSettings()" data-i18n="save">Saqlash</button>
    </div>
  </section>
</main>

<div id="toast" class="toast"></div>

<script>
// ---------- i18n ----------
const T = {
  uz: {
    conn_connecting: 'ulanmoqda…',
    conn_online: 'ulangan',
    conn_offline: 'uzilgan',
    conn_error: 'server xatosi',
    device: 'Qurilma:',
    last_seen: 'Oxirgi aloqa:',
    ai_label: 'AI:',
    decision_label: 'Qaror:',
    unknown: "noma'lum",
    ai_on: 'yoniq',
    ai_off: "o'chiq",
    src_ai: 'AI',
    src_rules: 'lokal qoidalar',
    sensors_title: 'Sensorlar',
    pumps_title: 'Nasoslar',
    history_title: 'Tarix (oxirgi 100 yozuv)',
    ai_assistant_title: '🤖 AI Yordamchi',
    settings_title: '⚙️ Sozlamalar (chegaralar)',
    zone1: 'Zona 1',
    zone2: 'Zona 2',
    pump1_name: 'Nasos 1',
    pump2_name: 'Nasos 2',
    temperature: 'Harorat',
    air_humidity: 'Havo namligi',
    btn_auto: 'Avto',
    btn_on: 'Yoqish',
    btn_off: "O'chirish",
    state_on: 'ON',
    state_off: 'OFF',
    state_turning_on: 'YOQILMOQDA...',
    state_turning_off: "O'CHIRILMOQDA...",
    state_waiting: 'KUTILMOQDA...',
    mode_prefix: 'rejim:',
    mode_auto: 'avto',
    mode_on: 'yoqish',
    mode_off: "o'chirish",
    soil_low_label: 'Tuproq quyi chegarasi',
    soil_high_label: 'Tuproq yuqori chegarasi',
    tank_min_label: 'Tank minimal',
    max_min_label: 'Nasos max vaqti (min)',
    save: 'Saqlash',
    saved: '✅ Saqlandi',
    save_error: 'Saqlash xato:',
    settings_saved_toast: 'Sozlamalar saqlandi',
    refresh_title: 'Yangilash',
    points: 'nuqta',
    waiting_data: "Sensor ma'lumotlari kutilmoqda…",
    now: 'hozir',
    never: 'hech qachon',
    age_seconds_ago: '{n} soniya oldin',
    age_minutes_ago: '{n} daqiqa oldin',
    age_hours_ago: '{n} soat oldin',
    last_updated_label: 'Oxirgi yangilanish:',
    chat_greeting: "Salom! Sug'orish bo'yicha savol bering yoki nasoslarni boshqaring. Masalan: «1-nasosni yoq», «2-nasosni o'chir» yoki «Bugun pomidorni sug'orsam bo'ladimi?»",
    chat_placeholder: 'Savol bering yoki buyruq yozing…',
    chat_empty: "Bo'sh javob",
    chat_no_server: "Server bilan aloqa yo'q",
    pump_action_on: 'yoqildi',
    pump_action_off: "o'chirildi",
    pump_action_auto: 'avto rejimga',
    pump_cmd_pending: 'buyruq yuborildi, qurilmadan javob kutilmoqda',
    pump_cmd_failed: 'Buyruq yuborilmadi:',
    reason_no_data: "ma'lumot yo'q",
    reason_local_rules: 'lokal qoidalar',
    reason_override: "qo'lda",
    reason_safety: 'xavfsizlik',
    reason_ai_decision: 'AI qarori',
  },
  ru: {
    conn_connecting: 'подключение…',
    conn_online: 'подключено',
    conn_offline: 'отключено',
    conn_error: 'ошибка сервера',
    device: 'Устройство:',
    last_seen: 'Последняя связь:',
    ai_label: 'AI:',
    decision_label: 'Решение:',
    unknown: 'неизвестно',
    ai_on: 'включён',
    ai_off: 'выключен',
    src_ai: 'AI',
    src_rules: 'локальные правила',
    sensors_title: 'Датчики',
    pumps_title: 'Насосы',
    history_title: 'История (последние 100 записей)',
    ai_assistant_title: '🤖 AI Помощник',
    settings_title: '⚙️ Настройки (пороги)',
    zone1: 'Зона 1',
    zone2: 'Зона 2',
    pump1_name: 'Насос 1',
    pump2_name: 'Насос 2',
    temperature: 'Температура',
    air_humidity: 'Влажность воздуха',
    btn_auto: 'Авто',
    btn_on: 'Включить',
    btn_off: 'Выключить',
    state_on: 'ВКЛ',
    state_off: 'ВЫКЛ',
    state_turning_on: 'ВКЛЮЧЕНИЕ...',
    state_turning_off: 'ВЫКЛЮЧЕНИЕ...',
    state_waiting: 'ОЖИДАНИЕ...',
    mode_prefix: 'режим:',
    mode_auto: 'авто',
    mode_on: 'вкл',
    mode_off: 'выкл',
    soil_low_label: 'Нижний порог почвы',
    soil_high_label: 'Верхний порог почвы',
    tank_min_label: 'Минимум бака',
    max_min_label: 'Макс. время насоса (мин)',
    save: 'Сохранить',
    saved: '✅ Сохранено',
    save_error: 'Ошибка сохранения:',
    settings_saved_toast: 'Настройки сохранены',
    refresh_title: 'Обновить',
    points: 'точек',
    waiting_data: 'Ожидание данных датчиков…',
    now: 'сейчас',
    never: 'никогда',
    age_seconds_ago: '{n} с назад',
    age_minutes_ago: '{n} мин назад',
    age_hours_ago: '{n} ч назад',
    last_updated_label: 'Последнее обновление:',
    chat_greeting: 'Привет! Задайте вопрос об орошении или управляйте насосами. Например: «включи насос 1», «выключи насос 2» или «можно ли сегодня поливать помидоры?»',
    chat_placeholder: 'Задайте вопрос или введите команду…',
    chat_empty: 'Пустой ответ',
    chat_no_server: 'Нет связи с сервером',
    pump_action_on: 'включён',
    pump_action_off: 'выключен',
    pump_action_auto: 'в авто режим',
    pump_cmd_pending: 'команда отправлена, ожидаем ответ устройства',
    pump_cmd_failed: 'Команда не отправлена:',
    reason_no_data: 'нет данных',
    reason_local_rules: 'локальные правила',
    reason_override: 'ручной',
    reason_safety: 'безопасность',
    reason_ai_decision: 'решение AI',
  },
  en: {
    conn_connecting: 'connecting…',
    conn_online: 'online',
    conn_offline: 'offline',
    conn_error: 'server error',
    device: 'Device:',
    last_seen: 'Last seen:',
    ai_label: 'AI:',
    decision_label: 'Decision:',
    unknown: 'unknown',
    ai_on: 'on',
    ai_off: 'off',
    src_ai: 'AI',
    src_rules: 'local rules',
    sensors_title: 'Sensors',
    pumps_title: 'Pumps',
    history_title: 'History (last 100 records)',
    ai_assistant_title: '🤖 AI Assistant',
    settings_title: '⚙️ Settings (thresholds)',
    zone1: 'Zone 1',
    zone2: 'Zone 2',
    pump1_name: 'Pump 1',
    pump2_name: 'Pump 2',
    temperature: 'Temperature',
    air_humidity: 'Air humidity',
    btn_auto: 'Auto',
    btn_on: 'Turn on',
    btn_off: 'Turn off',
    state_on: 'ON',
    state_off: 'OFF',
    state_turning_on: 'TURNING ON...',
    state_turning_off: 'TURNING OFF...',
    state_waiting: 'WAITING...',
    mode_prefix: 'mode:',
    mode_auto: 'auto',
    mode_on: 'on',
    mode_off: 'off',
    soil_low_label: 'Soil low threshold',
    soil_high_label: 'Soil high threshold',
    tank_min_label: 'Tank minimum',
    max_min_label: 'Max pump time (min)',
    save: 'Save',
    saved: '✅ Saved',
    save_error: 'Save error:',
    settings_saved_toast: 'Settings saved',
    refresh_title: 'Refresh',
    points: 'points',
    waiting_data: 'Waiting for sensor data…',
    now: 'now',
    never: 'never',
    age_seconds_ago: '{n}s ago',
    age_minutes_ago: '{n}m ago',
    age_hours_ago: '{n}h ago',
    last_updated_label: 'Last updated:',
    chat_greeting: 'Hi! Ask any irrigation question or control the pumps. For example: "turn on pump 1", "turn off pump 2", or "should I water the tomatoes today?"',
    chat_placeholder: 'Ask a question or type a command…',
    chat_empty: 'Empty response',
    chat_no_server: 'No server connection',
    pump_action_on: 'turned on',
    pump_action_off: 'turned off',
    pump_action_auto: 'set to auto',
    pump_cmd_pending: 'command sent, waiting for device confirmation',
    pump_cmd_failed: 'Command failed:',
    reason_no_data: 'no data yet',
    reason_local_rules: 'local rules',
    reason_override: 'override',
    reason_safety: 'safety',
    reason_ai_decision: 'AI decision',
  },
};

let lang = (function(){
  try {
    const saved = localStorage.getItem('lang');
    if (saved && T[saved]) return saved;
  } catch(e){}
  return 'uz';
})();

function tr(key){
  return (T[lang] && T[lang][key]) || T.uz[key] || key;
}
function trf(key, vars){
  let s = tr(key);
  if (vars) for (const k in vars) s = s.split('{'+k+'}').join(vars[k]);
  return s;
}

function setLang(newLang){
  if (!T[newLang]) return;
  lang = newLang;
  try { localStorage.setItem('lang', lang); } catch(e){}
  applyLang();
}

function applyLang(){
  document.documentElement.lang = lang;
  document.querySelectorAll('[data-i18n]').forEach(el => {
    el.textContent = tr(el.dataset.i18n);
  });
  document.querySelectorAll('.lang-btn').forEach(b => {
    b.classList.toggle('active', b.dataset.lang === lang);
  });
  const inp = document.getElementById('inp');
  if (inp) inp.placeholder = tr('chat_placeholder');
  const refresh = document.getElementById('refresh-btn');
  if (refresh) refresh.title = tr('refresh_title');
  applyDynamicLabels();
  renderLegend();
  if (lastChartPts) drawChart(lastChartPts);
  updateLastUpdated();
}

// ---------- helpers ----------
const $ = id => document.getElementById(id);
let lastSettings = null;
let lastStatus = null;
let lastChartPts = null;
let lastChartUpdate = 0;

function fmtAge(sec){
  if (sec == null) return tr('never');
  if (sec < 60) return trf('age_seconds_ago', {n: Math.round(sec)});
  if (sec < 3600) return trf('age_minutes_ago', {n: Math.round(sec/60)});
  return trf('age_hours_ago', {n: Math.round(sec/3600)});
}
function fmtAgeShort(sec){
  if (sec == null || sec < 30) return tr('now');
  if (sec < 60) return Math.round(sec)+'s';
  if (sec < 3600) return Math.round(sec/60)+'m';
  return Math.round(sec/3600)+'h';
}
function setBar(id, pct, kind){
  const el = $(id); if (!el) return;
  pct = Math.max(0, Math.min(100, pct||0));
  el.style.width = pct + '%';
  let color = 'var(--ok)';
  if (kind === 'soil'){
    if (pct < (lastSettings?.soil_low ?? 35)) color = 'var(--err)';
    else if (pct > (lastSettings?.soil_high ?? 60)) color = 'var(--warn)';
  } else if (kind === 'tank'){
    if (pct < (lastSettings?.tank_min ?? 20)) color = 'var(--err)';
    else if (pct < 40) color = 'var(--warn)';
  } else if (kind === 'temp'){
    color = (pct<10||pct>35) ? 'var(--err)' : (pct<15||pct>30) ? 'var(--warn)' : 'var(--ok)';
  } else if (kind === 'hum'){
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

// Translate static parts of the pump reason string. Free-form AI text and
// numeric inequalities pass through unchanged.
function translatePumpReason(reason){
  if (!reason) return '—';
  let out = reason;
  out = out.replace(/no data yet/gi, tr('reason_no_data'));
  out = out.replace(/local rules:/gi, tr('reason_local_rules') + ':');
  out = out.replace(/override:pump1=on/gi, tr('reason_override') + ':' + tr('pump1_name') + '=' + tr('mode_on'));
  out = out.replace(/override:pump1=off/gi, tr('reason_override') + ':' + tr('pump1_name') + '=' + tr('mode_off'));
  out = out.replace(/override:pump2=on/gi, tr('reason_override') + ':' + tr('pump2_name') + '=' + tr('mode_on'));
  out = out.replace(/override:pump2=off/gi, tr('reason_override') + ':' + tr('pump2_name') + '=' + tr('mode_off'));
  out = out.replace(/safety:pump1 off/gi, tr('reason_safety') + ':' + tr('pump1_name') + ' ' + tr('mode_off'));
  out = out.replace(/safety:pump2 off/gi, tr('reason_safety') + ':' + tr('pump2_name') + ' ' + tr('mode_off'));
  out = out.replace(/AI decision/gi, tr('reason_ai_decision'));
  return out;
}

// Re-apply translations on dynamic spans (called on language switch + on poll).
function applyDynamicLabels(){
  if (!lastStatus) return;
  const d = lastStatus;
  const online = !!d.online;
  $('conn-text').textContent = online ? tr('conn_online') : tr('conn_offline');
  $('device-state').textContent = online ? tr('conn_online') : tr('conn_offline');
  $('last-seen').textContent = fmtAge(d.age_seconds);
  $('ai-state').textContent = d.ai_enabled ? tr('ai_on') : tr('ai_off');
  const dec = d.decision || {};
  const ovr = d.overrides || {'1':'auto','2':'auto'};
  const srcMap = {ai: tr('src_ai'), rules: tr('src_rules'), init: '—'};
  $('decision-source').textContent = srcMap[dec.source] || dec.source || '—';
  ['1','2'].forEach(num => {
    const on = !!dec['pump'+num];
    const stateEl = $('p'+num+'-state');
    if (!stateEl.classList.contains('pending')){
      stateEl.textContent = on ? tr('state_on') : tr('state_off');
    }
    $('p'+num+'-reason').textContent = translatePumpReason(dec.reason);
    const m = ovr[num] || 'auto';
    const modeMap = {auto: tr('mode_auto'), on: tr('mode_on'), off: tr('mode_off')};
    $('p'+num+'-mode').textContent = tr('mode_prefix') + ' ' + (modeMap[m] || m);
  });
}

function updateLastUpdated(){
  const el = $('last-updated-ts');
  if (!el) return;
  if (!lastChartUpdate){ el.textContent = '—'; return; }
  const sec = (Date.now() / 1000) - lastChartUpdate;
  el.textContent = fmtAge(sec);
}

// ---------- chart series + interactive legend ----------
const SERIES = [
  {key:'soil1', color:'#3ad29f', name:'Soil 1'},
  {key:'soil2', color:'#5ac8fa', name:'Soil 2'},
  {key:'tank1', color:'#fdba74', name:'Tank 1'},
  {key:'tank2', color:'#c084fc', name:'Tank 2'},
];
const hiddenSeries = new Set();

function renderLegend(){
  const el = $('legend');
  if (!el) return;
  const count = lastChartPts ? lastChartPts.length : 0;
  let html = '';
  SERIES.forEach(s => {
    const cls = 'lg-item' + (hiddenSeries.has(s.key) ? ' hidden' : '');
    html += '<span class="' + cls + '" data-skey="' + s.key + '">'
         +  '<i style="background:' + s.color + '"></i>' + s.name
         +  '</span>';
  });
  html += '<span class="muted" id="hist-count">' + count + ' ' + tr('points') + '</span>';
  el.innerHTML = html;
  el.querySelectorAll('.lg-item').forEach(item => {
    item.addEventListener('click', () => {
      const k = item.dataset.skey;
      if (hiddenSeries.has(k)) hiddenSeries.delete(k);
      else hiddenSeries.add(k);
      if (lastChartPts) drawChart(lastChartPts);
      renderLegend();
    });
  });
}

// ---------- status polling ----------
async function poll(){
  try{
    const r = await fetch('/api/status');
    if (!r.ok) throw new Error('http '+r.status);
    const d = await r.json();
    lastStatus = d;
    lastSettings = d.settings || lastSettings;

    // header pill
    const online = !!d.online;
    $('conn').firstElementChild.className = 'dot ' + (online?'ok':'err');
    $('conn-text').textContent = online ? tr('conn_online') : tr('conn_offline');
    $('device-state').textContent = online ? tr('conn_online') : tr('conn_offline');
    $('device-state').className = online ? 'ok' : 'err';
    $('last-seen').textContent = fmtAge(d.age_seconds);
    $('ai-state').textContent = d.ai_enabled ? tr('ai_on') : tr('ai_off');
    $('ai-state').className = d.ai_enabled ? 'ok' : 'warn';

    // sensor cards
    const s = d.data || {};
    const set = (id, v, decimals) => {
      const el = $(id);
      if (v == null || v === undefined){ el.textContent = '--'; return; }
      el.textContent = (decimals != null) ? Number(v).toFixed(decimals) : v;
    };
    set('s1', s.soil1); setBar('b-s1', s.soil1, 'soil');
    set('s2', s.soil2); setBar('b-s2', s.soil2, 'soil');
    set('t1', s.tank1); setBar('b-t1', s.tank1, 'tank');
    set('t2', s.tank2); setBar('b-t2', s.tank2, 'tank');
    set('temp', s.temp, 1); setBar('b-temp', ((s.temp ?? 0) / 50) * 100, 'temp');
    set('hum', s.hum, 0); setBar('b-hum', s.hum, 'hum');

    // sensor-error coloring (override)
    if (s.soil1_err){ $('s1').textContent='ERR'; $('s1').className='err'; } else $('s1').className='';
    if (s.soil2_err){ $('s2').textContent='ERR'; $('s2').className='err'; } else $('s2').className='';
    if (s.tank1_err){ $('t1').textContent='ERR'; $('t1').className='err'; } else $('t1').className='';
    if (s.tank2_err){ $('t2').textContent='ERR'; $('t2').className='err'; } else $('t2').className='';

    // pumps
    const dec = d.decision || {};
    const ovr = d.overrides || {'1':'auto','2':'auto'};
    function paintPump(num){
      const on = !!dec['pump'+num];
      const el = $('p'+num+'-state');
      if (!el.classList.contains('pending')){
        el.textContent = on ? tr('state_on') : tr('state_off');
        el.className = 'pump-state ' + (on?'on':'off');
      }
      $('p'+num+'-reason').textContent = translatePumpReason(dec.reason);
      const m = ovr[String(num)] || 'auto';
      const modeMap = {auto: tr('mode_auto'), on: tr('mode_on'), off: tr('mode_off')};
      $('p'+num+'-mode').textContent = tr('mode_prefix') + ' ' + (modeMap[m] || m);
      document.querySelectorAll('.btn[data-pump="'+num+'"]').forEach(b => {
        b.classList.toggle('active', b.dataset.mode === (ovr[String(num)] || 'auto'));
      });
    }
    paintPump(1); paintPump(2);

    const srcMap = {ai: tr('src_ai'), rules: tr('src_rules'), init: '—'};
    $('decision-source').textContent = srcMap[dec.source] || dec.source || '—';
    $('decision-source').className = dec.source==='ai' ? 'ok' : (dec.source==='rules' ? 'warn' : 'muted');

    // settings (only fill if user is not currently editing)
    if (lastSettings && document.activeElement.tagName !== 'INPUT'){
      $('set-soil-low').value  = lastSettings.soil_low;
      $('set-soil-high').value = lastSettings.soil_high;
      $('set-tank-min').value  = lastSettings.tank_min;
      $('set-max-min').value   = lastSettings.max_pump_minutes;
    }
  } catch(e){
    $('conn').firstElementChild.className = 'dot err';
    $('conn-text').textContent = tr('conn_error');
  }
}

// ---------- history chart (vanilla SVG) ----------
async function refreshChart(){
  try{
    const r = await fetch('/api/history?limit=100');
    const d = await r.json();
    const pts = d.items || [];
    lastChartPts = pts;
    lastChartUpdate = Date.now() / 1000;
    drawChart(pts);
    renderLegend();
    updateLastUpdated();
  } catch(e){ /* ignore */ }
}

const SVG_NS = 'http://www.w3.org/2000/svg';
function svgEl(name, attrs){
  const el = document.createElementNS(SVG_NS, name);
  if (attrs) for (const k in attrs) el.setAttribute(k, attrs[k]);
  return el;
}

// Catmull-Rom-to-Bezier smoothing for visually pleasant lines.
function smoothPath(pts){
  if (pts.length < 2) return '';
  if (pts.length === 2){
    return `M${pts[0][0].toFixed(1)},${pts[0][1].toFixed(1)} L${pts[1][0].toFixed(1)},${pts[1][1].toFixed(1)}`;
  }
  let d = `M${pts[0][0].toFixed(1)},${pts[0][1].toFixed(1)}`;
  for (let i = 0; i < pts.length - 1; i++){
    const p0 = pts[i-1] || pts[i];
    const p1 = pts[i];
    const p2 = pts[i+1];
    const p3 = pts[i+2] || p2;
    const cp1x = p1[0] + (p2[0] - p0[0]) / 6;
    const cp1y = p1[1] + (p2[1] - p0[1]) / 6;
    const cp2x = p2[0] - (p3[0] - p1[0]) / 6;
    const cp2y = p2[1] - (p3[1] - p1[1]) / 6;
    d += ` C${cp1x.toFixed(1)},${cp1y.toFixed(1)} ${cp2x.toFixed(1)},${cp2y.toFixed(1)} ${p2[0].toFixed(1)},${p2[1].toFixed(1)}`;
  }
  return d;
}

function drawChart(pts){
  const svg = $('chart');
  const W = 800, H = 300, padL = 38, padR = 14, padT = 14, padB = 28;
  const innerW = W - padL - padR;
  const innerH = H - padT - padB;
  svg.innerHTML = '';

  // <defs> with one linear gradient per series — subtle (max opacity 0.1).
  const defs = svgEl('defs');
  SERIES.forEach(s => {
    const g = svgEl('linearGradient', {
      id: 'g-'+s.key, x1:'0', y1:'0', x2:'0', y2:'1',
    });
    g.appendChild(svgEl('stop', {offset:'0%',   'stop-color': s.color, 'stop-opacity':'0.1'}));
    g.appendChild(svgEl('stop', {offset:'100%', 'stop-color': s.color, 'stop-opacity':'0'}));
    defs.appendChild(g);
  });
  svg.appendChild(defs);

  // Y-axis grid lines + labels (0/25/50/75/100%) — all dashed except the bottom axis.
  for (let i = 0; i <= 4; i++){
    const y = padT + i * innerH / 4;
    svg.appendChild(svgEl('line', {
      x1: padL, x2: W - padR, y1: y, y2: y,
      stroke:'#1d2a44', 'stroke-width':'1',
      'stroke-dasharray': (i === 4) ? '0' : '3,5',
    }));
    const lbl = svgEl('text', {
      x: padL - 6, y: y + 4,
      fill:'#8aa0b8', 'font-size':'11', 'text-anchor':'end',
    });
    lbl.textContent = (100 - i * 25) + '%';
    svg.appendChild(lbl);
  }

  if (!pts || pts.length < 2){
    const t = svgEl('text', {
      x: W/2, y: H/2,
      fill:'#8aa0b8', 'text-anchor':'middle', 'font-size':'13',
    });
    t.textContent = tr('waiting_data');
    svg.appendChild(t);
    return;
  }

  const n = pts.length;
  const tsFirst = pts[0].ts || 0;
  const tsLast  = pts[n-1].ts || tsFirst + 1;
  const span = Math.max(1, tsLast - tsFirst);
  const nowSec = Date.now() / 1000;

  // X-axis time tick labels — 5 evenly spaced ticks ending at "now".
  const xTicks = 5;
  for (let i = 0; i <= xTicks; i++){
    const frac = i / xTicks;
    const x = padL + frac * innerW;
    const ts = tsFirst + frac * span;
    const ageSec = nowSec - ts;
    const label = (i === xTicks) ? tr('now') : fmtAgeShort(ageSec);
    const lbl = svgEl('text', {
      x: x, y: H - 10,
      fill:'#8aa0b8', 'font-size':'10', 'text-anchor':'middle',
    });
    lbl.textContent = label;
    svg.appendChild(lbl);
    if (i > 0 && i < xTicks){
      svg.appendChild(svgEl('line', {
        x1: x, x2: x, y1: padT, y2: H - padB,
        stroke:'#1d2a44', 'stroke-width':'1', 'stroke-dasharray':'3,5',
      }));
    }
  }

  // Plot each series — fill (area) first so the line sits on top.
  // Skip series that the user has toggled off in the legend.
  SERIES.forEach(s => {
    if (hiddenSeries.has(s.key)) return;
    const points = [];
    pts.forEach((p, i) => {
      const v = p[s.key];
      if (v == null || isNaN(v)) return;
      const x = padL + (i / (n - 1)) * innerW;
      const y = padT + (1 - Math.max(0, Math.min(100, v)) / 100) * innerH;
      points.push([x, y]);
    });
    if (points.length < 2) return;

    const linePath = smoothPath(points);

    // Subtle gradient area fill.
    const baseY = padT + innerH;
    const fillD = linePath
      + ` L${points[points.length-1][0].toFixed(1)},${baseY}`
      + ` L${points[0][0].toFixed(1)},${baseY} Z`;
    svg.appendChild(svgEl('path', {
      d: fillD, fill: 'url(#g-' + s.key + ')', stroke: 'none',
    }));

    // Line.
    svg.appendChild(svgEl('path', {
      d: linePath, stroke: s.color, 'stroke-width':'2',
      fill:'none', 'stroke-linejoin':'round', 'stroke-linecap':'round',
    }));

    // Dots at each data point — only when not too dense, to keep visuals clean.
    if (points.length <= 60){
      points.forEach(([x, y]) => {
        svg.appendChild(svgEl('circle', {
          cx: x.toFixed(1), cy: y.toFixed(1), r: '3',
          fill: s.color, stroke: '#0b1220', 'stroke-width': '1',
        }));
      });
    }
  });
}

// ---------- override buttons ----------
// State-based flow: button pressed → disabled + loading → wait for ESP32 confirmation
const pendingPumps = {1: false, 2: false};

document.querySelectorAll('.btn[data-pump]').forEach(b => {
  b.addEventListener('click', async () => {
    const pump = parseInt(b.dataset.pump, 10);
    const mode = b.dataset.mode;
    if (pendingPumps[pump]) return; // already waiting

    // Disable all buttons for this pump
    pendingPumps[pump] = true;
    const pumpBtns = document.querySelectorAll(`.btn[data-pump="${pump}"]`);
    pumpBtns.forEach(btn => { btn.disabled = true; btn.style.opacity = '0.5'; });

    // Show loading state (translated)
    const stateEl = $('p'+pump+'-state');
    const prevText = stateEl.textContent;
    const prevClass = stateEl.className;
    stateEl.textContent = mode === 'on' ? tr('state_turning_on')
                       : mode === 'off' ? tr('state_turning_off')
                       : tr('state_waiting');
    stateEl.className = 'pump-state pending';

    try{
      const r = await fetch('/api/override', {
        method: 'POST',
        headers: {'content-type': 'application/json'},
        body: JSON.stringify({pump, mode}),
      });
      if (!r.ok) throw new Error('http '+r.status);

      // Wait for ESP32 to confirm (poll until state changes, max 45s)
      const wantOn = mode === 'on';
      const wantOff = mode === 'off';
      let confirmed = false;
      for (let i = 0; i < 15; i++){
        await new Promise(ok => setTimeout(ok, 3000));
        const sr = await fetch('/api/status');
        const sd = await sr.json();
        const actual = pump === 1 ? sd.data.pump1 : sd.data.pump2;
        if (mode === 'auto' || (wantOn && actual) || (wantOff && !actual)){
          confirmed = true;
          break;
        }
      }
      const action = mode === 'on' ? tr('pump_action_on')
                  : mode === 'off' ? tr('pump_action_off')
                  : tr('pump_action_auto');
      const pumpName = pump === 1 ? tr('pump1_name') : tr('pump2_name');
      if (confirmed){
        showToast(pumpName + ' → ' + action + ' ✅', 'ok');
      } else {
        showToast(pumpName + ': ' + tr('pump_cmd_pending'), 'warn');
      }
    } catch(e){
      showToast(tr('pump_cmd_failed') + ' ' + e.message, 'err');
      stateEl.textContent = prevText;
      stateEl.className = prevClass;
    } finally{
      pendingPumps[pump] = false;
      pumpBtns.forEach(btn => { btn.disabled = false; btn.style.opacity = '1'; });
      poll();
    }
  });
});

// ---------- settings ----------
async function saveSettings(){
  const body = {
    soil_low:  parseInt($('set-soil-low').value, 10),
    soil_high: parseInt($('set-soil-high').value, 10),
    tank_min:  parseInt($('set-tank-min').value, 10),
    max_pump_minutes: parseInt($('set-max-min').value, 10),
  };
  const btn = $('save-btn');
  try{
    const r = await fetch('/api/settings', {
      method: 'POST',
      headers: {'content-type': 'application/json'},
      body: JSON.stringify(body),
    });
    const d = await r.json();
    if (!r.ok) throw new Error(d.detail || tr('save_error'));
    lastSettings = d.settings;
    showToast(tr('settings_saved_toast'), 'ok');
    // Visual feedback on the button itself.
    btn.classList.add('saved');
    btn.textContent = tr('saved');
    clearTimeout(saveSettings._t);
    saveSettings._t = setTimeout(() => {
      btn.classList.remove('saved');
      btn.textContent = tr('save');
    }, 2000);
  } catch(e){
    showToast(tr('save_error') + ' ' + e.message, 'err');
  }
}

// ---------- chat ----------
function parseMd(text){
  return text
    .replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;')
    .replace(/\*\*(.+?)\*\*/g,'<strong>$1</strong>')
    .replace(/\*(.+?)\*/g,'<em>$1</em>')
    .replace(/`(.+?)`/g,'<code>$1</code>')
    .replace(/\n/g,'<br>');
}
function addMsg(role, text, raw){
  const d = document.createElement('div');
  d.className = 'msg ' + role;
  if (raw) d.textContent = text;
  else d.innerHTML = parseMd(text);
  $('msgs').appendChild(d);
  $('msgs').scrollTop = $('msgs').scrollHeight;
  return d;
}
async function sendMsg(){
  const inp = $('inp'); const m = inp.value.trim(); if (!m) return;
  inp.value = '';
  addMsg('user', m, true);
  const loading = addMsg('ai', '…', true);
  $('send-btn').disabled = true;
  try{
    const r = await fetch('/api/ai/chat', {
      method: 'POST',
      headers: {'content-type': 'application/json'},
      body: JSON.stringify({message: m}),
    });
    const d = await r.json();
    loading.innerHTML = parseMd(d.reply || tr('chat_empty'));
    // If a pump override was applied via chat, refresh status immediately.
    if (d.action === 'override'){
      poll();
    }
  } catch(e){
    loading.textContent = tr('chat_no_server');
  } finally{
    $('send-btn').disabled = false;
    inp.focus();
  }
}

// ---------- refresh button ----------
function forceRefresh(){
  const btn = $('refresh-btn');
  if (btn){
    btn.classList.remove('spin');
    // Re-trigger animation by forcing reflow.
    void btn.offsetWidth;
    btn.classList.add('spin');
    setTimeout(() => btn.classList.remove('spin'), 700);
  }
  poll();
  refreshChart();
}

// ---------- boot ----------
applyLang();
poll();
refreshChart();
setInterval(poll, 3000);
setInterval(refreshChart, 15000);
setInterval(updateLastUpdated, 5000);
</script>
</body>
</html>
"""


if __name__ == "__main__":
    import uvicorn

    uvicorn.run(app, host="0.0.0.0", port=8000)
