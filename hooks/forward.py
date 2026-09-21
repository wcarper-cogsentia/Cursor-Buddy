#!/usr/bin/env python3
"""Cursor Buddy hook forwarder.

Reads Cursor hook JSON from stdin, appends to the capture log, and POSTs to
the local Buddy service. Observational hooks fail open and return immediately.
Gate hooks (beforeMCP / beforeShell) wait for Run/Cancel on Buddy.

Log location follows the host OS (see buddy.paths). Override with BUDDY_LOG_DIR.
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import urllib.request
from datetime import datetime, timezone
from pathlib import Path

_SERVICE_DIR = Path(__file__).resolve().parent.parent / "service"
if str(_SERVICE_DIR) not in sys.path:
    sys.path.insert(0, str(_SERVICE_DIR))

from buddy.paths import buddy_log_dir, buddy_log_file  # noqa: E402
from buddy.project import project_label_from_path as project_label  # noqa: E402

INGEST_URL = os.environ.get("BUDDY_INGEST_URL", "http://127.0.0.1:8787/ingest")
GATE_URL = os.environ.get("BUDDY_GATE_URL", "http://127.0.0.1:8787/gate")
CAPTURE_ONLY = os.environ.get("BUDDY_CAPTURE_ONLY", "").lower() in ("1", "true", "yes")
# Only the simulator sets _buddy_gate. Real Cursor MCP/shell hooks stay observational
# so routine browser_navigate does not freeze the agent as ATTENTION.
GATE_EVENTS: set[str] = set()


def _http_post(body: dict, url: str, timeout: float) -> bytes:
    req = urllib.request.Request(
        url,
        data=json.dumps(body).encode("utf-8"),
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return resp.read()


def _post_file_and_exit() -> bool:
    """Child entry: `forward.py --post <url> <timeout> <file>`. Parent already returned {}."""
    if len(sys.argv) < 5 or sys.argv[1] != "--post":
        return False
    url, timeout_s, path = sys.argv[2], sys.argv[3], sys.argv[4]
    try:
        body = json.loads(Path(path).read_text(encoding="utf-8"))
        if isinstance(body, dict):
            _http_post(body, url, float(timeout_s))
    except Exception:
        pass
    try:
        os.remove(path)
    except OSError:
        pass
    return True


def _background_post(body: dict, url: str, timeout: float) -> None:
    """Schedule the POST and return in the parent. The child never returns."""
    if os.name != "nt":
        try:
            pid = os.fork()
        except OSError:
            return
        if pid > 0:
            return
        try:
            os.setsid()
        except OSError:
            pass
        try:
            _http_post(body, url, timeout)
        except Exception:
            pass
        os._exit(0)

    fd, name = tempfile.mkstemp(prefix="buddy-hook-", suffix=".json")
    try:
        os.write(fd, json.dumps(body).encode("utf-8"))
    finally:
        os.close(fd)
    flags = (
        subprocess.DETACHED_PROCESS
        | subprocess.CREATE_NEW_PROCESS_GROUP
        | subprocess.CREATE_NO_WINDOW
    )
    try:
        subprocess.Popen(
            [sys.executable, str(Path(__file__).resolve()), "--post", url, str(timeout), name],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            creationflags=flags,
            close_fds=True,
        )
    except Exception:
        try:
            os.remove(name)
        except OSError:
            pass


def _append_log(record: dict) -> None:
    try:
        log_dir = buddy_log_dir()
        log_dir.mkdir(parents=True, exist_ok=True)
        with buddy_log_file().open("a", encoding="utf-8") as f:
            f.write(json.dumps(record, ensure_ascii=False) + "\n")
    except Exception:
        pass


def main() -> None:
    if _post_file_and_exit():
        return

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
    _append_log(record)

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
        if gate:
            try:
                raw_resp = _http_post(body, url, timeout)
                data = json.loads(raw_resp.decode("utf-8") or "{}")
                if isinstance(data, dict) and data.get("permission") == "deny":
                    permission = "deny"
            except Exception:
                permission = "allow"
        else:
            # Observational hooks must not block the agent on HTTP.
            _background_post(body, url, timeout)

    if event in GATE_EVENTS:
        sys.stdout.write(json.dumps({"permission": permission}) + "\n")
    else:
        sys.stdout.write("{}\n")
    sys.stdout.flush()
    sys.exit(0)


if __name__ == "__main__":
    main()
