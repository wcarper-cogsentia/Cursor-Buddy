import sys

from buddy.paths import (
    buddy_log_dir,
    buddy_log_file,
    cursor_log_root,
    cursor_user_dir,
    format_hook_command,
)


def _clear_overrides(monkeypatch):
    for name in (
        "BUDDY_LOG_DIR",
        "BUDDY_CURSOR_LOG_ROOT",
        "BUDDY_CURSOR_USER_DIR",
        "APPDATA",
        "LOCALAPPDATA",
        "XDG_CONFIG_HOME",
        "XDG_STATE_HOME",
    ):
        monkeypatch.delenv(name, raising=False)


def test_darwin_defaults(monkeypatch, tmp_path):
    _clear_overrides(monkeypatch)
    monkeypatch.setattr(sys, "platform", "darwin")
    monkeypatch.setattr("buddy.paths._home", lambda: tmp_path)
    assert cursor_user_dir() == tmp_path / "Library" / "Application Support" / "Cursor"
    assert cursor_log_root() == cursor_user_dir() / "logs"
    assert buddy_log_dir() == tmp_path / "Library" / "Logs" / "cursor-buddy"
    assert buddy_log_file() == buddy_log_dir() / "hooks.jsonl"


def test_windows_defaults(monkeypatch, tmp_path):
    _clear_overrides(monkeypatch)
    monkeypatch.setattr(sys, "platform", "win32")
    monkeypatch.setenv("APPDATA", str(tmp_path / "Roaming"))
    monkeypatch.setenv("LOCALAPPDATA", str(tmp_path / "Local"))
    assert cursor_log_root() == tmp_path / "Roaming" / "Cursor" / "logs"
    assert buddy_log_dir() == tmp_path / "Local" / "cursor-buddy"


def test_linux_defaults_and_xdg(monkeypatch, tmp_path):
    _clear_overrides(monkeypatch)
    monkeypatch.setattr(sys, "platform", "linux")
    monkeypatch.setattr("buddy.paths._home", lambda: tmp_path)
    assert cursor_log_root() == tmp_path / ".config" / "Cursor" / "logs"
    assert buddy_log_dir() == tmp_path / ".local" / "state" / "cursor-buddy"

    monkeypatch.setenv("XDG_CONFIG_HOME", str(tmp_path / "cfg"))
    monkeypatch.setenv("XDG_STATE_HOME", str(tmp_path / "state"))
    assert cursor_user_dir() == tmp_path / "cfg" / "Cursor"
    assert buddy_log_dir() == tmp_path / "state" / "cursor-buddy"


def test_env_overrides(monkeypatch, tmp_path):
    monkeypatch.setenv("BUDDY_CURSOR_LOG_ROOT", str(tmp_path / "logs"))
    monkeypatch.setenv("BUDDY_LOG_DIR", str(tmp_path / "buddy"))
    assert cursor_log_root() == tmp_path / "logs"
    assert buddy_log_file() == tmp_path / "buddy" / "hooks.jsonl"


def test_hook_command_quoting(monkeypatch):
    monkeypatch.setattr(sys, "platform", "win32")
    assert format_hook_command(r"C:\Python\python.exe", r"C:\repo\hooks\forward.py") == (
        r'"C:\Python\python.exe" "C:\repo\hooks\forward.py"'
    )
    monkeypatch.setattr(sys, "platform", "darwin")
    assert format_hook_command("/usr/bin/python3", "/repo/hooks/forward.py") == (
        "/usr/bin/python3 /repo/hooks/forward.py"
    )
