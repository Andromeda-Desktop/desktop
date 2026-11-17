#!/usr/bin/env bash
# Toggling a tiled column's maximize sends the new state and size in one configure. A state-only configure first
# makes the client lay out and draw a frame at the old size, then again at the new one.
set -euo pipefail
source "$ANDROMEDA_HARNESS_LIB"

readonly LOG="$ANDROMEDA_RUNTIME_DIR/maximize-one-configure.log"

LOG_CONFIGURES=1 FOLLOW_CONFIGURES=1 "$ANDROMEDA_UNMAP_CLIENT" maximize-one-configure 640 480 > "$LOG" 2>&1 &
await_events "$LOG" mapped 1
"$ANDROMEDA" settle > /dev/null

for step in maximize restore; do
  before=$(events "$LOG" configured-size=)
  "$ANDROMEDA" msg window-toggle-maximize > /dev/null
  await_events "$LOG" configured-size= $((before + 1))
  "$ANDROMEDA" settle > /dev/null
  sent=$(($(events "$LOG" configured-size=) - before))
  ((sent == 1)) || { echo "$step sent $sent configures: $(grep configured-size= "$LOG" | tail -n "$sent" | tr '\n' ' ')"; exit 1; }
done
