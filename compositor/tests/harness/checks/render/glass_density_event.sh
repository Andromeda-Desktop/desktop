#!/usr/bin/env bash
# dsk_surface_v1.density reports the density the compositor gives the tint: full for dark glass over a bright backdrop,
# none for light glass over the same backdrop. The shell's card fills follow this value, not a guess of their own.
set -euo pipefail
source "$ANDROMEDA_HARNESS_LIB"

readonly LAYER_LOG="$ANDROMEDA_RUNTIME_DIR/glass-density-layer.log"

cat >> "$ANDROMEDA_CONFIG" <<'CONFIG'

[animation]
enabled = false
[appearance.blur]
enabled = true
[colors]
backdrop = "#FFFFFFFF"
[[layer_rule]]
match.namespace = "^glass-density-event$"
blur_optimized = false
CONFIG
"$ANDROMEDA" msg config-reload > /dev/null

mkfifo "$ANDROMEDA_RUNTIME_DIR/layer.fifo"
env LAYER_CONTROL=1 LAYER_DSK_VERSION=16 LAYER_NAMESPACE=glass-density-event "$ANDROMEDA_LAYER_CLIENT" HEADLESS-1 200 \
  < "$ANDROMEDA_RUNTIME_DIR/layer.fifo" > "$LAYER_LOG" 2>&1 &
exec {layer_fd}> "$ANDROMEDA_RUNTIME_DIR/layer.fifo"
await_lines "$LAYER_LOG" ready 1

layer_command() {
  local action=$1 label=$2
  shift 2
  printf '%s %s %s\n' "$action" "$label" "$*" >&"$layer_fd"
  await_lines "$LAYER_LOG" "done $label" 1
}
# The glass samples the white surface under it, not its own buffer.
layer_command color white FFFFFFFF
layer_command blur-region area 100 50 100 50
layer_command source white-under 100 50
await_lines "$LAYER_LOG" source-mapped 1
layer_command dsk-set liquid 6 1 1 0 1
layer_command dsk-set dark-tint 8 0.1 0.1 0.1 0.5
if ! await_lines "$LAYER_LOG" "dsk-density 1.00" 1; then
  echo "dark glass over a white backdrop did not report full density: $(grep -E 'dsk-(density|backdrop)' "$LAYER_LOG" | tr '\n' ' ')"
  exit 1
fi
layer_command dsk-set light-tint 8 1 1 1 0.5
if ! await_lines "$LAYER_LOG" "dsk-density 0.00" 1; then
  echo "light glass over a white backdrop did not report zero density: $(grep -E 'dsk-(density|backdrop)' "$LAYER_LOG" | tr '\n' ' ')"
  exit 1
fi
