#!/usr/bin/env bash
# The Strip row name's layout chip opens the arrangement menu; a click on a row switches that workspace's arrangement and
# closes the menu, and a click outside only closes it (OV8).
set -euo pipefail

readonly BTN_LEFT=272
readonly OUTPUT_W=1280
readonly OUTPUT_H=720
readonly POINTER="${ANDROMEDA_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
IMAGE="$ANDROMEDA_RUNTIME_DIR/arrangement_menu.png"

pointer() {
  "$POINTER" "$OUTPUT_W" "$OUTPUT_H" "$@"
}

# The chrome takes its ink and fills from the scheme tokens (dark here): the chip is found by its thin fill, the menu by
# its solid fill.
cat >> "$ANDROMEDA_CONFIG" <<'EOF2'

[layout]
mode = "scrolling"

[overview]
style = "strip"
background_blur = false
workspace_wallpaper = false
titles = "hover"

[animation]
enabled = false
EOF2
"$ANDROMEDA" msg config-reload > /dev/null

FILL_COLOR=0xFF203040 "$ANDROMEDA_UNMAP_CLIENT" menu-a 500 400 > /dev/null 2>&1 &
for _ in $(seq 60); do
  [[ $("$ANDROMEDA" windows --json | jq 'length') -eq 1 ]] && break
  sleep 0.05
done
"$ANDROMEDA" settle
"$ANDROMEDA" msg overview-open > /dev/null
"$ANDROMEDA" settle

layout() { "$ANDROMEDA" workspaces --json | jq -r '.[] | select(.active) | .layout'; }
ink='r > 0.8 && g > 0.8 && b > 0.8'
thin='r > 0.08 && r < 0.2 && b > r'
# solid_regular at .97: the window cards are 0xFF203040, so under the chip only the menu is this neutral grey.
solid='r > 0.11 && r < 0.16 && g > 0.11 && g < 0.16 && b > 0.12 && b < 0.17'
menu_box() { "$ANDROMEDA_PIXEL_PROBE" "$IMAGE" blob "$solid" "370x240+$((cx - 8))+$((cy + ch))"; }
# Height of the menu panel; 0 when the menu is closed.
menu_height() {
  grim -o HEADLESS-1 "$IMAGE"
  menu_box | awk '{print $4}'
}

grim -o HEADLESS-1 "$IMAGE"
# The row frame is the largest patch of label ink; the chip is the thin-fill capsule just above its top edge.
read -r _ fy _ _ < <("$ANDROMEDA_PIXEL_PROBE" "$IMAGE" blob "$ink" "500x120+150+60")
read -r cx cy cw ch < <("$ANDROMEDA_PIXEL_PROBE" "$IMAGE" blob "$thin" "500x40+150+$((fy - 40))")
if ((cw == 0)); then
  echo "no layout chip on the Strip row name"
  exit 1
fi
pointer move $((cx + cw / 2)) $((cy + ch / 2)) click "$BTN_LEFT"
"$ANDROMEDA" settle
grim -o HEADLESS-1 "$IMAGE"
read -r mx my mw mh < <(menu_box)
if ((mh < 200 || mw > 300)); then
  echo "clicking the chip did not open the menu (panel ${mw}x${mh})"
  exit 1
fi

# A press outside the menu closes it and changes nothing.
pointer move $((OUTPUT_W - 40)) $((OUTPUT_H - 40)) click "$BTN_LEFT"
"$ANDROMEDA" settle
(($(menu_height) == 0)) || { echo "a click outside left the menu open"; exit 1; }
[[ $(layout) == scrolling ]] || { echo "a click outside changed the arrangement to $(layout)"; exit 1; }

# Rows: 6 px pad, 26 px header, then 48 px each; the fourth is Main + Stack.
pointer move $((cx + cw / 2)) $((cy + ch / 2)) click "$BTN_LEFT"
"$ANDROMEDA" settle
pointer move $((mx + 60)) $((my + 6 + 26 + 3 * 48 + 24)) click "$BTN_LEFT"
"$ANDROMEDA" settle
[[ $(layout) == master ]] || { echo "picking Main + Stack set the arrangement to $(layout)"; exit 1; }
pointer move $((OUTPUT_W - 40)) $((OUTPUT_H - 40))
(($(menu_height) == 0)) || { echo "the menu stayed open after a pick"; exit 1; }
echo "the chip opens the arrangement menu; a row switches the arrangement, a click outside only closes it"
