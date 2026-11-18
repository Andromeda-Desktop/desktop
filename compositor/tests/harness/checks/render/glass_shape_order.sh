#!/usr/bin/env bash
# Glass shapes draw in creation order (dsk_surface_v1.get_shape): where a red shape created second overlaps a blue
# shape created first, red shows. A third shape keeps the order of the first two.
set -euo pipefail
source "$ANDROMEDA_HARNESS_LIB"

readonly LAYER_LOG="$ANDROMEDA_RUNTIME_DIR/glass-shape-order-layer.log"
readonly SCREENSHOT="$ANDROMEDA_RUNTIME_DIR/glass-shape-order.png"

cat >> "$ANDROMEDA_CONFIG" <<'CONFIG'

[animation]
enabled = false
[appearance.blur]
enabled = true
[colors]
backdrop = "#00FF00FF"
CONFIG
"$ANDROMEDA" msg config-reload > /dev/null

mkfifo "$ANDROMEDA_RUNTIME_DIR/layer.fifo"
env LAYER_CONTROL=1 LAYER_DSK_VERSION=10 "$ANDROMEDA_LAYER_CLIENT" HEADLESS-1 200 \
  < "$ANDROMEDA_RUNTIME_DIR/layer.fifo" > "$LAYER_LOG" 2>&1 &
exec {layer_fd}> "$ANDROMEDA_RUNTIME_DIR/layer.fifo"
await_lines "$LAYER_LOG" ready 1

layer_command() {
  local action=$1 label=$2
  shift 2
  printf '%s %s %s\n' "$action" "$label" "$*" >&"$layer_fd"
  await_lines "$LAYER_LOG" "done $label" 1
}
# An opaque tint (r g b) over the clip x 50 200 100.
tinted_shape() {
  layer_command dsk-shape "$1-tint" 8 "$2" "$3" "$4" 1
  layer_command dsk-shape "$1-clip" 3 "$5" 50 200 100
}
pixel() {
  "$ANDROMEDA" settle > /dev/null
  grim -o HEADLESS-1 "$SCREENSHOT"
  "$ANDROMEDA_PIXEL_PROBE" "$SCREENSHOT" pixel "$1" 100
}
expect_red() {
  local r g b
  read -r r g b < <(pixel "$1")
  if (( r < 200 || b > 55 )); then
    echo "$2: rgb at $1,100 is $r $g $b, not the later red shape"
    exit 1
  fi
}

layer_command color clear 00000000
tinted_shape a 0 0 1 100
layer_command dsk-new-shape b
tinted_shape b 1 0 0 200
expect_red 250 "a shape created second draws under the first"
layer_command dsk-new-shape c
tinted_shape c 0 0 1 400
expect_red 250 "a third shape changes the order of the first two"
