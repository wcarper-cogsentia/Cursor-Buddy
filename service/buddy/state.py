"""In-memory session state machine for Cursor Buddy."""

from __future__ import annotations

from dataclasses import dataclass, field
from datetime import datetime, timezone
from typing import Any, Callable, Literal

SessionState = Literal["idle", "working", "attention", "complete", "error"]

Listener = Callable[[], None]


def _now_iso() -> str:
    return datetime.now(timezone.utc).astimezone().isoformat(timespec="seconds")


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
        }


class BuddyState:
    def __init__(self) -> None:
        self.sessions: dict[str, Session] = {}
        self.muted: bool = False
        self._listeners: list[Listener] = []

    def on_change(self, listener: Listener) -> None:
        self._listeners.append(listener)

    def _notify(self) -> None:
        for listener in list(self._listeners):
            try:
                listener()
            except Exception:
                pass

    def snapshot(self) -> dict[str, Any]:
        # Prefer attention/error/working/complete order for UI
        order = {"attention": 0, "error": 1, "working": 2, "complete": 3, "idle": 4}
        sessions = sorted(
            (s for s in self.sessions.values() if s.state != "idle"),
            key=lambda s: (order.get(s.state, 9), -s.updated_at.timestamp()),
        )
        return {
            "version": 1,
            "type": "snapshot",
            "sessions": [s.to_dict() for s in sessions],
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
        self._notify()

    def dismiss(self, session_id: str) -> None:
        if session_id in self.sessions:
            del self.sessions[session_id]
            self._notify()

    def set_muted(self, muted: bool) -> None:
        if self.muted != muted:
            self.muted = muted
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
            or payload.get("generation_id")
            or ""
        )
        if not session_id and event not in ("sessionStart", "sessionEnd"):
            # Still try generation_id alone above; if empty, skip
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
                self._notify()
            return

        if event in ("beforeSubmitPrompt", "preToolUse", "afterAgentThought", "postToolUse"):
            self.set_state(session_id, project=project, state="working", message="Agent running")
            return

        if event == "postToolUseFailure":
            failure = str(payload.get("failure_type") or payload.get("failureType") or "")
            if failure == "permission_denied":
                self.set_state(
                    session_id,
                    project=project,
                    state="attention",
                    message="Permission required",
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
