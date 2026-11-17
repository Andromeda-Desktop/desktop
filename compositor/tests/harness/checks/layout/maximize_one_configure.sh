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

# A maximize the client asks for gets the same single configure.
readonly CLIENT_LOG="$ANDROMEDA_RUNTIME_DIR/maximize-one-configure-client.log"
readonly CONTROL_FIFO="$ANDROMEDA_RUNTIME_DIR/maximize-one-configure-control"
mkfifo "$CONTROL_FIFO"
exec {control_fd}<>"$CONTROL_FIFO"
MAXIMIZE_ON_STDIN=1 LOG_CONFIGURES=1 FOLLOW_CONFIGURES=1 "$ANDROMEDA_UNMAP_CLIENT" maximize-one-configure-client 640 480 \
  < "$CONTROL_FIFO" > "$CLIENT_LOG" 2>&1 &
await_events "$CLIENT_LOG" mapped 1
"$ANDROMEDA" settle > /dev/null

for step in m M; do
  before=$(events "$CLIENT_LOG" configured-size=)
  echo "$step" >&"$control_fd"
  await_events "$CLIENT_LOG" configured-size= $((before + 1))
  "$ANDROMEDA" settle > /dev/null
  sent=$(($(events "$CLIENT_LOG" configured-size=) - before))
  ((sent == 1)) || { echo "client request $step sent $sent configures: $(grep configured-size= "$CLIENT_LOG" | tail -n "$sent" | tr '\n' ' ')"; exit 1; }
done
