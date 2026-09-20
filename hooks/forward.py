#!/usr/bin/env python3
"""Cursor Buddy hook forwarder.

Reads Cursor hook JSON from stdin, appends to the capture log (Phase 0),
and POSTs to the local Buddy service. Always exits 0 with {} so Cursor
is never blocked (fail-open).
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
CAPTURE_ONLY = os.environ.get("BUDDY_CAPTURE_ONLY", "").lower() in ("1", "true", "yes")


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

    if not CAPTURE_ONLY:
        body = dict(payload)
        body["_buddy_project"] = project_label(env_project) if env_project else body.get("_buddy_project")
        if not body.get("_buddy_project"):
            roots = body.get("workspace_roots") or []
            if isinstance(roots, list) and roots:
                body["_buddy_project"] = project_label(str(roots[0]))
        try:
            req = urllib.request.Request(
                INGEST_URL,
                data=json.dumps(body).encode("utf-8"),
                headers={"Content-Type": "application/json"},
                method="POST",
            )
            with urllib.request.urlopen(req, timeout=0.4) as resp:
                resp.read()
        except Exception:
            pass

    # Fail-open: always print empty object for hooks that expect JSON stdout
    sys.stdout.write("{}\n")
    sys.exit(0)


if __name__ == "__main__":
    main()
