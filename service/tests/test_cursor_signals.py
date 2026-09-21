import json

from buddy.cursor_signals import scan_question_signals


def test_scan_uses_configured_log_root(monkeypatch, tmp_path):
    monkeypatch.setenv("BUDDY_CURSOR_LOG_ROOT", str(tmp_path))
    log = (
        tmp_path
        / "20260921T120000"
        / "window1"
        / "exthost"
        / "anysphere.cursor-agent-host"
        / "Cursor Agent Host.log"
    )
    log.parent.mkdir(parents=True)
    record = {
        "actionCase": "asyncAskQuestionCompletionAction",
        "conversationId": "conv-1",
        "generationUUID": "gen-1",
    }
    log.write_text("info " + json.dumps(record) + "\n", encoding="utf-8")

    seen: set[tuple[str, str]] = set()
    found = scan_question_signals(seen)
    assert len(found) == 1
    assert found[0]["conversation_id"] == "conv-1"
    assert found[0]["tool_name"] == "AskQuestion"
    assert scan_question_signals(seen) == []
