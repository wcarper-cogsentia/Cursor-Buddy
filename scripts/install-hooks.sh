#!/usr/bin/env bash
# Install Cursor Buddy user-level hooks (merge into ~/.cursor/hooks.json).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FORWARD="$ROOT/hooks/forward.py"
CURSOR_DIR="${HOME}/.cursor"
HOOKS_JSON="${CURSOR_DIR}/hooks.json"
HOOKS_DIR="${CURSOR_DIR}/hooks"
mkdir -p "$HOOKS_DIR"

ln -sfn "$FORWARD" "$HOOKS_DIR/buddy-forward.py"
chmod +x "$FORWARD"

CMD="python3 ${HOOKS_DIR}/buddy-forward.py"

python3 - << PY
import json
from pathlib import Path

hooks_path = Path("${HOOKS_JSON}")
cmd = r"""$CMD"""

events = [
    "sessionStart", "sessionEnd", "beforeSubmitPrompt", "preToolUse",
    "postToolUseFailure", "stop", "afterAgentResponse", "afterAgentThought",
    "subagentStart", "subagentStop",
]

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
    entries.append({"command": cmd})

hooks_path.parent.mkdir(parents=True, exist_ok=True)
hooks_path.write_text(json.dumps(data, indent=2) + "\n")
print(f"Installed Buddy hooks → {hooks_path}")
print(f"Forwarder → {cmd}")
print("Cursor reloads hooks.json automatically.")
PY

echo "Log file: ~/Library/Logs/cursor-buddy/hooks.jsonl"
