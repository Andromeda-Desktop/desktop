#!/usr/bin/env bash
# A reload that moves only looks (a theme: colours, corners, glass) keeps the overview open, and its cards take the new
# look; a ui scale change, which moves the overview's layout, still closes it.
set -euo pipefail

readonly CLIENT="${ANDROMEDA_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly SHOT="$ANDROMEDA_RUNTIME_DIR/overview-theme-reload.png"
readonly EVENTS="$ANDROMEDA_RUNTIME_DIR/overview-theme-events.log"
readonly WAIT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../../.." && pwd)/tools/wait.sh
# The shown tab's slot is the only magenta, then the only green, on screen: the windows fill red and blue.
readonly MAGENTA='r > 0.9 && g < 0.1 && b > 0.9'
readonly GREEN='r < 0.1 && g > 0.9 && b < 0.1'

if [[ ! -x $CLIENT ]]; then
  echo "unmap client is not built"
  exit 1
fi

cat >> "$ANDROMEDA_CONFIG" <<'EOF'

[animation]
enabled = false

[appearance]
corner_radius = 12

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

spawn theme-red 0xFFFF0000
spawn theme-blue 0xFF0000FF
"$ANDROMEDA" msg window-consume-left > /dev/null
"$ANDROMEDA" msg column-toggle-tabbed > /dev/null
"$ANDROMEDA" settle

"$ANDROMEDA" subscribe overview > "$EVENTS" &
"$WAIT" --file "$EVENTS" --timeout 2 > /dev/null
# Closes after the first open: the subscription starts with the current, closed state.
closes() {
  jq -s '[.[] | select(.event == "overview") | .data.open] | .[(index(true) // length):] | map(select(. == false)) | length' \
    "$EVENTS"
}

"$ANDROMEDA" msg overview-open > /dev/null
"$ANDROMEDA" settle
grim "$SHOT"
read -r _ _ magenta_w _ < <("$ANDROMEDA_PIXEL_PROBE" "$SHOT" blob "$MAGENTA")
if ((magenta_w == 0)); then
  echo "the tab group's overview card has no tab bar"
  exit 1
fi

# A theme switch: the tab colours, the window corners and the accent move together in one reload.
sed -i -e 's/#FF00FFFF/#00FF00FF/' -e 's/^corner_radius = 12$/corner_radius = 4\naccent = "#F2C879FF"/' "$ANDROMEDA_CONFIG"
"$ANDROMEDA" msg config-reload > /dev/null
"$ANDROMEDA" settle
(($(closes) == 0)) || { echo "a look-only reload closed the overview: $(< "$EVENTS")"; exit 1; }
grim "$SHOT"
read -r _ _ green_w _ < <("$ANDROMEDA_PIXEL_PROBE" "$SHOT" blob "$GREEN")
read -r _ _ magenta_w _ < <("$ANDROMEDA_PIXEL_PROBE" "$SHOT" blob "$MAGENTA")
if ((green_w == 0 || magenta_w != 0)); then
  echo "the open overview's card kept its old tab colour: green ${green_w}px, magenta ${magenta_w}px ($SHOT)"
  exit 1
fi

# The shell's way: one version 3 commit of theme values in a layer.
readonly D=${ANDROMEDA_DESKTOP_CLIENT:-./build-debug/tests/desktop-client}
mkfifo "$ANDROMEDA_RUNTIME_DIR/theme-in"
"$D" settings-commit 3 style/appearance.scheme=light style/appearance.accent=#4060F0FF \
  style/appearance.glass_style=liquid style/appearance.blur.saturation=1.4 \
  < "$ANDROMEDA_RUNTIME_DIR/theme-in" > "$ANDROMEDA_RUNTIME_DIR/theme-out" 2>&1 &
exec 3> "$ANDROMEDA_RUNTIME_DIR/theme-in"
echo commit >&3
"$WAIT" --log "$ANDROMEDA_RUNTIME_DIR/theme-out" \
  --match '^applied 3$' --fail '^failed' --timeout 4 > /dev/null || { echo "no applied 3: $(< "$ANDROMEDA_RUNTIME_DIR/theme-out")"; exit 1; }
"$ANDROMEDA" settle
(($(closes) == 0)) || { echo "a committed theme layer closed the overview: $(< "$EVENTS")"; exit 1; }

sed -i 's/^corner_radius = 4$/corner_radius = 4\nui_scale = 1.25/' "$ANDROMEDA_CONFIG"
"$ANDROMEDA" msg config-reload > /dev/null
"$ANDROMEDA" settle
(($(closes) == 1)) || { echo "a ui scale reload left the overview open: $(< "$EVENTS")"; exit 1; }

echo "a theme reload kept the overview open and recoloured its card; a ui scale reload closed it"
