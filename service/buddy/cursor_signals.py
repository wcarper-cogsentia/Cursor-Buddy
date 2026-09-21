"""Watch Cursor agent-host logs for AskQuestion, which does not fire hooks."""

from __future__ import annotations

import json
import logging
from pathlib import Path
from typing import Callable

from buddy.paths import cursor_log_root

logger = logging.getLogger("cursor-buddy")

QUESTION_ACTION = "asyncAskQuestionCompletionAction"


def _agent_host_logs() -> list[Path]:
    root = cursor_log_root()
    if not root.exists():
        return []
    return sorted(root.glob("*/window*/exthost/anysphere.cursor-agent-host/Cursor Agent Host.log"))


def scan_question_signals(seen: set[tuple[str, str]]) -> list[dict]:
    """Return new AskQuestion signals as hook-shaped payloads."""
    found: list[dict] = []
    for path in _agent_host_logs():
        try:
            text = path.read_text(errors="replace")
        except OSError:
            continue
        for line in text.splitlines()[-80:]:
            brace = line.find("{")
            if brace < 0 or QUESTION_ACTION not in line:
                continue
            try:
                data = json.loads(line[brace:])
            except json.JSONDecodeError:
                continue
            if data.get("actionCase") != QUESTION_ACTION:
                continue
            sid = str(data.get("conversationId") or "")
            gen = str(data.get("generationUUID") or "")
            key = (sid, gen)
            if not sid or key in seen:
                continue
            seen.add(key)
            found.append(
                {
                    "hook_event_name": "preToolUse",
                    "conversation_id": sid,
                    "generation_id": gen,
                    "tool_name": "AskQuestion",
                    "tool_input": {"title": "Waiting for your answer"},
                }
            )
    return found


async def watch_question_signals(apply: Callable[[dict], None]) -> None:
    import asyncio

    root = cursor_log_root()
    if root.exists():
        logger.info("Watching Cursor logs in %s", root)
    else:
        logger.info("Cursor log directory not found (%s); AskQuestion signals wait until it exists", root)
    seen: set[tuple[str, str]] = set()
    # Prime so historical lines do not replay as fresh attention.
    scan_question_signals(seen)
    while True:
        try:
            for payload in scan_question_signals(seen):
                logger.info("AskQuestion signal for %s", payload.get("conversation_id"))
                apply(payload)
        except Exception:
            logger.exception("cursor question signal scan failed")
        await asyncio.sleep(2.0)


async def watch_stale_working(promote: Callable[[float], list[str]], max_age_seconds: float) -> None:
    import asyncio

    while True:
        await asyncio.sleep(2.0)
        try:
            promoted = promote(max_age_seconds)
            for sid in promoted:
                logger.info("Stale working → attention for %s", sid)
        except Exception:
            logger.exception("stale working scan failed")
