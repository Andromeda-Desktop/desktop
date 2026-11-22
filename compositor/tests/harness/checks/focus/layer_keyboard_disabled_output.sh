#!/usr/bin/env bash
# harness: outputs=2
# An exclusive-keyboard layer surface holds the keyboard while its output is on the desktop. When that output is
# disabled (still connected), the layer is closed, so a window can take the keyboard again.
set -euo pipefail
source "$ANDROMEDA_HARNESS_LIB"

readonly POINTER="${ANDROMEDA_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
readonly OBSERVER="${ANDROMEDA_SEAT_LOG_CLIENT:-./build-debug/tests/seat-log-client}"
readonly LAYER_CLIENT="${ANDROMEDA_LAYER_CLIENT:-./build-debug/tests/layer-client}"
readonly WINDOW_LOG="$ANDROMEDA_RUNTIME_DIR/layer-disabled-window.log"
readonly PANEL_LOG="$ANDROMEDA_RUNTIME_DIR/layer-disabled-panel.log"
readonly KEYBOARD_LOG="$ANDROMEDA_RUNTIME_DIR/layer-disabled-keyboard.log"
BASELINE=$(< "$ANDROMEDA_CONFIG")

"$POINTER" 1280 720 mod none pause 60000 > "$KEYBOARD_LOG" 2>&1 &

"$OBSERVER" layer-disabled-window > "$WINDOW_LOG" 2>&1 &
window_id=
for _ in $(seq 60); do
  window_id=$("$ANDROMEDA" windows --json | jq -r '.[] | select(.title == "layer-disabled-window") | .id')
  [[ -n $window_id && $window_id != null ]] && break
  sleep 0.1
done
[[ -n $window_id && $window_id != null ]] || { echo "the observer window was not registered"; exit 1; }
await_events "$WINDOW_LOG" keyboard-enter 1 "the window"

"$LAYER_CLIENT" HEADLESS-2 40 keyboard=exclusive > "$PANEL_LOG" 2>&1 &
await_events "$PANEL_LOG" keyboard-enter 1 "the panel"
await_events "$WINDOW_LOG" keyboard-leave 1 "the window"

{
  printf '%s\n' "$BASELINE"
  printf '\n[output.HEADLESS-2]\nenabled = false\n'
} > "$ANDROMEDA_CONFIG"
"$ANDROMEDA" msg config-reload > /dev/null

"$ANDROMEDA" msg "window-focus:$window_id" > /dev/null
await_events "$WINDOW_LOG" keyboard-enter 2 "the window"

echo "a disabled output's exclusive layer gives the keyboard back to windows"
