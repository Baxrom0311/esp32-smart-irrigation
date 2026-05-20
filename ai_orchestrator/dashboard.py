"""Web Dashboard — FastAPI + SSE real-time progress for AI Agent Orchestrator."""

from __future__ import annotations

import asyncio
import json
from pathlib import Path
from typing import AsyncGenerator, Optional

try:
    from fastapi import FastAPI, Request
    from fastapi.responses import HTMLResponse, StreamingResponse
    import uvicorn
except ImportError:
    raise SystemExit("Install: pip install fastapi uvicorn")

app = FastAPI(title="AgentLoop Dashboard")

# State shared with orchestrator via files
_logs_dir: Optional[Path] = None
_event_queue: asyncio.Queue = asyncio.Queue(maxsize=100)


def set_logs_dir(path: Path) -> None:
    global _logs_dir
    _logs_dir = path


def push_event(event_type: str, data: dict) -> None:
    """Called from orchestrator to push events (non-blocking)."""
    try:
        _event_queue.put_nowait({"event": event_type, "data": data})
    except asyncio.QueueFull:
        pass


def _find_latest_run() -> Optional[Path]:
    if not _logs_dir or not _logs_dir.exists():
        return None
    runs = sorted(_logs_dir.iterdir(), reverse=True)
    return runs[0] if runs else None


def _read_json(path: Path) -> dict:
    try:
        return json.loads(path.read_text())
    except (FileNotFoundError, json.JSONDecodeError):
        return {}


@app.get("/", response_class=HTMLResponse)
async def index():
    return """<!DOCTYPE html>
<html><head><title>AgentLoop Dashboard</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
body{font-family:system-ui;background:#0f172a;color:#e2e8f0;padding:20px}
h1{color:#38bdf8;margin-bottom:20px}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(280px,1fr));gap:16px;margin-bottom:24px}
.card{background:#1e293b;border-radius:12px;padding:20px;border:1px solid #334155}
.card h3{color:#94a3b8;font-size:12px;text-transform:uppercase;margin-bottom:8px}
.card .value{font-size:28px;font-weight:700;color:#f1f5f9}
.card .sub{font-size:12px;color:#64748b;margin-top:4px}
#events{background:#1e293b;border-radius:12px;padding:20px;max-height:500px;overflow-y:auto;border:1px solid #334155}
.event{padding:8px 12px;border-bottom:1px solid #334155;font-size:13px;font-family:monospace}
.event .time{color:#64748b;margin-right:8px}
.event.build{border-left:3px solid #22c55e}
.event.review{border-left:3px solid #eab308}
.event.plan{border-left:3px solid #3b82f6}
.event.error{border-left:3px solid #ef4444}
.status{display:inline-block;width:8px;height:8px;border-radius:50%;margin-right:6px}
.status.running{background:#22c55e;animation:pulse 1s infinite}
.status.done{background:#64748b}
@keyframes pulse{0%,100%{opacity:1}50%{opacity:.4}}
</style></head><body>
<h1>🤖 AgentLoop Dashboard</h1>
<div class="grid">
  <div class="card"><h3>Status</h3><div class="value" id="status"><span class="status running"></span>Loading...</div></div>
  <div class="card"><h3>Total Builds</h3><div class="value" id="builds">-</div><div class="sub" id="build-sub"></div></div>
  <div class="card"><h3>Reviews</h3><div class="value" id="reviews">-</div><div class="sub" id="review-sub"></div></div>
  <div class="card"><h3>Cost</h3><div class="value" id="cost">$0.00</div><div class="sub" id="cost-sub"></div></div>
  <div class="card"><h3>Duration</h3><div class="value" id="duration">-</div></div>
  <div class="card"><h3>Files Changed</h3><div class="value" id="files">-</div></div>
</div>
<h2 style="color:#94a3b8;margin-bottom:12px">Live Events</h2>
<div id="events"></div>
<script>
const es = new EventSource('/events');
es.onmessage = e => {
  const d = JSON.parse(e.data);
  if(d.event==='status') updateStatus(d.data);
  addEvent(d);
};
function updateStatus(s){
  document.getElementById('status').innerHTML = s.done
    ? '<span class="status done"></span>Done' 
    : '<span class="status running"></span>Running';
  document.getElementById('builds').textContent = s.total_builds||0;
  document.getElementById('reviews').textContent = s.total_reviews||0;
  document.getElementById('cost').textContent = '$'+(s.total_cost||0).toFixed(2);
  document.getElementById('duration').textContent = Math.round(s.duration||0)+'s';
  document.getElementById('files').textContent = s.files_changed||0;
  if(s.cost_limit) document.getElementById('cost-sub').textContent = 'limit: $'+s.cost_limit;
  if(s.pass_rate!==undefined) document.getElementById('review-sub').textContent = 'pass: '+(s.pass_rate*100).toFixed(0)+'%';
}
function addEvent(d){
  const el = document.getElementById('events');
  const div = document.createElement('div');
  div.className = 'event '+(d.event||'');
  const t = new Date().toLocaleTimeString();
  div.innerHTML = '<span class="time">'+t+'</span> <b>'+d.event+'</b> '+(d.data.message||JSON.stringify(d.data).slice(0,120));
  el.prepend(div);
  if(el.children.length>200) el.lastChild.remove();
}
// Poll status every 3s
setInterval(()=>fetch('/status').then(r=>r.json()).then(updateStatus), 3000);
fetch('/status').then(r=>r.json()).then(updateStatus);
</script></body></html>"""


