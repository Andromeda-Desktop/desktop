#!/usr/bin/env bash
# Desktop Layouts, Windows: the taskbar row's Show Desktop strip at the far end minimizes the workspace's windows on a
# click and brings them back on the next one (DESIGN.md "Desktop Layouts"; .agent/TASK-overview.md DL2, DL4).
# Fails when the show_desktop widget is missing from the bar, does not take the click, or the shell has no
# show-desktop command.
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/taskbar-show-desktop}
source "$(dirname "$0")/lib.sh"
UNMAP=$ROOT/compositor/build-debug/tests/unmap-client
POINTER=$ROOT/compositor/build-debug/tests/pointer-client

boot_headless 1
printf '\n[layout]\nmode = "floating"\n' >> "$RUNTIME/andromeda.toml"
run "$ANDROMEDA" msg config-reload > /dev/null
cat >> "$RUNTIME/home/.config/andromeda/shell/config.toml" <<'CONFIG'

[bar.default]
position = "bottom"
islands = false
start = []
center = ["start", "windows_taskbar"]
thickness = 48
margin_ends = 0
margin_edge = 0
padding = 0
widget_spacing = 4
radius_top_left = 0
radius_top_right = 0
radius_bottom_left = 0
radius_bottom_right = 0
end = ["tray", "group:status", "windows_clock", "notifications", "show_desktop"]
[[bar.default.capsule_group]]
id = "status"
members = ["status_network", "status_volume", "status_battery"]
fill = "#00000000"
radius = 8
hover_as_one = false

[dock]
enabled = false
CONFIG

