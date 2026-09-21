#!/bin/sh
# Observational Buddy hook: print {} and exit immediately.
# Log + HTTP ingest run detached so Cursor agents are not stalled.
set -eu

INGEST="${BUDDY_INGEST_URL:-http://127.0.0.1:8787/ingest}"
LOG_DIR="${HOME}/Library/Logs/cursor-buddy"
LOG_FILE="${LOG_DIR}/hooks.jsonl"

payload=$(cat)
tmp=$(mktemp -t buddy-hook)
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