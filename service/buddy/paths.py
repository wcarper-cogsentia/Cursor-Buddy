"""Host paths for Cursor logs and Buddy's hook capture log.

Defaults follow each OS's Cursor user-data directory. Override with:

  BUDDY_LOG_DIR           directory that receives hooks.jsonl
  BUDDY_CURSOR_LOG_ROOT   Cursor logs directory (parent of the dated folders)
  BUDDY_CURSOR_USER_DIR   Cursor user-data directory; logs default to <dir>/logs
"""

from __future__ import annotations

import os
import shlex
import sys
from pathlib import Path


def _home() -> Path:
    return Path.home()


def _env_path(name: str) -> Path | None:
    raw = os.environ.get(name, "").strip()
    if not raw:
        return None
    return Path(raw).expanduser()


def cursor_user_dir() -> Path:
    override = _env_path("BUDDY_CURSOR_USER_DIR")
    if override is not None:
        return override
    if sys.platform == "darwin":
        return _home() / "Library" / "Application Support" / "Cursor"
    if sys.platform == "win32":
        appdata = os.environ.get("APPDATA", "").strip()
        base = Path(appdata) if appdata else _home() / "AppData" / "Roaming"
        return base / "Cursor"
    config = os.environ.get("XDG_CONFIG_HOME", "").strip()
    base = Path(config).expanduser() if config else _home() / ".config"
    return base / "Cursor"


def cursor_log_root() -> Path:
    override = _env_path("BUDDY_CURSOR_LOG_ROOT")
    if override is not None:
        return override
    return cursor_user_dir() / "logs"


def buddy_log_dir() -> Path:
    override = _env_path("BUDDY_LOG_DIR")
    if override is not None:
        return override
    if sys.platform == "darwin":
        return _home() / "Library" / "Logs" / "cursor-buddy"
    if sys.platform == "win32":
        local = os.environ.get("LOCALAPPDATA", "").strip()
        base = Path(local) if local else _home() / "AppData" / "Local"
        return base / "cursor-buddy"
    state = os.environ.get("XDG_STATE_HOME", "").strip()
    base = Path(state).expanduser() if state else _home() / ".local" / "state"
    return base / "cursor-buddy"


def buddy_log_file() -> Path:
    return buddy_log_dir() / "hooks.jsonl"


def format_hook_command(python: str, script: str) -> str:
    """Shell command Cursor should run for a user hook."""
    if sys.platform == "win32":
        return f'"{python}" "{script}"'
    return shlex.join([python, script])
