"""Derive a short project label from a workspace path."""

from __future__ import annotations

import os


def project_label_from_path(path: str | None) -> str:
    if not path:
        return "Cursor"
    # Accept both separators so a Windows workspace root still labels correctly
    # when this runs on another OS (and the reverse).
    normalized = os.path.expanduser(path.strip()).replace("\\", "/").rstrip("/")
    name = normalized.split("/")[-1] or "Cursor"
    # Common: "SCOT-platform" → "SCOT"
    if name.lower().endswith("-platform"):
        name = name[: -len("-platform")]
    return name[:24] or "Cursor"


def project_from_hook_env_and_payload(payload: dict) -> str:
    env_dir = os.environ.get("CURSOR_PROJECT_DIR") or os.environ.get("CURSOR_WORKSPACE_ROOT")
    roots = payload.get("workspace_roots") or []
    if isinstance(roots, list) and roots:
        return project_label_from_path(str(roots[0]))
    if env_dir:
        return project_label_from_path(env_dir)
    cwd = payload.get("cwd")
    if cwd:
        return project_label_from_path(str(cwd))
    return "Cursor"
