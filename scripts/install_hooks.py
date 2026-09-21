#!/usr/bin/env python3
"""Install Cursor Buddy user-level hooks into ~/.cursor/hooks.json."""

from __future__ import annotations

import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SERVICE_DIR = ROOT / "service"
if str(SERVICE_DIR) not in sys.path:
    sys.path.insert(0, str(SERVICE_DIR))

from buddy.paths import buddy_log_file, format_hook_command  # noqa: E402

EVENTS = [
    "sessionStart",
    "sessionEnd",
    "beforeSubmitPrompt",
    "preToolUse",
    "postToolUseFailure",
    "stop",
    "afterAgentResponse",
    "subagentStart",
    "subagentStop",
    "beforeMCPExecution",
    "beforeShellExecution",
]


def _is_buddy_hook(entry: object) -> bool:
    if not isinstance(entry, dict):
        return False
    cmd = str(entry.get("command", "")).replace("\\", "/")
    return "buddy-forward" in cmd or cmd.endswith("hooks/forward.py") or "/hooks/forward.py" in cmd


def main() -> None:
    forward_py = ROOT / "hooks" / "forward.py"
    if not forward_py.is_file():
        raise SystemExit(f"Missing forwarder: {forward_py}")

    cursor_dir = Path.home() / ".cursor"
    hooks_path = cursor_dir / "hooks.json"
    command = format_hook_command(sys.executable, str(forward_py))

    data: dict = {"version": 1, "hooks": {}}
    if hooks_path.exists():
        try:
            loaded = json.loads(hooks_path.read_text(encoding="utf-8"))
            if isinstance(loaded, dict):
                data = loaded
        except Exception:
            data = {"version": 1, "hooks": {}}
    data.setdefault("version", 1)
    hooks = data.setdefault("hooks", {})
    if not isinstance(hooks, dict):
        hooks = {}
        data["hooks"] = hooks

    for event in EVENTS:
        entries = hooks.setdefault(event, [])
        if not isinstance(entries, list):
            entries = []
            hooks[event] = entries
        hooks[event] = [e for e in entries if not _is_buddy_hook(e)]
        # Short timeout: observational only. Never let a hung hook freeze the agent.
        hooks[event].append({"command": command, "timeout": 2})

    thoughts = hooks.get("afterAgentThought") or []
    if isinstance(thoughts, list):
        kept = [e for e in thoughts if not _is_buddy_hook(e)]
        if kept:
            hooks["afterAgentThought"] = kept
        else:
            hooks.pop("afterAgentThought", None)

    cursor_dir.mkdir(parents=True, exist_ok=True)
    hooks_path.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
    print(f"Installed Buddy hooks → {hooks_path}")
    print(f"Forwarder → {command}")
    print(f"Log file: {buddy_log_file()}")
    print("Cursor reloads hooks.json automatically.")


if __name__ == "__main__":
    main()
