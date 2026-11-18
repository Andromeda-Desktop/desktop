#!/usr/bin/env bash
# Spaces overview: dragging the second workspace's thumb onto the first one moves that workspace to the front.
set -euo pipefail
source "$(dirname "$ANDROMEDA_HARNESS_LIB")/title_bar.sh"
source "$ANDROMEDA_HARNESS_LIB"
cat >> "$ANDROMEDA_CONFIG" <<'CONFIG'
[animation]
enabled = false
[appearance.title_bar]
mode = "off"
[overview]
style = "spaces"
CONFIG
"$ANDROMEDA" msg config-reload > /dev/null
"$ANDROMEDA_FRACTIONAL_CLIENT" first 480 300 > "$ANDROMEDA_RUNTIME_DIR/first.log" 2>&1 &
title_bar_wait_field first floating false
"$ANDROMEDA" msg workspace-switch:2 > /dev/null
"$ANDROMEDA_FRACTIONAL_CLIENT" second 480 300 > "$ANDROMEDA_RUNTIME_DIR/second.log" 2>&1 &
title_bar_wait_field second floating false
"$ANDROMEDA" settle > /dev/null
# A window's workspace field is the stable id; the position is that workspace's index.
slot() {
  "$ANDROMEDA" workspaces --json | jq -r --arg id "$(title_bar_window "$1" | jq -r '.workspace')" \
    '.[] | select(.id == $id) | .index'
}
[[ $(slot first) == 1 && $(slot second) == 2 ]] || { echo "expected first on 1, second on 2"; exit 1; }
count=$("$ANDROMEDA" workspaces --json | jq 'length')
"$ANDROMEDA" msg overview-open > /dev/null
"$ANDROMEDA" settle > /dev/null
# Spaces pieces (style.cpp placeBar on 1280 x 720): 140.8 x 79.2 at y 52, 16 apart, centred, a half-width + last.
x0=$(awk -v n="$count" 'BEGIN { printf "%d", (1280 - (n + 0.5) * 140.8 - n * 16) / 2 + 70.4 }')
pointer_hold 1280 720 move $((x0 + 157)) 91 press 272 move $((x0 + 120)) 91 move $((x0 + 10)) 91 move "$x0" 91 \
  -- release 272
pointer_release
"$ANDROMEDA" settle > /dev/null
"$ANDROMEDA" msg overview-close > /dev/null
[[ $(slot second) == 1 && $(slot first) == 2 ]] ||
  { echo "no reorder: first on $(slot first), second on $(slot second)"; exit 1; }
echo "dragging the second thumb onto the first moved that workspace to the front"
