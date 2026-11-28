#!/usr/bin/env bash
# The screenshot freeze (a cover) stays above status surfaces: a live-activity island that maps after it, at the same
# place in the same layer, stays under the still.
set -euo pipefail
source "$ANDROMEDA_HARNESS_LIB"

cat >> "$ANDROMEDA_CONFIG" <<'CONF'

[animation]
enabled = false
[colors]
backdrop = "#000000FF"
CONF
"$ANDROMEDA" msg config-reload > /dev/null

mkfifo "$ANDROMEDA_RUNTIME_DIR/cover.fifo"
env LAYER_CONTROL=1 LAYER_NAMESPACE=noctalia-screenshot-region "$ANDROMEDA_LAYER_CLIENT" HEADLESS-1 0 overlay-layer \
  < "$ANDROMEDA_RUNTIME_DIR/cover.fifo" > "$ANDROMEDA_RUNTIME_DIR/cover.log" 2>&1 &
exec {cover_fd}> "$ANDROMEDA_RUNTIME_DIR/cover.fifo"
await_lines "$ANDROMEDA_RUNTIME_DIR/cover.log" ready 1
echo "color blue FF0000FF" >&"$cover_fd"
await_lines "$ANDROMEDA_RUNTIME_DIR/cover.log" "done blue" 1

LAYER_NAMESPACE=noctalia-live-activity "$ANDROMEDA_LAYER_CLIENT" HEADLESS-1 0 overlay-layer \
  > "$ANDROMEDA_RUNTIME_DIR/status.log" 2>&1 &
await_lines "$ANDROMEDA_RUNTIME_DIR/status.log" ready 1
"$ANDROMEDA" settle > /dev/null

grim "$ANDROMEDA_RUNTIME_DIR/stack.png"
read -r r _ b _ < <("$ANDROMEDA_PIXEL_PROBE" "$ANDROMEDA_RUNTIME_DIR/stack.png" mean 20x20+90+90)
((b > 200 && r < 50)) || { echo "a status surface mapped over the screenshot cover: centre rgb mean r=$r b=$b"; exit 1; }
