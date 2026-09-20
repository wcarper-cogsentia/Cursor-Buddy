# Phase 0 → Phase 4: refining ATTENTION vs COMPLETE

After installing hooks and using Cursor for a few real agent sessions, inspect:

```bash
tail -n 50 ~/Library/Logs/cursor-buddy/hooks.jsonl | python3 -m json.tool
```

## What to look for

| Situation | Expected hooks | Desired Buddy state |
|-----------|----------------|---------------------|
| Send prompt / tools run | `beforeSubmitPrompt`, `preToolUse` | `working` |
| Turn finishes OK | `stop` with `status: "completed"` | `complete` (v1 also chimes like attention) |
| Hard failure | `stop` with `status: "error"` | `error` |
| Permission blocked | `postToolUseFailure` + `failure_type: "permission_denied"` | `attention` |
| User abort | `stop` with `status: "aborted"` | `attention` |

## Known Cursor gap

Claude-style `Notification` / `PermissionRequest` are **not** mapped. If logs show no reliable permission event until deny/timeout, keep v1’s broad “come back” behavior: treat `complete` as a chime-worthy event.

## Where to change classification

Edit `service/buddy/state.py` → `BuddyState.apply_hook`. Redeploy is just restarting `./scripts/run-service.sh`; ESP32 firmware need not change if states remain the same.
