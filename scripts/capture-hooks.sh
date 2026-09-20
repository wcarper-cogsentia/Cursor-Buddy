#!/usr/bin/env bash
# Phase 0: capture-only mode (do not require Buddy service).
set -euo pipefail
export BUDDY_CAPTURE_ONLY=1
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
exec python3 "$ROOT/hooks/forward.py"
