# Cursor Buddy protocol v1

Device-facing WebSocket JSON. The ESP32 does not interpret Cursor-specific events.

## Service → device

### `snapshot`

Sent on connect and whenever session state or mute changes.

```json
{
  "version": 1,
  "type": "snapshot",
  "sessions": [
    {
      "session_id": "a82f91",
      "project": "SCOT",
      "state": "attention",
      "message": "Agent waiting for user",
      "elapsed_seconds": 384,
      "updated_at": "2026-09-20T15:42:18-06:00",
      "source": "cursor"
    }
  ],
  "muted": false
}
```

### Session states

| State | Meaning |
|-------|---------|
| `idle` | No active work (usually omitted from `sessions`) |
| `working` | Agent actively executing |
| `attention` | Walt should return (permission denied, or completed turn awaiting review in v1) |
| `complete` | Agent finished successfully |
| `error` | Agent stopped with error |

## Device → service

```json
{ "type": "ack", "session_id": "a82f91", "action": "dismiss" }
```

| Action | Effect |
|--------|--------|
| `dismiss` | Remove session from active list (or mark dismissed) |
| `mute` | Global mute on |
| `unmute` | Global mute off |

## Ingest (localhost only)

`POST http://127.0.0.1:8787/ingest` with Cursor hook JSON (plus optional `hook_event_name` if not already in body). Hooks must fail open.
