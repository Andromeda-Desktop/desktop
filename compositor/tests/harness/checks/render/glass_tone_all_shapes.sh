#!/usr/bin/env bash
# Every glass shape of a surface counts toward its backdrop tone, weighted by area: a small first shape over black and
# a larger second shape over a white surface give a light backdrop. Only the first shape counted before, so a bar's
# islands all took the tone under its leftmost island.
set -euo pipefail
source "$ANDROMEDA_HARNESS_LIB"

readonly LAYER_LOG="$ANDROMEDA_RUNTIME_DIR/glass-tone-layer.log"

cat >> "$ANDROMEDA_CONFIG" <<'CONFIG'

[animation]
enabled = false
[appearance.blur]
enabled = true
[colors]
backdrop = "#000000FF"
# The optimized wallpaper cache omits the bottom-layer white surface; live blur samples it.
[[layer_rule]]
match.namespace = "^glass-tone-all-shapes$"
blur_optimized = false
CONFIG
"$ANDROMEDA" msg config-reload > /dev/null

mkfifo "$ANDROMEDA_RUNTIME_DIR/layer.fifo"
env LAYER_CONTROL=1 LAYER_DSK_VERSION=10 LAYER_NAMESPACE=glass-tone-all-shapes "$ANDROMEDA_LAYER_CLIENT" HEADLESS-1 200 \
  < "$ANDROMEDA_RUNTIME_DIR/layer.fifo" > "$LAYER_LOG" 2>&1 &
exec {layer_fd}> "$ANDROMEDA_RUNTIME_DIR/layer.fifo"
await_lines "$LAYER_LOG" ready 1

layer_command() {
  local action=$1 label=$2
  shift 2
  printf '%s %s %s\n' "$action" "$label" "$*" >&"$layer_fd"
  await_lines "$LAYER_LOG" "done $label" 1
}
# The white fill also colours the source surface; the glass samples only the backdrop under it.
layer_command color white FFFFFFFF
layer_command source white-under 300 50
await_lines "$LAYER_LOG" source-mapped 1
layer_command color clear 00000000
layer_command dsk-shape a-liquid 6 1 1 0 0
layer_command dsk-shape a-clip 3 10 10 20 20
layer_command dsk-new-shape b
layer_command dsk-shape b-liquid 6 1 1 0 0
layer_command dsk-shape b-clip 3 320 60 60 30
if ! await_lines "$LAYER_LOG" "dsk-backdrop 0" 1; then
  echo "a 20x20 shape over black and a 60x30 shape inside a white surface did not give a light backdrop: $(grep dsk-backdrop "$LAYER_LOG" | tr '\n' ' ')"
  exit 1
fi
