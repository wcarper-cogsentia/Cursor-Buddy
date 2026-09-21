"""Buddy desktop service: localhost ingest + LAN WebSocket snapshot broadcast."""

from __future__ import annotations

import asyncio
import json
import logging
import os
import sys
from contextlib import asynccontextmanager
from typing import Any

from pathlib import Path

from fastapi import FastAPI, Request, WebSocket, WebSocketDisconnect
from fastapi.responses import FileResponse, JSONResponse

from buddy.cursor_signals import watch_question_signals, watch_stale_working
from buddy.paths import buddy_log_file, cursor_log_root
from buddy.project import project_from_hook_env_and_payload
from buddy.state import BuddyState

STATIC_DIR = Path(__file__).resolve().parent / "static"

logger = logging.getLogger("cursor-buddy")

INGEST_HOST = os.environ.get("BUDDY_INGEST_HOST", "127.0.0.1")
PUBLIC_HOST = os.environ.get("BUDDY_HOST", "0.0.0.0")
PORT = int(os.environ.get("BUDDY_PORT", "8787"))
STALE_WORKING_SEC = float(os.environ.get("BUDDY_STALE_WORKING_SEC", "30"))

state = BuddyState()
_clients: set[WebSocket] = set()
_loop: asyncio.AbstractEventLoop | None = None


def _is_localhost(request: Request) -> bool:
    client = request.client.host if request.client else ""
    return client in ("127.0.0.1", "::1", "localhost")


async def _broadcast_snapshot() -> None:
    payload = json.dumps(state.snapshot())
    dead: list[WebSocket] = []
    for ws in list(_clients):
        try:
            await ws.send_text(payload)
        except Exception:
            dead.append(ws)
    for ws in dead:
        _clients.discard(ws)


def _schedule_broadcast() -> None:
    loop = _loop
    if loop is None or not loop.is_running():
        return
    asyncio.run_coroutine_threadsafe(_broadcast_snapshot(), loop)


state.on_change(_schedule_broadcast)


def _apply_payload(payload: dict[str, Any]) -> None:
    state.apply_hook(payload, _project_for(payload))


@asynccontextmanager
async def lifespan(app: FastAPI):
    global _loop
    _loop = asyncio.get_running_loop()
    logger.info("Buddy listening ingest+ws on %s:%s (bind %s)", PUBLIC_HOST, PORT, PUBLIC_HOST)
    logger.info("Local receiver: http://127.0.0.1:%s/", PORT)
    logger.info("Platform %s; hook log %s", sys.platform, buddy_log_file())
    logger.info("Stale working → attention after %.0fs", STALE_WORKING_SEC)
    watch_task = asyncio.create_task(watch_question_signals(_apply_payload))
    stale_task = asyncio.create_task(
        watch_stale_working(state.promote_stale_working, STALE_WORKING_SEC)
    )
    try:
        yield
    finally:
        watch_task.cancel()
        stale_task.cancel()
        _clients.clear()


app = FastAPI(title="Cursor Buddy", version="0.1.0", lifespan=lifespan)


@app.get("/")
async def receiver() -> FileResponse:
    return FileResponse(STATIC_DIR / "index.html", headers={"Cache-Control": "no-store"})


@app.get("/health")
async def health() -> dict[str, Any]:
    return {
        "ok": True,
        "platform": sys.platform,
        "sessions": len(state.snapshot()["sessions"]),
        "muted": state.muted,
        "cursor_log_root": str(cursor_log_root()),
        "hook_log": str(buddy_log_file()),
    }


@app.get("/snapshot")
async def get_snapshot() -> dict[str, Any]:
    return state.snapshot()


@app.post("/clear")
async def clear_sessions(request: Request) -> JSONResponse:
    if not _is_localhost(request):
        return JSONResponse({"ok": False, "error": "localhost only"}, status_code=403)
    state.clear()
    return JSONResponse({"ok": True, "sessions": 0, "muted": False})


def _project_for(payload: dict[str, Any]) -> str:
    project = project_from_hook_env_and_payload(payload)
    if isinstance(payload.get("_buddy_project"), str) and payload["_buddy_project"]:
        project = payload["_buddy_project"]
    return project


def _session_id(payload: dict[str, Any]) -> str:
    return str(payload.get("conversation_id") or payload.get("session_id") or "")


@app.post("/ingest")
async def ingest(request: Request) -> JSONResponse:
    if not _is_localhost(request):
        return JSONResponse({"ok": False, "error": "localhost only"}, status_code=403)
    try:
        payload = await request.json()
    except Exception:
        return JSONResponse({"ok": False, "error": "invalid json"}, status_code=400)
    if not isinstance(payload, dict):
        return JSONResponse({"ok": False, "error": "object required"}, status_code=400)

    try:
        state.apply_hook(payload, _project_for(payload))
    except Exception:
        logger.exception("ingest apply_hook failed")
    return JSONResponse({"ok": True})


@app.post("/gate")
async def gate(request: Request) -> JSONResponse:
    """Hold a Cursor hook until Buddy Run/Cancel, then return allow/deny."""
    if not _is_localhost(request):
        return JSONResponse({"permission": "allow", "error": "localhost only"}, status_code=403)
    try:
        payload = await request.json()
    except Exception:
        return JSONResponse({"permission": "allow"})
    if not isinstance(payload, dict):
        return JSONResponse({"permission": "allow"})

    sid = _session_id(payload)
    try:
        state.apply_hook(payload, _project_for(payload))
    except Exception:
        logger.exception("gate apply_hook failed")
        return JSONResponse({"permission": "allow"})
    if not sid:
        return JSONResponse({"permission": "allow"})
    decision = await asyncio.to_thread(state.wait_decision, sid, 90.0)
    if decision == "deny":
        return JSONResponse({"permission": "deny", "user_message": "Denied from Cursor Buddy"})
    return JSONResponse({"permission": "allow"})


@app.websocket("/ws")
async def websocket_endpoint(ws: WebSocket) -> None:
    await ws.accept()
    _clients.add(ws)
    try:
        await ws.send_text(json.dumps(state.snapshot()))
        while True:
            raw = await ws.receive_text()
            try:
                msg = json.loads(raw)
            except json.JSONDecodeError:
                continue
            if not isinstance(msg, dict) or msg.get("type") != "ack":
                continue
            action = str(msg.get("action") or "")
            session_id = str(msg.get("session_id") or "")
            if action == "mute":
                state.set_muted(True)
            elif action == "unmute":
                state.set_muted(False)
            elif action == "clear":
                state.clear()
            elif action == "dismiss" and session_id:
                state.dismiss(session_id)
            elif action == "focus" and session_id:
                state.focus(session_id)
            elif action in ("run", "cancel", "allow", "deny") and session_id:
                state.decide(session_id, action)
    except WebSocketDisconnect:
        pass
    finally:
        _clients.discard(ws)


def main() -> None:
    import uvicorn

    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    # Single bind: ingest is still localhost-restricted in the handler.
    uvicorn.run(
        "buddy.server:app",
        host=PUBLIC_HOST,
        port=PORT,
        log_level="info",
        reload=False,
    )


if __name__ == "__main__":
    main()
