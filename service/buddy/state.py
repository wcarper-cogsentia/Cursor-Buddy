"""In-memory session state machine for Cursor Buddy."""

from __future__ import annotations

import threading
import re
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Callable, Literal

SessionState = Literal["idle", "working", "attention", "complete", "error"]

_UUID_RE = re.compile(
    r"^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$"
)

Listener = Callable[[], None]


def _clip(text: str, n: int = 80) -> str:
    text = " ".join(str(text).split())
    return text if len(text) <= n else text[: n - 1] + "…"


def request_summary(payload: dict[str, Any]) -> str:
    """Human-readable request for attention / approval screens."""
    tool = str(
        payload.get("tool_name")
        or payload.get("tool")
        or payload.get("mcp_tool_name")
        or ""
    )
    raw_input = payload.get("tool_input") or payload.get("input") or payload.get("arguments") or {}
    if not isinstance(raw_input, dict):
        raw_input = {}
    command = str(payload.get("command") or raw_input.get("command") or "")
    url = str(raw_input.get("url") or payload.get("url") or "")
    path = str(raw_input.get("file_path") or raw_input.get("path") or "")
    label = tool.replace("MCP:", "").replace("mcp_", "")
    if url:
        return _clip(f"{label or 'Open'} {url}")
    if command:
        first = command.strip().splitlines()[0].strip()
        return _clip(f"{label or 'Shell'}: {first}")
    if path:
        return _clip(f"{label or 'File'}: {Path(path).name}")
    extra = raw_input.get("action") or raw_input.get("query") or raw_input.get("title") or ""
    if label and extra:
        return _clip(f"{label}: {extra}")
    if label:
        return _clip(f"Run {label}")
    return ""


@dataclass
class Session:
    session_id: str
    project: str
    state: SessionState = "idle"
    message: str = ""
    source: str = "cursor"
    started_at: datetime = field(default_factory=lambda: datetime.now(timezone.utc))
    updated_at: datetime = field(default_factory=lambda: datetime.now(timezone.utc))
    working_since: datetime | None = None
    last_request: str = ""
    can_act: bool = False
    closed_generation_id: str = ""

    def elapsed_seconds(self) -> int:
        if self.working_since is not None and self.state == "working":
            return max(0, int((datetime.now(timezone.utc) - self.working_since).total_seconds()))
        return max(0, int((datetime.now(timezone.utc) - self.started_at).total_seconds()))

    def to_dict(self) -> dict[str, Any]:
        return {
            "session_id": self.session_id,
            "project": self.project,
            "state": self.state,
            "message": self.message,
            "elapsed_seconds": self.elapsed_seconds(),
            "updated_at": self.updated_at.astimezone().isoformat(timespec="seconds"),
            "source": self.source,
            "can_act": self.can_act,
        }


