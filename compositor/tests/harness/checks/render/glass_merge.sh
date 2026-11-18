#!/usr/bin/env bash
# Two liquid glass shapes with the same material merge into one shape when the gap between them is under
# glass::kMergeReach: the red tint fills an 8 px gap. A 40 px gap keeps two shapes, and the gap shows the backdrop.
set -euo pipefail
source "$ANDROMEDA_HARNESS_LIB"

readonly LAYER_LOG="$ANDROMEDA_RUNTIME_DIR/glass-merge-layer.log"
readonly SCREENSHOT="$ANDROMEDA_RUNTIME_DIR/glass-merge.png"

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
glass_shape() {
  layer_command dsk-shape "$1-radius" 4 30 0 0 0
  layer_command dsk-shape "$1-liquid" 6 1 1 0 1
  layer_command dsk-shape "$1-tint" 8 1 0 0 1
  layer_command dsk-shape "$1-clip" 3 "$2" 50 200 100
}
# Red and green of the gap centre at y 100, the middle of the straight edges.
gap_pixel() {
  "$ANDROMEDA" settle > /dev/null
  grim -o HEADLESS-1 "$SCREENSHOT"
  "$ANDROMEDA_PIXEL_PROBE" "$SCREENSHOT" pixel "$1" 100
}

layer_command color clear 00000000
glass_shape a 100
layer_command dsk-new-shape b
glass_shape b 308
read -r r g _ < <(gap_pixel 304)
if (( r < 128 || g > 128 )); then
  echo "an 8 px gap between two liquid shapes is not merged glass: rgb at 304,100 is $r $g"
  exit 1
fi

layer_command dsk-shape b-far 3 340 50 200 100
read -r r g _ < <(gap_pixel 320)
if (( r > 32 || g < 200 )); then
  echo "a 40 px gap between two liquid shapes does not show the backdrop: rgb at 320,100 is $r $g"
  exit 1
fi
