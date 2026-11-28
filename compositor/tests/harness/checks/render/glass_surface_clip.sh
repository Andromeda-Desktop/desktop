#!/usr/bin/env bash
# dsk_surface_v1 glass (v24): with no blur region, glass a = 1 turns the surface glass on in its rounded clip; without
# it the surface draws no glass. A detached sheet morphs its clip, so its glass needs no region per frame.
set -euo pipefail
source "$ANDROMEDA_HARNESS_LIB"

readonly LAYER_LOG="$ANDROMEDA_RUNTIME_DIR/glass-surface-clip-layer.log"
readonly SCREENSHOT="$ANDROMEDA_RUNTIME_DIR/glass-surface-clip.png"

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
env LAYER_CONTROL=1 LAYER_DSK_VERSION=24 "$ANDROMEDA_LAYER_CLIENT" HEADLESS-1 200 \
  < "$ANDROMEDA_RUNTIME_DIR/layer.fifo" > "$LAYER_LOG" 2>&1 &
exec {layer_fd}> "$ANDROMEDA_RUNTIME_DIR/layer.fifo"
await_lines "$LAYER_LOG" ready 1

layer_command() {
  local action=$1 label=$2
  shift 2
  printf '%s %s %s\n' "$action" "$label" "$*" >&"$layer_fd"
  await_lines "$LAYER_LOG" "done $label" 1
}
red() {
  local r g b
  "$ANDROMEDA" settle > /dev/null
  grim -o HEADLESS-1 "$SCREENSHOT"
  read -r r g b < <("$ANDROMEDA_PIXEL_PROBE" "$SCREENSHOT" pixel "$1" "$2")
  (( r >= 200 && g <= 55 ))
}

layer_command color clear 00000000
layer_command dsk-set tint 8 1 0 0 1
layer_command dsk-set liquid 6 0.2 0.3 0 0
layer_command dsk-set clip 3 100 40 200 100
layer_command dsk-set radius 4 24 0 0 0
if red 200 90; then
  echo "the surface draws glass with neither a blur region nor glass"
  exit 1
fi
layer_command dsk-set glass 16 1 0 0 0
red 200 90 || { echo "glass 1 does not turn the surface glass on in its clip (200, 90)"; exit 1; }
if red 102 42; then
  echo "the glass corner (102, 42) is not rounded by the clip radius"
  exit 1
fi
if red 200 150; then
  echo "the glass draws outside the clip (200, 150)"
  exit 1
fi
layer_command dsk-set clip2 3 100 40 200 140
red 200 160 || { echo "the glass does not follow a new clip (200, 160)"; exit 1; }
layer_command dsk-set off 16 0 0 0 0
if red 200 90; then
  echo "glass 0 leaves the surface glass on"
  exit 1
fi
