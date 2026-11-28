#!/usr/bin/env bash
# A dsk_shape_v1 rotation (v22) turns the glass shape about its clip centre, clockwise: a 200 x 40 red shape at 45
# degrees covers its rotated long axis, not the end of its unrotated box. The turn is logical: it holds on an output
# rotated 90 degrees and on a flipped one.
set -euo pipefail
source "$ANDROMEDA_HARNESS_LIB"

readonly LAYER_LOG="$ANDROMEDA_RUNTIME_DIR/glass-shape-rotation-layer.log"
readonly SCREENSHOT="$ANDROMEDA_RUNTIME_DIR/glass-shape-rotation.png"

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
env LAYER_CONTROL=1 LAYER_DSK_VERSION=22 "$ANDROMEDA_LAYER_CLIENT" HEADLESS-1 200 \
  < "$ANDROMEDA_RUNTIME_DIR/layer.fifo" > "$LAYER_LOG" 2>&1 &
exec {layer_fd}> "$ANDROMEDA_RUNTIME_DIR/layer.fifo"
await_lines "$LAYER_LOG" ready 1

layer_command() {
  local action=$1 label=$2
  shift 2
  printf '%s %s %s\n' "$action" "$label" "$*" >&"$layer_fd"
  await_lines "$LAYER_LOG" "done $label" 1
}
pixel() {
  "$ANDROMEDA" settle > /dev/null
  grim -o HEADLESS-1 "$SCREENSHOT"
  "$ANDROMEDA_PIXEL_PROBE" "$SCREENSHOT" pixel "$1" "$2"
}
red() {
  local r g b
  read -r r g b < <(pixel "$1" "$2")
  (( r >= 200 && g <= 55 ))
}

layer_command color clear 00000000
# Opaque red tint over the clip 100 80 200 40 (centre 200, 100), turned 45 degrees.
layer_command dsk-shape tint 8 1 0 0 1
layer_command dsk-shape radius 4 8 0 0 0
layer_command dsk-shape clip 3 100 80 200 40
layer_command dsk-shape rotation 14 0.785398 0 0 0
expect_rotated() {
  red 200 100 || { echo "$1: the rotated shape does not cover its centre"; exit 1; }
  red 250 150 || { echo "$1: the shape does not cover its long axis turned clockwise (250, 150)"; exit 1; }
  if red 290 100; then
    echo "$1: the end of the unrotated box (290, 100) still draws glass"
    exit 1
  fi
  if red 250 50; then
    echo "$1: the long axis turned counter-clockwise (250, 50) draws glass"
    exit 1
  fi
}
expect_rotated normal
readonly BASE="$ANDROMEDA_RUNTIME_DIR/glass-shape-rotation-base.toml"
cp "$ANDROMEDA_CONFIG" "$BASE"
for transform in 90 flipped-90; do
  cp "$BASE" "$ANDROMEDA_CONFIG"
  printf '\n[output."HEADLESS-1"]\ntransform = "%s"\n' "$transform" >> "$ANDROMEDA_CONFIG"
  "$ANDROMEDA" msg config-reload > /dev/null
  "$ANDROMEDA" outputs --json | jq -e --arg t "$transform" '.[0].transform == $t' > /dev/null \
    || { echo "the output did not take transform $transform"; exit 1; }
  expect_rotated "transform $transform"
done
