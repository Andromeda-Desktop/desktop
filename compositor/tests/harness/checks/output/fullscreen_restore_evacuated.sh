#!/usr/bin/env bash
# harness: outputs=2
# A floating window that is fullscreen when its output goes away leaves fullscreen inside the output it moved to: its
# restore box moves with it.
set -euo pipefail

readonly WINDOW_CLIENT="${ANDROMEDA_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"

cat >> "$ANDROMEDA_CONFIG" <<'CONF'

[animation]
enabled = false

[output.HEADLESS-1]
mode = "2560x1440"
position = [0, 0]

[output.HEADLESS-2]
mode = "1920x1080"
position = [2560, 0]

[[window_rule]]
match.title = "^evacuated-fullscreen$"
default_output = "HEADLESS-2"
default_floating = true
default_floating_size_px = { width = 500, height = 300 }
default_position = { x = 1200, y = 600, anchor = "top_left" }
CONF
"$ANDROMEDA" msg config-reload > /dev/null

field() {
  "$ANDROMEDA" windows --json | jq -r --arg field "$1" '.[] | select(.title == "evacuated-fullscreen") | .[$field]'
}
wait_for() {
  local name=$1 expected=$2 actual=
  for _ in $(seq 80); do
    actual=$(field "$name")
    [[ $actual =~ $expected ]] && return 0
    sleep 0.1
  done
  echo "expected $name to match '$expected', got '$actual'"
  exit 1
}

"$WINDOW_CLIENT" evacuated-fullscreen 500 300 > "$ANDROMEDA_RUNTIME_DIR/client.log" 2>&1 &
wait_for workspace '^HEADLESS-2:'
wait_for floating '^true$'
"$ANDROMEDA" msg "window-focus-warp:$(field id)" > /dev/null
"$ANDROMEDA" msg window-toggle-fullscreen > /dev/null
wait_for fullscreen '^true$'
"$ANDROMEDA" output-destroy HEADLESS-2 > /dev/null
wait_for workspace '^HEADLESS-1:'
"$ANDROMEDA" msg "window-focus-warp:$(field id)" > /dev/null
"$ANDROMEDA" msg window-toggle-fullscreen > /dev/null
wait_for fullscreen '^false$'
timeout 10 "$ANDROMEDA" settle > /dev/null
read -r x y w h < <("$ANDROMEDA" windows --json \
  | jq -r '.[] | select(.title == "evacuated-fullscreen") | "\(.x) \(.y) \(.w) \(.h)"')
(( x >= 0 && y >= 0 && x + w <= 2560 && y + h <= 1440 )) \
  || { echo "after fullscreen ended on HEADLESS-1 (0,0 2560x1440) the window is at $x,$y ${w}x$h"; exit 1; }
