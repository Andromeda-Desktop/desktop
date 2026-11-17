#!/usr/bin/env bash
# A synchronized subsurface that commits several buffers before its parent commits gets each superseded buffer back
# at once. Holding them until the parent commits starves a GL client of buffers and deadlocks it (Flutter on resize).
set -euo pipefail
source "$ANDROMEDA_HARNESS_LIB"

readonly LOG="$ANDROMEDA_RUNTIME_DIR/sync-subsurface.log"

"$ANDROMEDA_SYNC_SUBSURFACE_CLIENT" > "$LOG" 2>&1 &
await_lines "$LOG" 'applied ' 1
grep -qx 'queued middle=released' "$LOG" || { cat "$LOG"; fail "superseded queued buffer was held until the parent commit"; }
grep -qx 'applied first=released middle=released' "$LOG" || { cat "$LOG"; fail "applied commit did not release the earlier buffers"; }
