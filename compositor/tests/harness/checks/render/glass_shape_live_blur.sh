#!/usr/bin/env bash
# dsk glass shapes blur the live content behind them with no layer rule, as blur regions do: dark glass over a white
# window gets density above 0.5 although the optimized wallpaper cache holds only the black backdrop.
set -euo pipefail
source "$ANDROMEDA_HARNESS_LIB"

readonly LAYER_LOG="$ANDROMEDA_RUNTIME_DIR/glass-shape-live.log"

cat >> "$ANDROMEDA_CONFIG" <<'CONFIG'

[animation]
enabled = false
[appearance.blur]
enabled = true
optimized = true
[colors]
backdrop = "#000000FF"
[[window_rule]]
match.title = "^white$"
default_floating = true
default_position = { x = 0, y = 0, anchor = "top_left" }
CONFIG
"$ANDROMEDA" msg config-reload > /dev/null

OPAQUE=1 FILL_COLOR=0xFFFFFFFF "$ANDROMEDA_UNMAP_CLIENT" white 600 400 > "$ANDROMEDA_RUNTIME_DIR/white.log" 2>&1 &
await_lines "$ANDROMEDA_RUNTIME_DIR/white.log" mapped 1

mkfifo "$ANDROMEDA_RUNTIME_DIR/layer.fifo"
env LAYER_CONTROL=1 LAYER_DSK_VERSION=16 LAYER_NAMESPACE=glass-shape-live "$ANDROMEDA_LAYER_CLIENT" HEADLESS-1 0 overlay-layer \
  < "$ANDROMEDA_RUNTIME_DIR/layer.fifo" > "$LAYER_LOG" 2>&1 &
exec {layer_fd}> "$ANDROMEDA_RUNTIME_DIR/layer.fifo"
await_lines "$LAYER_LOG" ready 1
"$ANDROMEDA" settle > /dev/null

layer_command() {
  local action=$1 label=$2
  shift 2
  printf '%s %s %s\n' "$action" "$label" "$*" >&"$layer_fd"
  await_lines "$LAYER_LOG" "done $label" 1
}
layer_command color clear 00000000
layer_command dsk-set dark-tint 8 0 0 0 0.5
layer_command dsk-shape a-liquid 6 1 1 0 0
layer_command dsk-shape a-tint 8 0 0 0 0.5
layer_command dsk-shape a-clip 3 20 20 100 50
last_density() { awk '$1 == "dsk-density" { v = $2 } END { print v + 0 }' "$LAYER_LOG"; }
for _ in $(seq 60); do
  awk -v d="$(last_density)" 'BEGIN { exit !(d >= 0.5) }' && exit 0
  sleep 0.1
done
echo "dark glass shape over a white window stayed below density 0.5 (wallpaper cache blur?): $(grep dsk-density "$LAYER_LOG" | tr '\n' ' ')"
exit 1
