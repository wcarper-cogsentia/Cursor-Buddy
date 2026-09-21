from datetime import datetime, timedelta, timezone

from buddy.state import BuddyState, request_summary


def test_working_to_complete():
    s = BuddyState()
    s.apply_hook(
        {"hook_event_name": "sessionStart", "session_id": "abc", "conversation_id": "abc"},
        "SCOT",
    )
    s.apply_hook(
        {"hook_event_name": "beforeSubmitPrompt", "conversation_id": "abc"},
        "SCOT",
    )
    assert s.sessions["abc"].state == "working"
    s.apply_hook(
        {"hook_event_name": "stop", "conversation_id": "abc", "status": "completed"},
        "SCOT",
    )
    assert s.sessions["abc"].state == "complete"
    snap = s.snapshot()
    assert snap["sessions"][0]["project"] == "SCOT"


def test_stale_working_becomes_attention():
    s = BuddyState()
    s.apply_hook(
        {"hook_event_name": "preToolUse", "conversation_id": "abc", "tool_name": "Read"},
        "SCOT",
    )
    assert s.sessions["abc"].state == "working"
    s.sessions["abc"].updated_at = datetime.now(timezone.utc) - timedelta(seconds=45)
    assert s.promote_stale_working(30) == ["abc"]
    assert s.sessions["abc"].state == "attention"
    assert s.sessions["abc"].message == "Might need you"
    assert s.promote_stale_working(30) == []


def test_fresh_working_is_not_promoted():
    s = BuddyState()
    s.apply_hook(
        {"hook_event_name": "preToolUse", "conversation_id": "abc", "tool_name": "Read"},
        "SCOT",
    )
    assert s.promote_stale_working(30) == []
    assert s.sessions["abc"].state == "working"


def test_ask_question_is_attention():
    s = BuddyState()
    s.apply_hook(
        {
            "hook_event_name": "preToolUse",
            "conversation_id": "scot",
            "tool_name": "AskQuestion",
            "tool_input": {
                "title": "Question",
                "questions": [{"id": "q1", "prompt": "What should learners see?"}],
            },
        },
        "SCOT",
    )
    assert s.sessions["scot"].state == "attention"
    assert "learners" in s.sessions["scot"].message


def test_permission_denied_attention():
    s = BuddyState()
    s.apply_hook(
        {
            "hook_event_name": "postToolUseFailure",
            "conversation_id": "x",
            "failure_type": "permission_denied",
        },
        "Aiden",
    )
    assert s.sessions["x"].state == "attention"


def test_dismiss_and_mute():
    s = BuddyState()
    s.set_state("a", project="P", state="complete", message="done")
    s.set_muted(True)
    assert s.snapshot()["muted"] is True
    s.dismiss("a")
    assert s.snapshot()["sessions"] == []


def test_clear_wipes_sessions_and_unmutes():
    s = BuddyState()
    s.set_state("a", project="SCOT", state="working", message="run")
    s.set_state("b", project="Aiden", state="attention", message="wait")
    s.set_muted(True)
    s.clear()
    assert s.snapshot()["sessions"] == []
    assert s.snapshot()["muted"] is False
    assert s.snapshot()["focused_session_id"] == ""


def test_focus_is_shared_and_attention_steals():
    s = BuddyState()
    s.set_state("scot", project="SCOT", state="working", message="run")
    s.set_state("aiden", project="Aiden", state="complete", message="done")
    assert s.snapshot()["focused_session_id"] == "aiden"
    s.focus("scot")
    assert s.snapshot()["focused_session_id"] == "scot"
    s.set_state("scot", project="SCOT", state="attention", message="Waiting for approval")
    assert s.snapshot()["focused_session_id"] == "scot"
    s.set_state("aiden", project="Aiden", state="error", message="boom")
    assert s.snapshot()["focused_session_id"] == "scot"
    s.dismiss("scot")
    assert s.snapshot()["focused_session_id"] == "aiden"


