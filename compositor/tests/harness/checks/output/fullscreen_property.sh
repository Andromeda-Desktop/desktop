#!/usr/bin/env bash
# dsk_output_manager_v1 reports "fullscreen" = 1 only while the output's active workspace shows a fullscreen window,
# not when that window sits on a hidden workspace; "fullscreen_app" is that window's app id, else empty.
set -euo pipefail
source "$ANDROMEDA_HARNESS_LIB"

readonly DESKTOP=${ANDROMEDA_DESKTOP_CLIENT:-./build-debug/tests/desktop-client}
readonly CLIENT="${ANDROMEDA_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly LOG="$ANDROMEDA_RUNTIME_DIR/fullscreen-property.log"
msg() { "$ANDROMEDA" msg "$@" > /dev/null; }

output=$("$ANDROMEDA" workspaces --json | jq -r '.[0].output')
expect_property() {
  local got=
  for _ in $(seq 40); do
    got=$("$DESKTOP" properties-state | grep "^$output $1=" | cut -d= -f2- || true)
    [[ $got == "$2" ]] && return 0
    sleep 0.1
  done
  echo "$3: expected $1='$2' on $output, got '$got'"
  "$DESKTOP" properties-state | sed 's/^/  | /'
  return 1
}
expect_fullscreen() {
  expect_property fullscreen "$1" "$2"
  expect_property fullscreen_app "${1:+$app}" "$2"
}

expect_fullscreen "" "an empty output"
APP_ID=fullscreen-property-app "$CLIENT" fullscreen-property 480 300 > "$LOG" 2>&1 &
await_lines "$LOG" 'mapped$' 1
window=$("$ANDROMEDA" windows --json | jq -r '.[] | select(.title == "fullscreen-property") | .id')
app=fullscreen-property-app
expect_fullscreen "" "a window that is not fullscreen"

msg "window-focus:$window"
msg window-toggle-fullscreen
expect_fullscreen 1 "a fullscreen window on the active workspace"

msg workspace-switch:2
expect_fullscreen "" "a fullscreen window on a hidden workspace"
msg workspace-switch:1
expect_fullscreen 1 "the fullscreen window's workspace shown again"
