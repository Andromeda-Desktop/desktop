#!/usr/bin/env bash
# A log-out whose systemctl request never answers stops the compositor after the request deadline (R8B-1).
set -euo pipefail
python3 "$ANDROMEDA_REPO/tests/session_logout.py" "$ANDROMEDA" hang
