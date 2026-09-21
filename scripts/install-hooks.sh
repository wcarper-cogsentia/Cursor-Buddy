#!/usr/bin/env bash
# Install Cursor Buddy user-level hooks (merge into ~/.cursor/hooks.json).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FORWARD_PY="$ROOT/hooks/forward.py"
FORWARD_SH="$ROOT/hooks/forward.sh"
CURSOR_DIR="${HOME}/.cursor"
HOOKS_JSON="${CURSOR_DIR}/hooks.json"
HOOKS_DIR="${CURSOR_DIR}/hooks"
mkdir -p "$HOOKS_DIR"

ln -sfn "$FORWARD_PY" "$HOOKS_DIR/buddy-forward.py"
ln -sfn "$FORWARD_SH" "$HOOKS_DIR/buddy-forward.sh"
chmod +x "$FORWARD_PY" "$FORWARD_SH"

# Shell wrapper returns immediately; Python is only used if someone invokes it directly.
CMD="${HOOKS_DIR}/buddy-forward.sh"

python3 - << PY
import json
from pathlib import Path

hooks_path = Path("${HOOKS_JSON}")
cmd = r"""$CMD"""

events = [
    "sessionStart", "sessionEnd", "beforeSubmitPrompt", "preToolUse",
    "postToolUseFailure", "stop", "afterAgentResponse",
    "subagentStart", "subagentStop",
    "beforeMCPExecution", "beforeShellExecution",
]
# afterAgentThought is too chatty and stalls every reasoning step.

data = {"version": 1, "hooks": {}}
if hooks_path.exists():
    try:
        data = json.loads(hooks_path.read_text())
    except Exception:
        data = {"version": 1, "hooks": {}}
data.setdefault("version", 1)
data.setdefault("hooks", {})

for ev in events:
    entries = data["hooks"].setdefault(ev, [])
    entries[:] = [
        e for e in entries
        if not (isinstance(e, dict) and "buddy-forward" in str(e.get("command", "")))
    ]
    # Short timeout: observational only. Never let a hung hook freeze the agent.
    entries.append({"command": cmd, "timeout": 2})

# Drop the old chatty thought hook if a previous install left it behind.
thoughts = data["hooks"].get("afterAgentThought") or []
data["hooks"]["afterAgentThought"] = [
    e for e in thoughts
    if not (isinstance(e, dict) and "buddy-forward" in str(e.get("command", "")))
]
if not data["hooks"]["afterAgentThought"]:
    data["hooks"].pop("afterAgentThought", None)

hooks_path.parent.mkdir(parents=True, exist_ok=True)
hooks_path.write_text(json.dumps(data, indent=2) + "\n")
print(f"Installed Buddy hooks → {hooks_path}")
print(f"Forwarder → {cmd}")
print("Cursor reloads hooks.json automatically.")
PY

echo "Log file: ~/Library/Logs/cursor-buddy/hooks.jsonl"
