"""Buddy Mac service: localhost ingest + LAN WebSocket snapshot broadcast."""

from __future__ import annotations

import asyncio
import json
import logging
import os
from contextlib import asynccontextmanager
from typing import Any

from fastapi import FastAPI, Request, WebSocket, WebSocketDisconnect
from fastapi.responses import JSONResponse

from buddy.project import project_from_hook_env_and_payload
from buddy.state import BuddyState

logger = logging.getLogger("cursor-buddy")

INGEST_HOST = os.environ.get("BUDDY_INGEST_HOST", "127.0.0.1")
PUBLIC_HOST = os.environ.get("BUDDY_HOST", "0.0.0.0")
PORT = int(os.environ.get("BUDDY_PORT", "8787"))

state = BuddyState()
_clients: set[WebSocket] = set()
_loop: asyncio.AbstractEventLoop | None = None


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


@asynccontextmanager
async def lifespan(app: FastAPI):
    global _loop
    _loop = asyncio.get_running_loop()
    logger.info("Buddy listening ingest+ws on %s:%s (bind %s)", PUBLIC_HOST, PORT, PUBLIC_HOST)
    yield
    _clients.clear()


app = FastAPI(title="Cursor Buddy", version="0.1.0", lifespan=lifespan)


@app.get("/health")
async def health() -> dict[str, Any]:
    return {"ok": True, "sessions": len(state.snapshot()["sessions"]), "muted": state.muted}


@app.get("/snapshot")
async def get_snapshot() -> dict[str, Any]:
    return state.snapshot()


@app.post("/ingest")
async def ingest(request: Request) -> JSONResponse:
    # Localhost-only ingest: reject non-loopback clients
    client = request.client.host if request.client else ""
    if client not in ("127.0.0.1", "::1", "localhost"):
        return JSONResponse({"ok": False, "error": "localhost only"}, status_code=403)
    try:
        payload = await request.json()
    except Exception:
        return JSONResponse({"ok": False, "error": "invalid json"}, status_code=400)
    if not isinstance(payload, dict):
        return JSONResponse({"ok": False, "error": "object required"}, status_code=400)

    project = project_from_hook_env_and_payload(payload)
    # Prefer project injected by forwarder
    if isinstance(payload.get("_buddy_project"), str) and payload["_buddy_project"]:
        project = payload["_buddy_project"]
    try:
        state.apply_hook(payload, project)
    except Exception:
        logger.exception("ingest apply_hook failed")
    return JSONResponse({"ok": True})


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
            elif action == "dismiss" and session_id:
                state.dismiss(session_id)
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