@app.get("/status")
async def status():
    run = _find_latest_run()
    if not run:
        return {"done": False, "total_builds": 0, "total_reviews": 0, "total_cost": 0}
    state = _read_json(run / "run_state.json")
    metrics = _read_json(run / "metrics.json")
    summary = metrics.get("summary", {})
    return {
        "done": state.get("done", False),
        "total_builds": state.get("total_builds", 0),
        "total_reviews": summary.get("total_reviews", 0),
        "total_cost": state.get("total_cost_usd", 0),
        "cost_limit": _read_json(run / "effective_config.json").get("budget", {}).get("max_cost_usd", 0),
        "duration": summary.get("total_duration_sec", 0),
        "files_changed": summary.get("total_files_changed", 0),
        "pass_rate": summary.get("review_pass_rate", 0),
        "plan_cycle": state.get("plan_cycle", 0),
        "review_cycle": state.get("review_cycle", 0),
    }


@app.get("/metrics")
async def metrics():
    run = _find_latest_run()
    if not run:
        return {}
    return _read_json(run / "metrics.json")


@app.get("/logs")
async def logs():
    run = _find_latest_run()
    if not run:
        return {"runs": []}
    return {"run": str(run), "files": [f.name for f in run.rglob("*") if f.is_file()]}


@app.get("/events")
async def events(request: Request):
    async def stream() -> AsyncGenerator[str, None]:
        while True:
            if await request.is_disconnected():
                break
            try:
                event = await asyncio.wait_for(_event_queue.get(), timeout=5.0)
                yield f"data: {json.dumps(event)}\n\n"
            except asyncio.TimeoutError:
                # Send keepalive
                run = _find_latest_run()
                if run:
                    state = _read_json(run / "run_state.json")
                    yield f"data: {json.dumps({'event': 'status', 'data': {'done': state.get('done', False), 'total_builds': state.get('total_builds', 0), 'total_cost': state.get('total_cost_usd', 0)}})}\n\n"
                else:
                    yield f"data: {json.dumps({'event': 'heartbeat', 'data': {}})}\n\n"

    return StreamingResponse(stream(), media_type="text/event-stream",
                             headers={"Cache-Control": "no-cache", "X-Accel-Buffering": "no"})


def run_dashboard(logs_dir: Path, host: str = "0.0.0.0", port: int = 8420) -> None:
    """Start dashboard server (blocking)."""
    set_logs_dir(logs_dir)
    print(f"[dashboard] http://{host}:{port}")
    uvicorn.run(app, host=host, port=port, log_level="warning")


if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser(description="AgentLoop Web Dashboard")
    parser.add_argument("--logs-dir", default=".agentloop/runs", help="Logs directory")
    parser.add_argument("--port", type=int, default=8420)
    parser.add_argument("--host", default="0.0.0.0")
    args = parser.parse_args()
    run_dashboard(Path(args.logs_dir), args.host, args.port)
