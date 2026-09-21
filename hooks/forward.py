#!/usr/bin/env python3
"""Cursor Buddy hook forwarder.

Reads Cursor hook JSON from stdin, appends to the capture log (Phase 0),
and POSTs to the local Buddy service. Observational hooks fail open.
Gate hooks (beforeMCP / beforeShell) wait for Run/Cancel on Buddy.
"""

from __future__ import annotations

import json
import os
import sys
import urllib.error
import urllib.request
from datetime import datetime, timezone
from pathlib import Path

LOG_DIR = Path.home() / "Library" / "Logs" / "cursor-buddy"
LOG_FILE = LOG_DIR / "hooks.jsonl"
INGEST_URL = os.environ.get("BUDDY_INGEST_URL", "http://127.0.0.1:8787/ingest")
GATE_URL = os.environ.get("BUDDY_GATE_URL", "http://127.0.0.1:8787/gate")
CAPTURE_ONLY = os.environ.get("BUDDY_CAPTURE_ONLY", "").lower() in ("1", "true", "yes")
# Only the simulator sets _buddy_gate. Real Cursor MCP/shell hooks stay observational
# so routine browser_navigate does not freeze the agent as ATTENTION.
GATE_EVENTS: set[str] = set()


def project_label(path: str | None) -> str:
    if not path:
        return "Cursor"
    name = Path(path.rstrip("/")).name or "Cursor"
    if name.lower().endswith("-platform"):
        name = name[: -len("-platform")]
    return name[:24] or "Cursor"


def main() -> None:
    raw = sys.stdin.read()
    payload: dict = {}
    try:
        parsed = json.loads(raw) if raw.strip() else {}
        if isinstance(parsed, dict):
            payload = parsed
    except json.JSONDecodeError:
        payload = {"_raw": raw[:4000], "hook_event_name": "parse_error"}

    env_project = os.environ.get("CURSOR_PROJECT_DIR") or ""
    record = {
        "ts": datetime.now(timezone.utc).astimezone().isoformat(timespec="seconds"),
        "env": {
            "CURSOR_PROJECT_DIR": env_project,
            "CURSOR_TRANSCRIPT_PATH": os.environ.get("CURSOR_TRANSCRIPT_PATH"),
        },
        "payload": payload,
    }

    try:
        LOG_DIR.mkdir(parents=True, exist_ok=True)
        with LOG_FILE.open("a", encoding="utf-8") as f:
            f.write(json.dumps(record, ensure_ascii=False) + "\n")
    except Exception:
        pass

    event = str(payload.get("hook_event_name") or payload.get("event") or "")
    permission = "allow"

    if not CAPTURE_ONLY:
        body = dict(payload)
        body["_buddy_project"] = project_label(env_project) if env_project else body.get("_buddy_project")
        if not body.get("_buddy_project"):
            roots = body.get("workspace_roots") or []
            if isinstance(roots, list) and roots:
                body["_buddy_project"] = project_label(str(roots[0]))
        gate = event in GATE_EVENTS
        url = GATE_URL if gate else INGEST_URL
        timeout = 95.0 if gate else 0.25
        # Observational hooks must not block the agent on HTTP.
        if not gate and os.fork() > 0:
            sys.stdout.write("{}\n")
            sys.exit(0)
        if not gate:
            try:
                os.setsid()
            except Exception:
                pass
        try:
            req = urllib.request.Request(
                url,
                data=json.dumps(body).encode("utf-8"),
                headers={"Content-Type": "application/json"},
                method="POST",
            )
            with urllib.request.urlopen(req, timeout=timeout) as resp:
                raw_resp = resp.read()
            if gate:
                try:
                    data = json.loads(raw_resp.decode("utf-8") or "{}")
                    if isinstance(data, dict) and data.get("permission") == "deny":
                        permission = "deny"
                except json.JSONDecodeError:
                    permission = "allow"
        except Exception:
            permission = "allow"
        if not gate:
            os._exit(0)

    if event in GATE_EVENTS:
        sys.stdout.write(json.dumps({"permission": permission}) + "\n")
    else:
        sys.stdout.write("{}\n")
    sys.exit(0)


if __name__ == "__main__":
    main()
