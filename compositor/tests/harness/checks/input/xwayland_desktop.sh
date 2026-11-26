#!/usr/bin/env bash
# harness: xwayland=native
# harness: outputs=2
# Steam-style XTEST input reaches native windows through EIS, including scaled logical outputs, relative motion,
# pointer clicks, ordinary keyboard input, and compositor shortcuts. A second X11 client asks for consent again.
set -euo pipefail
source "$ANDROMEDA_REPO/tests/harness/eis_lib.sh"

readonly FIRST_TITLE=eis-desktop-first
readonly SECOND_TITLE=eis-desktop-second
readonly FIRST_LOG="$ANDROMEDA_RUNTIME_DIR/eis-desktop-first.log"
readonly SECOND_LOG="$ANDROMEDA_RUNTIME_DIR/eis-desktop-second.log"
readonly APP=andromeda-xtest-desktop

cat >> "$ANDROMEDA_CONFIG" <<'CONFIG'

[animation]
enabled = false

[layout.scrolling]
default_extent_fraction = 1.0

[input.focus]
follows_mouse = false

[output.HEADLESS-1]
position = [-640, 0]
scale = 2.0

[output.HEADLESS-2]
position = [0, 0]
scale = 1.0

[keybinds]
"Ctrl+F8" = "window-toggle-floating"

[[window_rule]]
match.title = "^eis-desktop-first$"
default_output = "HEADLESS-1"

[[window_rule]]
match.title = "^eis-desktop-second$"
default_output = "HEADLESS-2"
CONFIG
"$ANDROMEDA" msg config-reload > /dev/null
"$NATIVE_CLIENT" "$FIRST_TITLE" > "$FIRST_LOG" 2>&1 &
eis_wait_window "$FIRST_TITLE"
"$NATIVE_CLIENT" "$SECOND_TITLE" > "$SECOND_LOG" 2>&1 &
eis_wait_window "$SECOND_TITLE"
eis_wait_active "$SECOND_TITLE"
"$ANDROMEDA" settle > /dev/null

eis_start first "$APP"
eis_approve first "$APP"
eis_send first 'tap 31'
await_events "$SECOND_LOG" 'keyboard-key code=31 state=pressed' 1 "the native window after EIS consent"

# Xwayland publishes two physical 1280 by 720 monitors. Their center points must reach logical layout positions
# (-320, 180) on the scaled left output and (640, 360) on the unscaled right output. Native client coordinates expose
# any confusion between X11 pixels, logical layout positions, and the translated X11 origin.
for title in "$FIRST_TITLE" "$SECOND_TITLE"; do
  window=$(eis_window "$title")
  log=$FIRST_LOG
  x=640 logical_x=-320 logical_y=180 relative_x=34 relative_y=26
  if [[ $title == "$SECOND_TITLE" ]]; then
    log=$SECOND_LOG
    x=1920 logical_x=640 logical_y=360 relative_x=17 relative_y=13
  fi
  local_x=$((logical_x - $(jq -r '.x | round' <<< "$window")))
  local_y=$((logical_y - $(jq -r '.y | round' <<< "$window")))
  presses=$(events "$log" 'pointer-button code=272 state=pressed')
  eis_send first "move $x 360" 'click 1'
  await_events "$log" 'pointer-button code=272 state=pressed' "$((presses + 1))" "$title absolute click"
  await_events "$log" "press-position x=$local_x y=$local_y" 1 "$title logical click coordinates"
  eis_wait_active "$title"
  eis_send first "relative $relative_x $relative_y" 'click 1'
  await_events "$log" "press-position x=$((local_x + 17)) y=$((local_y + 13))" 1 "$title relative motion"
done

eis_send first 'press 29' 'tap 66' 'release 29'
for _ in $(seq 60); do
  [[ $(eis_window "$SECOND_TITLE" | jq -r .floating) == true ]] && break
  sleep 0.05
done
if [[ $(eis_window "$SECOND_TITLE" | jq -r .floating) != true ]]; then
  echo "emulated Ctrl+F8 did not run the compositor shortcut: $("$ANDROMEDA" windows --json)"
  exit 1
fi
eis_send first 'tap 32'
await_events "$SECOND_LOG" 'keyboard-key code=32 state=pressed' 1 "normal typing after the compositor shortcut"

# Reconnect with the same X11 application name. Consent is per X11 client: Xwayland would hand the approved EI
# connection to any later client with that name, so it ends with its client and the new one asks again.
eis_stop first
eis_start second "$APP"
eis_approve second "$APP" 2
eis_send second 'tap 33'
await_events "$SECOND_LOG" 'keyboard-key code=33 state=pressed' 1 "typing after a consented reconnect"
if [[ $(events "$ANDROMEDA_LOG" ".*input permission requested .*app='$APP'") -ne 2 ]]; then
  echo "each X11 client must ask exactly once: $(cat "$ANDROMEDA_LOG")"
  exit 1
fi
eis_stop second

echo "XTEST controls native windows across scaled outputs, runs shortcuts, and asks each X11 client for consent"
