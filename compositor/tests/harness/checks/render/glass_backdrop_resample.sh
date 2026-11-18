#!/usr/bin/env bash
# A backdrop change inside the backdrop probe's 100 ms interval still reaches liquid glass: the glass samples again
# once the interval ends, so glass that maps before the wallpaper takes the density of the new backdrop.
set -euo pipefail
source "$ANDROMEDA_HARNESS_LIB"

readonly LAYER_LOG="$ANDROMEDA_RUNTIME_DIR/glass-resample-layer.log"

cat >> "$ANDROMEDA_CONFIG" <<'CONFIG'

[animation]
enabled = false
[appearance.blur]
enabled = true
[colors]
backdrop = "#000000FF"
CONFIG
"$ANDROMEDA" msg config-reload > /dev/null

mkfifo "$ANDROMEDA_RUNTIME_DIR/layer.fifo"
env LAYER_CONTROL=1 LAYER_DSK_VERSION=16 "$ANDROMEDA_LAYER_CLIENT" HEADLESS-1 200 \
  < "$ANDROMEDA_RUNTIME_DIR/layer.fifo" > "$LAYER_LOG" 2>&1 &
exec {layer_fd}> "$ANDROMEDA_RUNTIME_DIR/layer.fifo"
await_lines "$LAYER_LOG" ready 1

layer_command() {
  local action=$1 label=$2
  shift 2
  printf '%s %s %s\n' "$action" "$label" "$*" >&"$layer_fd"
  await_lines "$LAYER_LOG" "done $label" 1 0.005
}
# The white fill also colours the source surface; the glass samples only the backdrop under it.
layer_command color white FFFFFFFF
layer_command blur-region area 100 50 100 50
layer_command dsk-set liquid 6 1 1 0 0
layer_command dsk-set light-tint 8 1 1 1 0.5
await_lines "$LAYER_LOG" "dsk-density 1.00" 1 0.005
# A white bottom-layer surface under the glass a few ms after the first sample, inside the probe interval; nothing
# redraws the glass after it.
layer_command source white-under 100 50
for _ in $(seq 60); do
  awk '$1 == "dsk-density" && $2 < 0.9 { found = 1 } END { exit !found }' "$LAYER_LOG" && break
  sleep 0.1
done
if ! awk '$1 == "dsk-density" && $2 < 0.9 { found = 1 } END { exit !found }' "$LAYER_LOG"; then
  echo "light glass kept full density after a white surface mapped under it inside the probe interval"
  exit 1
fi
