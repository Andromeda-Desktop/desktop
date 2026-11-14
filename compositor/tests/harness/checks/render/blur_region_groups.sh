#!/usr/bin/env bash
# A layer blur region with a plain rect A and an L-shaped group B (two overlapping rects, no rounded rectangle): A is
# still its own glass shape, with the specular rim on its own edges. Before, one unreadable group made the whole region
# one glass shape over its extents, so the rim ran around the extents and A's right edge had none.
set -euo pipefail
source "$ANDROMEDA_HARNESS_LIB"

readonly LAYER_LOG="$ANDROMEDA_RUNTIME_DIR/blur-groups-layer.log"
readonly SCREENSHOT="$ANDROMEDA_RUNTIME_DIR/blur-groups.png"

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
env LAYER_CONTROL=1 LAYER_DSK_VERSION=10 "$ANDROMEDA_LAYER_CLIENT" HEADLESS-1 400 \
  < "$ANDROMEDA_RUNTIME_DIR/layer.fifo" > "$LAYER_LOG" 2>&1 &
exec {layer_fd}> "$ANDROMEDA_RUNTIME_DIR/layer.fifo"
await_lines "$LAYER_LOG" ready 1

layer_command() {
  local action=$1 label=$2
  shift 2
  printf '%s %s %s\n' "$action" "$label" "$*" >&"$layer_fd"
  await_lines "$LAYER_LOG" "done $label" 1
}
layer_command color clear 00000000
layer_command dsk-set rim 6 0 1 0 0
layer_command blur-region groups 100 100 200 100 500 100 200 100 600 150 200 150
"$ANDROMEDA" settle > /dev/null
grim -o HEADLESS-1 "$SCREENSHOT"

pixel() { magick "$SCREENSHOT" -crop "1x1+$1+$2" +repage -format "%[fx:int(255*u.r)] %[fx:int(255*u.g)]\n" info:; }
# The rim is a white highlight on the green backdrop: red rises at the shape edge, not in its middle.
read -r edge _ < <(pixel 299 150)
read -r middle _ < <(pixel 200 150)
if (( edge < 8 || middle > 2 )); then
  echo "group A has no rim on its own right edge: red $edge at 299,150, $middle at 200,150"
  exit 1
fi
