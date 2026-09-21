#!/usr/bin/env bash
# Install Cursor Buddy user-level hooks (macOS and Linux).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
exec python3 "$ROOT/scripts/install_hooks.py"
