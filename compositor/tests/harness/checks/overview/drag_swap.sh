#!/usr/bin/env bash
# Strip overview: dropping a window on the middle of a window in the next workspace's row swaps the two across
# workspaces; a plain move would leave the target where it was.
set -euo pipefail
source "$(dirname "$ANDROMEDA_HARNESS_LIB")/title_bar.sh"
source "$ANDROMEDA_HARNESS_LIB"
cat >> "$ANDROMEDA_CONFIG" <<'CONFIG'
[animation]
enabled = false
[appearance.title_bar]
mode = "off"
[layout]
mode = "dwindle"
[overview]
style = "strip"
# A small row brings the middle of the next row's window on screen (the default row leaves only its top edge visible).
zoom = 0.3
background_blur = false
CONFIG
"$ANDROMEDA" msg config-reload > /dev/null
FILL_COLOR=0xFFFF0000 "$ANDROMEDA_UNMAP_CLIENT" first 480 300 > "$ANDROMEDA_RUNTIME_DIR/first.log" 2>&1 &
title_bar_wait_field first floating false
"$ANDROMEDA" msg workspace-switch:2 > /dev/null
FILL_COLOR=0xFF0000FF "$ANDROMEDA_UNMAP_CLIENT" second 480 300 > "$ANDROMEDA_RUNTIME_DIR/second.log" 2>&1 &
title_bar_wait_field second floating false
"$ANDROMEDA" msg workspace-switch:1 > /dev/null
"$ANDROMEDA" settle > /dev/null
ws_of() { title_bar_window "$1" | jq -r '.workspace'; }
first_ws=$(ws_of first)
second_ws=$(ws_of second)
[[ $first_ws != "$second_ws" ]] || { echo "expected two workspaces, both on $first_ws"; exit 1; }
"$ANDROMEDA" msg overview-open > /dev/null
"$ANDROMEDA" settle > /dev/null
# Window centres from the screen: the current row holds red, the next row (dimmed neighbour) blue.
IMAGE="$ANDROMEDA_RUNTIME_DIR/drag-swap.png"
grim -o HEADLESS-1 "$IMAGE"
centre() {
  local x y w h
  read -r x y w h < <("$ANDROMEDA_PIXEL_PROBE" "$IMAGE" bbox "$1")
  ((w > 0)) || { echo "no $2 window on screen" >&2; return 1; }
  echo "$((x + w / 2)) $((y + h / 2))"
}
read -r ax ay < <(centre 'r > 0.6 && g < 0.3 && b < 0.3' red)
read -r bx by < <(centre 'b > 0.4 && r < 0.2 && g < 0.2' blue)
pointer_hold 1280 720 move "$ax" "$ay" press 272 move "$ax" $((ay + 40)) move $((bx - 20)) "$by" move "$bx" "$by" \
  -- release 272
pointer_release
"$ANDROMEDA" settle > /dev/null
"$ANDROMEDA" msg overview-close > /dev/null
[[ $(ws_of first) == "$second_ws" && $(ws_of second) == "$first_ws" ]] ||
  { echo "no swap: first on $(ws_of first), second on $(ws_of second) (was $first_ws, $second_ws)"; exit 1; }
echo "a drop on the middle of a window in another workspace swapped the two"
