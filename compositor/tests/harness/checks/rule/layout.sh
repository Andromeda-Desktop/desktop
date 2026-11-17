#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$ANDROMEDA_HARNESS_LIB")/title_bar.sh"
title_bar_config
sed -i '/^\[appearance.title_bar\]$/ { n; s/mode = "on"/mode = "off"/; }' "$ANDROMEDA_CONFIG"
cat >> "$ANDROMEDA_CONFIG" <<'CONFIG'
[[window_rule]]
match.layout = ["floating", "dwindle"]
title_bar = "forced"
[[workspace]]
name = "DEST"
layout.mode = "dwindle"
CONFIG
"$ANDROMEDA" msg config-reload > /dev/null

env XDG_DECORATION=client "$ANDROMEDA_FRACTIONAL_CLIENT" layout-tile 480 300 > "$ANDROMEDA_RUNTIME_DIR/tile.log" 2>&1 &
env XDG_DECORATION=client "$ANDROMEDA_FRACTIONAL_CLIENT" ssd-float-layout 480 300 > "$ANDROMEDA_RUNTIME_DIR/float.log" 2>&1 &
title_bar_wait_field layout-tile title_bar false
title_bar_wait_field ssd-float-layout title_bar false
tile_id=$(title_bar_window layout-tile | jq -r .id)
float_id=$(title_bar_window ssd-float-layout | jq -r .id)

expect_bars() {
  local expected=$1 pixels
  title_bar_wait_field layout-tile title_bar "$expected"
  title_bar_wait_field ssd-float-layout title_bar "$expected"
  "$ANDROMEDA" settle > /dev/null
  grim -o HEADLESS-1 "$ANDROMEDA_RUNTIME_DIR/layout.png"
  pixels=$("$ANDROMEDA_PIXEL_PROBE" "$ANDROMEDA_RUNTIME_DIR/layout.png" count 'r > 0.95 && g < 0.05 && b > 0.95')
  if [[ $expected == true ]]; then
    ((pixels > 1000)) || { echo "title bars were not drawn: pixels=$pixels"; return 1; }
  else
    ((pixels == 0)) || { echo "title bars stayed drawn: pixels=$pixels"; return 1; }
  fi
}

expect_bars false
for mode in dwindle master floating scrolling; do
  "$ANDROMEDA" msg "workspace-set-layout:$mode" > /dev/null
  if [[ $mode == floating || $mode == dwindle ]]; then
    expect_bars true
  else
    expect_bars false
  fi
done

# Only the window's destination layout changes, not its floating state.
"$ANDROMEDA" msg "window-focus:$float_id" > /dev/null
"$ANDROMEDA" msg "window-move-to-workspace:DEST" > /dev/null
title_bar_wait_field ssd-float-layout title_bar true
"$ANDROMEDA" msg "window-focus:$tile_id" > /dev/null
title_bar_wait_field layout-tile title_bar false
"$ANDROMEDA" msg "window-move-to-workspace:DEST" > /dev/null
expect_bars true

# A newly mapped window uses its assigned workspace's layout.
env XDG_DECORATION=client "$ANDROMEDA_FRACTIONAL_CLIENT" layout-late 480 300 > "$ANDROMEDA_RUNTIME_DIR/late.log" 2>&1 &
title_bar_wait_field layout-late title_bar true

sed -i 's/match.layout = \["floating", "dwindle"\]/match.layout = "floating"/' "$ANDROMEDA_CONFIG"
"$ANDROMEDA" msg config-reload > /dev/null
expect_bars false
title_bar_wait_field layout-late title_bar false
echo 'layout rules refresh title bars across layout switches, workspace moves, mapping, and reload'
