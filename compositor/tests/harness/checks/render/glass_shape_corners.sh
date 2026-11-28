#!/usr/bin/env bash
# dsk_shape_v1 corners (v23): per-corner radii, and a concave flare pair that runs across its edge with join c = 1
# (a full-length bar's inner corners): each flare hugs its side below the body; nothing draws between them.
set -euo pipefail
source "$ANDROMEDA_HARNESS_LIB"

readonly LAYER_LOG="$ANDROMEDA_RUNTIME_DIR/glass-shape-corners-layer.log"
readonly SCREENSHOT="$ANDROMEDA_RUNTIME_DIR/glass-shape-corners.png"

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
env LAYER_CONTROL=1 LAYER_DSK_VERSION=23 "$ANDROMEDA_LAYER_CLIENT" HEADLESS-1 200 \
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
# Opaque red tint; body 100 40 200 40; top-left radius 16, the bottom corners flare 20 px.
layer_command dsk-shape tint 8 1 0 0 1
layer_command dsk-shape liquid 6 0.2 0.3 0 0
layer_command dsk-shape clip 3 100 40 200 40
layer_command dsk-shape corners 15 16 0 -20 -20
layer_command dsk-shape join 9 0 0 1 0
red 200 60 || { echo "the body does not draw glass"; exit 1; }
if red 102 42; then
  echo "the top-left corner (102, 42) is not rounded"
  exit 1
fi
red 297 42 || { echo "the square top-right corner (297, 42) draws no glass"; exit 1; }
red 102 84 || { echo "the bottom-left flare does not hug the left side below the body (102, 84)"; exit 1; }
red 297 84 || { echo "the bottom-right flare does not hug the right side below the body (297, 84)"; exit 1; }
if red 200 90; then
  echo "glass draws below the body between the flares (200, 90)"
  exit 1
fi
if red 95 78; then
  echo "a flare runs along the bottom edge (95, 78), not across it"
  exit 1
fi
