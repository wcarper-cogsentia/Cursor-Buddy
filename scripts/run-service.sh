#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT/service"
if [[ ! -d .venv ]]; then
  python3 -m venv .venv
  .venv/bin/pip install -q -r requirements.txt
fi
export BUDDY_HOST="${BUDDY_HOST:-0.0.0.0}"
export BUDDY_PORT="${BUDDY_PORT:-8787}"
exec .venv/bin/python -m buddy
