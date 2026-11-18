#!/usr/bin/env bash
# Spaces overview: dropping a window on the + thumb moves it to a new workspace.
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
style = "spaces"
CONFIG
"$ANDROMEDA" msg config-reload > /dev/null
"$ANDROMEDA_FRACTIONAL_CLIENT" card 480 300 > "$ANDROMEDA_RUNTIME_DIR/card.log" 2>&1 &
title_bar_wait_field card floating false
"$ANDROMEDA" settle > /dev/null
before=$(title_bar_window card | jq -r '.workspace')
count=$("$ANDROMEDA" workspaces --json | jq 'length')
"$ANDROMEDA" msg overview-open > /dev/null
"$ANDROMEDA" settle > /dev/null
# Spaces pieces: 140.8 x 79.2 at y 52, 16 apart, centred, a half-width + last.
plus_x=$(awk -v n="$count" 'BEGIN { printf "%d", (1280 - (n + 0.5) * 140.8 - n * 16) / 2 + n * 156.8 + 35.2 }')
pointer_hold 1280 720 move 640 443 press 272 move 640 403 move "$plus_x" 100 move "$plus_x" 91 -- release 272
pointer_release
"$ANDROMEDA" settle > /dev/null
after=$(title_bar_window card | jq -r '.workspace')
[[ $after != "$before" ]] || { echo "drop on + left the window on $before"; "$ANDROMEDA" workspaces --json; exit 1; }
"$ANDROMEDA" msg overview-close > /dev/null
echo "a drop on + moved the window from $before to the new workspace $after"