def test_alert_focus_prefers_more_urgent():
    s = BuddyState()
    s.set_state("b", project="B", state="complete", message="done")
    s.set_state("c", project="C", state="error", message="boom")
    assert s.snapshot()["focused_session_id"] == "c"
    s.set_state("d", project="D", state="attention", message="wait")
    assert s.snapshot()["focused_session_id"] == "d"
    s.set_state("e", project="E", state="complete", message="later")
    assert s.snapshot()["focused_session_id"] == "d"
    s.set_state("f", project="F", state="attention", message="newer")
    assert s.snapshot()["focused_session_id"] == "f"
    s.set_state("g", project="G", state="working", message="run")
    assert s.snapshot()["focused_session_id"] == "f"


def test_mcp_navigate_is_working_not_attention():
    s = BuddyState()
    s.apply_hook(
        {
            "hook_event_name": "beforeMCPExecution",
            "conversation_id": "aiden",
            "generation_id": "gen-1",
            "tool_name": "browser_navigate",
            "tool_input": {"url": "https://sentry.io/"},
        },
        "Aiden",
    )
    assert s.sessions["aiden"].state == "working"
    assert s.sessions["aiden"].can_act is False


def test_approval_hooks_are_attention():
    s = BuddyState()
    s.apply_hook(
        {
            "hook_event_name": "beforeMCPExecution",
            "conversation_id": "aiden",
            "tool_name": "MCP:browser_navigate",
            "tool_input": {"url": "https://sentry.io/"},
            "_buddy_gate": True,
        },
        "Aiden",
    )
    assert s.sessions["aiden"].state == "attention"
    assert s.sessions["aiden"].can_act is True
    assert "sentry.io" in s.sessions["aiden"].message
    assert s.snapshot()["sessions"][0]["can_act"] is True
    assert s.decide("aiden", "run") is True
    assert s.sessions["aiden"].state == "working"
    assert s.sessions["aiden"].can_act is False


def test_stop_is_not_overwritten_by_late_hooks():
    s = BuddyState()
    s.apply_hook({"hook_event_name": "beforeSubmitPrompt", "conversation_id": "abc", "generation_id": "g1"}, "SCOT")
    s.apply_hook({"hook_event_name": "stop", "conversation_id": "abc", "generation_id": "g1", "status": "completed"}, "SCOT")
    assert s.sessions["abc"].state == "complete"
    s.apply_hook({"hook_event_name": "afterAgentThought", "conversation_id": "abc", "generation_id": "g1"}, "SCOT")
    s.apply_hook({"hook_event_name": "preToolUse", "conversation_id": "abc", "generation_id": "g1-3-late", "tool_name": "Read"}, "SCOT")
    assert s.sessions["abc"].state == "complete"
    s.apply_hook({"hook_event_name": "beforeSubmitPrompt", "conversation_id": "abc", "generation_id": "g2"}, "SCOT")
    assert s.sessions["abc"].state == "working"


def test_generation_id_alone_does_not_create_a_session():
    s = BuddyState()
    s.apply_hook(
        {"hook_event_name": "preToolUse", "generation_id": "gen-only", "tool_name": "Read"},
        "Cursor",
    )
    assert s.sessions == {}


def test_synthetic_cursor_ids_are_ignored():
    s = BuddyState()
    s.apply_hook(
        {"hook_event_name": "preToolUse", "conversation_id": "latency-test", "tool_name": "Read"},
        "Cursor",
    )
    assert s.sessions == {}


def test_request_summary_and_denied_message():
    assert "sentry.io" in request_summary(
        {"tool_name": "MCP:browser_navigate", "tool_input": {"url": "https://sentry.io/"}}
    )
    s = BuddyState()
    s.apply_hook(
        {
            "hook_event_name": "preToolUse",
            "conversation_id": "x",
            "tool_name": "MCP:browser_navigate",
            "tool_input": {"url": "https://sentry.io/"},
        },
        "Aiden",
    )
    s.apply_hook(
        {"hook_event_name": "postToolUseFailure", "conversation_id": "x", "failure_type": "permission_denied"},
        "Aiden",
    )
    assert "sentry.io" in s.sessions["x"].message
    assert s.sessions["x"].can_act is False
