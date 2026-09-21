#!/bin/sh
# Unix fast path for observational hooks. The installer uses hooks/forward.py,
# which is the cross-platform forwarder. This wrapper remains for manual use.
set -eu

INGEST="${BUDDY_INGEST_URL:-http://127.0.0.1:8787/ingest}"

case "$(uname -s)" in
  Darwin)
    LOG_DIR="${BUDDY_LOG_DIR:-${HOME}/Library/Logs/cursor-buddy}"
    ;;
  *)
    LOG_DIR="${BUDDY_LOG_DIR:-${XDG_STATE_HOME:-${HOME}/.local/state}/cursor-buddy}"
    ;;
esac
LOG_FILE="${LOG_DIR}/hooks.jsonl"

payload=$(cat)
tmp=$(mktemp "${TMPDIR:-/tmp}/buddy-hook.XXXXXX")
printf '%s' "$payload" > "$tmp"

nohup /bin/sh -c '
  ingest="$1"
  log="$2"
  tmp="$3"
  mkdir -p "$(dirname "$log")" 2>/dev/null || true
  { cat "$tmp"; printf "\n"; } >> "$log" 2>/dev/null || true
  curl -sS -m 0.25 -X POST -H "Content-Type: application/json" \
    --data-binary @"$tmp" "$ingest" >/dev/null 2>&1 || true
  rm -f "$tmp"
' _ "$INGEST" "$LOG_FILE" "$tmp" >/dev/null 2>&1 &

printf '%s\n' '{}'