with_noctalia '
  fail() { echo "FAIL: $*"; exit 1; }
  minimized() { "$ANDROMEDA" windows --json | jq -r ".[] | select(.title == \"desk-a\") | .minimized"; }
  minimized_is() { [[ $(minimized) == "$1" ]]; }
  wait_minimized() { poll_for 3 minimized_is "$1"; }
  APP_ID=desk-a FILL_COLOR=0xFF3060C0 "'"$UNMAP"'" desk-a 600 400 > "$OUT/client.log" 2>&1 &
  WAIT_S=5 wait_for "the window did not map" minimized_is false
  "$NOCTALIA" msg status > /dev/null; "$ANDROMEDA" settle > /dev/null
  # Wait for the bar to reserve its row, then click the 8 px strip at the far end of the flush row.
  bar_reserved() {
    area=$("$ANDROMEDA" outputs --json | jq -r ".[0].usable_area.height // empty" 2> /dev/null || true)
    [[ -n $area && $area -lt 720 ]]
  }
  WAIT_S=10 wait_for "the bottom bar reserved no row" bar_reserved
  y=$(( (720 + area) / 2 ))
  grim "$OUT/before.png"
  row="0,$area 1280x$((720 - area))"
  # Hover states (board section 1). The tray group centre comes from the trailing widths: Show Desktop 8, bell cell
  # 40, clock about 72, 4 apart, then the 80 wide status group.
  "'"$POINTER"'" 1280 720 move 1108 "$y" > /dev/null
  "$NOCTALIA" msg status > /dev/null; "$ANDROMEDA" settle > /dev/null
  grim -g "$row" "$OUT/hover-tray.png"
  "'"$POINTER"'" 1280 720 move 1276 "$y" > /dev/null
  "$NOCTALIA" msg status > /dev/null; "$ANDROMEDA" settle > /dev/null
  grim -g "$row" "$OUT/hover-show-desktop.png"
  "'"$POINTER"'" 1280 720 move 1276 "$y" click 272 > /dev/null
  wait_minimized true || fail "the Show Desktop strip did not minimize the window"
  grim "$OUT/shown.png"
  "'"$POINTER"'" 1280 720 move 1276 "$y" click 272 > /dev/null
  wait_minimized false || fail "a second click did not bring the window back"
  # Window previews (R6): rest on the app button (right of Start at the row centre) past the 400 ms open delay.
  # The popover has no IPC state, so the shot is the check; it opens on a timer, not an event.
  "'"$POINTER"'" 1280 720 move 651 "$y" > /dev/null
  sleep 0.9
  "$NOCTALIA" msg status > /dev/null; "$ANDROMEDA" settle > /dev/null
  grim "$OUT/preview.png"
  # Three windows of one app share one button (combine_apps); the pointer rests on the close button of the middle
  # card (24, 4 in from the card edge, in the 24 header).
  "'"$POINTER"'" 1280 720 move 900 200 > /dev/null
  APP_ID=desk-a FILL_COLOR=0xFFC06030 "'"$UNMAP"'" desk-a2 600 400 > "$OUT/client-a2.log" 2>&1 &
  a2=$!
  APP_ID=desk-a FILL_COLOR=0xFF9040B0 "'"$UNMAP"'" desk-a3 600 400 > "$OUT/client-a3.log" 2>&1 &
  a3=$!
  three_windows() { [[ $("$ANDROMEDA" windows --json | jq length) -ge 3 ]]; }
  poll_for 5 three_windows || true
  "$NOCTALIA" msg status > /dev/null; "$ANDROMEDA" settle > /dev/null
  "'"$POINTER"'" 1280 720 move 651 "$y" > /dev/null
  sleep 0.9
  "$NOCTALIA" msg status > /dev/null; "$ANDROMEDA" settle > /dev/null
  # Three cards on the one desk-a button: 632 wide centred on x 651, 180 high, 8 above the 40 cell (4 above the row).
  top=$(( area - 4 - 180 ))
  "'"$POINTER"'" 1280 720 move 651 $(( top + 76 )) move 735 $(( top + 24 )) > /dev/null
  "$NOCTALIA" msg status > /dev/null; "$ANDROMEDA" settle > /dev/null
  grim "$OUT/preview-3-close.png"
  "'"$POINTER"'" 1280 720 move 900 200 > /dev/null
  kill "$a2" "$a3"
  one_window() { [[ $("$ANDROMEDA" windows --json | jq length) -eq 1 ]]; }
  poll_for 5 one_window || true
  # Snap group (board desktop-menus.html B): Snap Layouts puts desk-a left, snap assist puts desk-b right, and the
  # desk-a preview opens with the group card first. The buttons are now Start, desk-a, desk-b around the row centre.
  "'"$POINTER"'" 1280 720 move 900 200 > /dev/null
  APP_ID=desk-b FILL_COLOR=0xFF30A060 "'"$UNMAP"'" desk-b 600 400 > "$OUT/client-b.log" 2>&1 &
  window() { "$ANDROMEDA" windows --json | jq -c ".[] | select(.title == \"$1\")"; }
  desk_b_mapped() { [[ -n $(window desk-b) ]]; }
  poll_for 5 desk_b_mapped || true
  "$ANDROMEDA" msg workspace-set-layout:floating > /dev/null
  "$ANDROMEDA" msg "window-focus:$(window desk-a | jq -r .id)" > /dev/null
  "$ANDROMEDA" msg window-snap-layouts > /dev/null
  "'"$POINTER"'" 1280 720 tap 28 > /dev/null
  desk_a_left() { [[ $(window desk-a | jq -r .snap) == 1.1 ]]; }
  WAIT_S=3 wait_for "Snap Layouts did not put desk-a in the left half" desk_a_left
  # Snap assist opens once the snap motion ends (compositor/tests/harness/checks/layout/snap_assist.sh runs with
  # animations off).
  "$ANDROMEDA" settle > /dev/null
  "'"$POINTER"'" 1280 720 tap 28 > /dev/null
  desk_b_right() { [[ $(window desk-b | jq -r .snap) == 1.2 ]]; }
  poll_for 3 desk_b_right || true
  group=$(window desk-a | jq -r .snap_group)
  [[ $group =~ ^[0-9]+$ && $(window desk-b | jq -r .snap_group) == "$group" ]] || fail "desk-a and desk-b formed no snap group (got $group)"
  "$NOCTALIA" msg status > /dev/null; "$ANDROMEDA" settle > /dev/null
  "'"$POINTER"'" 1280 720 move 640 "$y" > /dev/null
  sleep 0.9
  "$NOCTALIA" msg status > /dev/null; "$ANDROMEDA" settle > /dev/null
  grim "$OUT/preview-group.png"
  # Dark taskbar at rest and with the pointer on an app button.
  "'"$POINTER"'" 1280 720 move 900 200 > /dev/null
  "$NOCTALIA" msg theme-mode-set dark > /dev/null
  "$NOCTALIA" msg status > /dev/null; "$ANDROMEDA" settle > /dev/null
  stable_grim -g "$row" "$OUT/taskbar-dark.png"
  "'"$POINTER"'" 1280 720 move 640 "$y" > /dev/null
  "$NOCTALIA" msg status > /dev/null; "$ANDROMEDA" settle > /dev/null
  grim -g "$row" "$OUT/hover-app-dark.png"
  "$NOCTALIA" msg theme-mode-set light > /dev/null
  echo "PASS: Show Desktop minimized the window and brought it back"
'
