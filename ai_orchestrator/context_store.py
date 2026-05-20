"""Agent Context Store — shared memory between planner, builder, reviewer."""

from __future__ import annotations

import json
import time
from pathlib import Path
from typing import Any, Dict, List, Optional


class ContextStore:
    """Persistent shared context between agents. Saved as JSON."""

    def __init__(self, run_dir: Path):
        self.path = run_dir / "context_store.json"
        self._data: Dict[str, Any] = {
            "decisions": [],
            "key_files": [],
            "architecture": "",
            "blockers": [],
            "completed_tasks": [],
            "agent_notes": {},
        }
        self._load()

    def _load(self) -> None:
        if self.path.exists():
            try:
                self._data = json.loads(self.path.read_text())
            except (json.JSONDecodeError, OSError):
                pass

    def _save(self) -> None:
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self.path.write_text(json.dumps(self._data, indent=2, default=str))

    def add_decision(self, agent: str, decision: str) -> None:
        self._data["decisions"].append({
            "agent": agent, "decision": decision,
            "timestamp": time.time(),
        })
        # Keep last 50
        self._data["decisions"] = self._data["decisions"][-50:]
        self._save()

    def add_key_file(self, path: str, purpose: str) -> None:
        existing = {f["path"] for f in self._data["key_files"]}
        if path not in existing:
            self._data["key_files"].append({"path": path, "purpose": purpose})
            self._save()

    def set_architecture(self, arch: str) -> None:
        self._data["architecture"] = arch
        self._save()

    def add_blocker(self, blocker: str) -> None:
        self._data["blockers"].append({"issue": blocker, "timestamp": time.time()})
        self._save()

    def resolve_blocker(self, index: int) -> None:
        if 0 <= index < len(self._data["blockers"]):
            self._data["blockers"][index]["resolved"] = True
            self._save()

    def mark_task_done(self, task: str) -> None:
        self._data["completed_tasks"].append({"task": task, "timestamp": time.time()})
        self._save()

    def set_agent_note(self, agent: str, key: str, value: Any) -> None:
        if agent not in self._data["agent_notes"]:
            self._data["agent_notes"][agent] = {}
        self._data["agent_notes"][agent][key] = value
        self._save()

    def get_context_summary(self, max_chars: int = 3000) -> str:
        """Generate a summary string to inject into agent prompts."""
        parts = []
        if self._data["architecture"]:
            parts.append(f"## Architecture\n{self._data['architecture'][:500]}")
        if self._data["completed_tasks"]:
            recent = self._data["completed_tasks"][-10:]
            tasks_str = "\n".join(f"  ✓ {t['task']}" for t in recent)
            parts.append(f"## Completed tasks\n{tasks_str}")
        if self._data["blockers"]:
            active = [b for b in self._data["blockers"] if not b.get("resolved")]
            if active:
                parts.append(f"## Active blockers\n" + "\n".join(f"  ⚠ {b['issue']}" for b in active))
        if self._data["decisions"]:
            recent = self._data["decisions"][-5:]
            parts.append(f"## Recent decisions\n" + "\n".join(f"  • [{d['agent']}] {d['decision']}" for d in recent))
        if self._data["key_files"]:
            parts.append(f"## Key files\n" + "\n".join(f"  • {f['path']} — {f['purpose']}" for f in self._data["key_files"][:10]))

        result = "\n\n".join(parts)
        return result[:max_chars] if len(result) > max_chars else result

    def update_from_builder_report(self, report: Optional[Dict[str, Any]]) -> None:
        """Extract context from builder JSON report."""
        if not report:
            return
        if report.get("files_changed"):
            for f in report["files_changed"][:5]:
                self.add_key_file(f, "recently changed")
        if report.get("summary"):
            self.mark_task_done(report["summary"][:100])
        if report.get("blockers"):
            for b in report["blockers"]:
                self.add_blocker(str(b))

    def update_from_review(self, verdict_json: Optional[Dict[str, Any]]) -> None:
        """Extract context from reviewer JSON verdict."""
        if not verdict_json:
            return
        if verdict_json.get("defects"):
            for d in verdict_json["defects"][:3]:
                if d.get("severity") in ("critical", "high"):
                    self.add_blocker(f"{d.get('file','')}: {d.get('description','')}"[:100])
        if verdict_json.get("next_tasks"):
            self.set_agent_note("reviewer", "next_tasks",
                                [t.get("task", "") for t in verdict_json["next_tasks"][:5]])
