#!/usr/bin/env bash
# While a sheet's dsk motion runs, its density stays at the last value sent: each change would retint every label of
# the sheet in each frame of the open. The value for the backdrop where the motion ends follows when it settles.
set -euo pipefail
source "$ANDROMEDA_HARNESS_LIB"

readonly LAYER_LOG="$ANDROMEDA_RUNTIME_DIR/glass-density-motion-layer.log"

cat >> "$ANDROMEDA_CONFIG" <<'CONFIG'

[animation]
enabled = true
[appearance.blur]
enabled = true
[colors]
backdrop = "#000000FF"
[[layer_rule]]
match.namespace = "^glass-density-motion$"
blur_optimized = false
CONFIG
"$ANDROMEDA" msg config-reload > /dev/null

mkfifo "$ANDROMEDA_RUNTIME_DIR/layer.fifo"
env LAYER_CONTROL=1 LAYER_DSK_VERSION=16 LAYER_NAMESPACE=glass-density-motion "$ANDROMEDA_LAYER_CLIENT" HEADLESS-1 200 \
  < "$ANDROMEDA_RUNTIME_DIR/layer.fifo" > "$LAYER_LOG" 2>&1 &
exec {layer_fd}> "$ANDROMEDA_RUNTIME_DIR/layer.fifo"
await_lines "$LAYER_LOG" ready 1

layer_command() {
  local action=$1 label=$2
  shift 2
  printf '%s %s %s\n' "$action" "$label" "$*" >&"$layer_fd"
  await_lines "$LAYER_LOG" "done $label" 1
}
# Dark glass over the white source reads a high density (the blur edge takes in some black); over the black backdrop none.
layer_command color white FFFFFFFF
layer_command blur-region area 100 50 100 50
layer_command source white-under 100 50
await_lines "$LAYER_LOG" source-mapped 1
layer_command dsk-set liquid 6 1 1 0 1
layer_command dsk-set dark-tint 8 0.1 0.1 0.1 0.5
if ! await_lines "$LAYER_LOG" 'dsk-density \(0\.[5-9]\|1\.00\)' 1; then
  echo "dark glass over the white source did not report a high density: $(grep -E 'dsk-density' "$LAYER_LOG" | tr '\n' ' ')"
  exit 1
fi
# The glass slides off the source onto the black backdrop.
layer_command dsk-animate slide 1 0 7 600 0 0 0
await_lines "$LAYER_LOG" "dsk-done 1 0" 1
settled_density() { sed -n '/dsk-done 1/,$p' "$LAYER_LOG" | grep -q 'dsk-density 0.00'; }
if ! await_until 60 0.1 settled_density; then
  echo "the settled glass over the black backdrop did not report zero density: $(grep -E 'dsk-' "$LAYER_LOG" | tr '\n' ' ')"
  exit 1
fi
during=$(sed -n '/dsk-started 1/,/dsk-done 1/p' "$LAYER_LOG" | grep -c 'dsk-density' || true)
if [[ $during != 0 ]]; then
  echo "density changed $during times during the motion: $(grep -E 'dsk-' "$LAYER_LOG" | tr '\n' ' ')"
  exit 1
fi
