#!/usr/bin/env bash
# A click on a hidden tab's slot in a tab group's overview card leaves the overview on that tab; sliding off the slot
# before letting go cancels.
set -euo pipefail

readonly CLIENT="${ANDROMEDA_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly POINTER="${ANDROMEDA_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
readonly OVERVIEW="$ANDROMEDA_RUNTIME_DIR/overview-tab-click.png"
readonly BTN_LEFT=272
readonly OUTPUT_W=1280
readonly OUTPUT_H=720
# The shown tab's slot is the only magenta on screen.
readonly MAGENTA='r > 0.9 && g < 0.1 && b > 0.9'

for tool in "$CLIENT" "$POINTER"; do
  if [[ ! -x $tool ]]; then
    echo "$tool is not built"
    exit 1
  fi
done

pointer() { "$POINTER" "$OUTPUT_W" "$OUTPUT_H" "$@"; }

cat >> "$ANDROMEDA_CONFIG" <<'EOF'

[animation]
enabled = false

[appearance.shadow]
enabled = false

[overview]
background_blur = false

[appearance.tab_bar]
style = "indicator"
height = 24

[colors.tab_bar]
background = "#202020FF"
active = "#FF00FFFF"
active_unfocused = "#FF00FFFF"
EOF
"$ANDROMEDA" msg config-reload > /dev/null

window_of() { "$ANDROMEDA" windows --json | jq -c --arg title "$1" '.[] | select(.title == $title)'; }
focused_title() { "$ANDROMEDA" windows --json | jq -r '.[] | select(.focused) | .title'; }

spawn() {
  local title=$1 color=$2
  RESIZE_FILL_COLOR="$color" "$CLIENT" "$title" 600 400 > "$ANDROMEDA_RUNTIME_DIR/$title.log" 2>&1 &
  for _ in $(seq 80); do
    [[ -n "$(window_of "$title")" ]] && return 0
    sleep 0.025
  done
  echo "window '$title' never appeared: $(cat "$ANDROMEDA_RUNTIME_DIR/$title.log")"
  return 1
}

spawn tab-red 0xFFFF0000
spawn tab-blue 0xFF0000FF
"$ANDROMEDA" msg window-consume-left > /dev/null
"$ANDROMEDA" msg column-toggle-tabbed > /dev/null
"$ANDROMEDA" settle
shown=$(focused_title)
case $shown in
  tab-red) hidden=tab-blue shown_color='r > 0.9 && g < 0.1 && b < 0.1' ;;
  tab-blue) hidden=tab-red shown_color='b > 0.9 && r < 0.1 && g < 0.1' ;;
  *)
    echo "no tab of the group holds focus (focused: '$shown')"
    exit 1
    ;;
esac

"$ANDROMEDA" msg overview-open > /dev/null
"$ANDROMEDA" settle
grim "$OVERVIEW"
read -r slot_x slot_y slot_w slot_h < <("$ANDROMEDA_PIXEL_PROBE" "$OVERVIEW" blob "$MAGENTA")
read -r card_x _ card_w _ < <("$ANDROMEDA_PIXEL_PROBE" "$OVERVIEW" bbox "$shown_color")
if ((slot_w == 0 || card_w == 0)); then
  echo "the tab group's overview card has no tab bar (slot ${slot_w}px, card ${card_w}px)"
  exit 1
fi
# Two tabs split the bar across the card: the hidden tab's slot is the half the shown slot does not cover.
y=$((slot_y + slot_h / 2))
if ((slot_x + slot_w / 2 < card_x + card_w / 2)); then
  x=$((card_x + card_w * 3 / 4))
else
  x=$((card_x + card_w / 4))
fi

pointer move "$x" "$y" press "$BTN_LEFT" move "$x" $((y + 200)) release "$BTN_LEFT"
"$ANDROMEDA" settle
if [[ $(focused_title) != "$shown" ]]; then
  echo "sliding off the hidden tab's slot before releasing still switched tabs"
  exit 1
fi

pointer move "$x" "$y" click "$BTN_LEFT"
"$ANDROMEDA" settle
if [[ $(focused_title) != "$hidden" ]]; then
  echo "a click on $hidden's slot at ${x},${y} left focus on '$(focused_title)'"
  exit 1
fi

echo "a click on a hidden tab's slot in the overview opened that tab"