class BuddyState:
    def __init__(self) -> None:
        self.sessions: dict[str, Session] = {}
        self.muted: bool = False
        self.focused_session_id: str = ""
        self._listeners: list[Listener] = []
        self._waiters: dict[str, threading.Event] = {}
        self._decisions: dict[str, str] = {}

    def on_change(self, listener: Listener) -> None:
        self._listeners.append(listener)

    def _notify(self) -> None:
        for listener in list(self._listeners):
            try:
                listener()
            except Exception:
                pass

    def active_sessions(self) -> list[Session]:
        order = {"attention": 0, "error": 1, "working": 2, "complete": 3, "idle": 4}
        return sorted(
            (s for s in self.sessions.values() if s.state != "idle"),
            key=lambda s: (order.get(s.state, 9), -s.updated_at.timestamp()),
        )

    def _resolved_focus(self, sessions: list[Session] | None = None) -> str:
        items = sessions if sessions is not None else self.active_sessions()
        ids = {s.session_id for s in items}
        if self.focused_session_id in ids:
            return self.focused_session_id
        return items[0].session_id if items else ""

    def snapshot(self) -> dict[str, Any]:
        sessions = self.active_sessions()
        return {
            "version": 1,
            "type": "snapshot",
            "sessions": [s.to_dict() for s in sessions],
            "focused_session_id": self._resolved_focus(sessions),
            "muted": self.muted,
        }

    def _get_or_create(self, session_id: str, project: str) -> Session:
        sess = self.sessions.get(session_id)
        if sess is None:
            sess = Session(session_id=session_id, project=project or "Cursor")
            self.sessions[session_id] = sess
        elif project and sess.project in ("Cursor", ""):
            sess.project = project
        return sess

    def set_state(
        self,
        session_id: str,
        *,
        project: str,
        state: SessionState,
        message: str = "",
    ) -> None:
        if not session_id:
            return
        sess = self._get_or_create(session_id, project)
        sess.state = state
        sess.message = message
        sess.updated_at = datetime.now(timezone.utc)
        if state == "working":
            if sess.working_since is None:
                sess.working_since = sess.updated_at
        else:
            sess.working_since = None
        sess.can_act = False
        if state in ("attention", "error"):
            self.focused_session_id = session_id
        elif not self.focused_session_id:
            self.focused_session_id = session_id
        self._notify()

    def focus(self, session_id: str) -> None:
        if session_id in self.sessions and self.sessions[session_id].state != "idle":
            self.focused_session_id = session_id
            self._notify()

    def begin_gate(self, session_id: str, project: str, request: str) -> None:
        if not session_id:
            return
        request = request or "Waiting for approval"
        sess = self._get_or_create(session_id, project)
        sess.state = "attention"
        sess.message = request
        sess.last_request = request
        sess.can_act = True
        sess.updated_at = datetime.now(timezone.utc)
        sess.working_since = None
        self.focused_session_id = session_id
        self._waiters[session_id] = threading.Event()
        self._decisions.pop(session_id, None)
        self._notify()

    def decide(self, session_id: str, action: str) -> bool:
        sess = self.sessions.get(session_id)
        if sess is None or not sess.can_act:
            return False
        allow = action in ("run", "allow")
        sess.can_act = False
        sess.updated_at = datetime.now(timezone.utc)
        if allow:
            sess.state = "working"
            sess.message = "Agent running"
            if sess.working_since is None:
                sess.working_since = sess.updated_at
        else:
            sess.message = "Cancelled"
        self._decisions[session_id] = "allow" if allow else "deny"
        ev = self._waiters.get(session_id)
        if ev:
            ev.set()
        self._notify()
        return True

    def wait_decision(self, session_id: str, timeout: float) -> str:
        ev = self._waiters.get(session_id)
        if ev is None:
            return self._decisions.get(session_id) or "timeout"
        if ev.wait(timeout):
            return self._decisions.get(session_id) or "timeout"
        sess = self.sessions.get(session_id)
        if sess is not None and sess.can_act:
            sess.can_act = False
            self._notify()
        return "timeout"

    def _finish_waiter(self, session_id: str, decision: str) -> None:
        self._decisions[session_id] = decision
        ev = self._waiters.get(session_id)
        if ev:
            ev.set()

    def dismiss(self, session_id: str) -> None:
        if session_id in self.sessions:
            del self.sessions[session_id]
            if self.focused_session_id == session_id:
                self.focused_session_id = ""
            self._finish_waiter(session_id, "deny")
            self._notify()

    def set_muted(self, muted: bool) -> None:
        if self.muted != muted:
            self.muted = muted
            self._notify()

    def clear(self) -> None:
        ids = list(self.sessions.keys())
        self.sessions.clear()
        self.focused_session_id = ""
        self.muted = False
        for sid in ids:
            self._finish_waiter(sid, "deny")
        self._notify()

    def apply_hook(self, payload: dict[str, Any], project: str) -> None:
        event = (
            payload.get("hook_event_name")
            or payload.get("event")
            or payload.get("hookEventName")
            or ""
        )
        event = str(event)
        session_id = str(
            payload.get("conversation_id")
            or payload.get("session_id")
            or ""
        )
        # Never key a session on generation_id — that is per-turn and never
        # receives stop, so it would stay WORKING forever.
        if not session_id:
            return
        # Drop leftover test/synthetic hooks that have no real Cursor id
        # and no project label (they show up as "Cursor · WORKING" forever).
        if not _UUID_RE.match(session_id) and (not project or project == "Cursor"):
            return

        if event == "sessionStart":
            sid = str(payload.get("session_id") or payload.get("conversation_id") or session_id)
            if not sid:
                return
            self._get_or_create(sid, project)
            # stay idle until work begins
            self._notify()
            return

        if event == "sessionEnd":
            sid = str(payload.get("session_id") or payload.get("conversation_id") or session_id)
            if sid in self.sessions:
                del self.sessions[sid]
                if self.focused_session_id == sid:
                    self.focused_session_id = ""
                self._finish_waiter(sid, "deny")
                self._notify()
            return

        generation_id = str(payload.get("generation_id") or "")

        if event == "beforeMCPExecution" and payload.get("_buddy_gate"):
            self.begin_gate(session_id, project, request_summary(payload) or "Waiting for approval")
            return

        if event in (
            "beforeSubmitPrompt",
            "preToolUse",
            "beforeShellExecution",
            "beforeMCPExecution",
            "afterAgentThought",
            "postToolUse",
        ):
            existing = self.sessions.get(session_id)
            if (
                event != "beforeSubmitPrompt"
                and existing is not None
                and existing.state == "complete"
                and (
                    not generation_id
                    or not existing.closed_generation_id
                    or generation_id == existing.closed_generation_id
                    or generation_id.startswith(existing.closed_generation_id)
                )
            ):
                return
            summary = request_summary(payload)
            if session_id and summary:
                sess = self._get_or_create(session_id, project)
                sess.last_request = summary
            self.set_state(session_id, project=project, state="working", message="Agent running")
            if event == "beforeSubmitPrompt":
                self.sessions[session_id].closed_generation_id = ""
            return

        if event == "postToolUseFailure":
            failure = str(payload.get("failure_type") or payload.get("failureType") or "")
            if failure == "permission_denied":
                detail = request_summary(payload)
                if not detail:
                    existing = self.sessions.get(session_id)
                    detail = existing.last_request if existing else ""
                message = f"Permission: {detail}" if detail else "Permission required"
                self.set_state(
                    session_id,
                    project=project,
                    state="attention",
                    message=message,
                )
            else:
                # Keep working unless it's a hard fail; still surface attention for timeout
                if failure == "timeout":
                    self.set_state(
                        session_id,
                        project=project,
                        state="attention",
                        message="Tool timed out",
                    )
            return

        if event == "stop":
            status = str(payload.get("status") or "completed")
            if status == "completed":
                # v1: completed turns also mean "come look" — attention chime via complete state
                self.set_state(
                    session_id,
                    project=project,
                    state="complete",
                    message="Agent completed",
                )
                if session_id in self.sessions:
                    self.sessions[session_id].closed_generation_id = generation_id
            elif status == "error":
                self.set_state(
                    session_id,
                    project=project,
                    state="error",
                    message="Agent error",
                )
            else:
                # aborted
                self.set_state(
                    session_id,
                    project=project,
                    state="attention",
                    message="Agent aborted",
                )
            return

        if event == "afterAgentResponse":
            # Soft signal: still working until stop, unless we want attention — leave as working
            if session_id in self.sessions and self.sessions[session_id].state == "idle":
                self.set_state(session_id, project=project, state="working", message="Agent responding")
            return
