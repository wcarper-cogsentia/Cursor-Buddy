from buddy.state import BuddyState


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
