#!/usr/bin/env bash
# follows_mouse = "always" focuses the window under the pointer on every motion, even without a border crossing. A
# still pointer keeps a keyboard choice or a new window's focus, but a layout command or workspace switch that leaves
# another window beneath it focuses that window without motion. With follows_focus the pointer travels to the keyboard
# choice, so motion keeps it.
set -euo pipefail

source "$ANDROMEDA_HARNESS_LIB"

readonly POINTER="${ANDROMEDA_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
readonly CLIENT="${ANDROMEDA_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"

windows() { "$ANDROMEDA" windows --json; }
field() { windows | jq -r --arg title "$1" ".[] | select(.title == \$title) | .$2"; }
pointer() { "$POINTER" 1280 720 "$@"; }

spawn() {
  "$CLIENT" "$1" 1200 700 > /dev/null 2>&1 &
  for _ in $(seq 60); do
    [[ -n $(field "$1" id) ]] && return 0
    sleep 0.1
  done
  echo "window $1 did not map: $(windows)"
  exit 1
}

cat >> "$ANDROMEDA_CONFIG" <<'EOF'

[layout]
mode = "dwindle"

[animation]
enabled = false

[input.focus]
follows_mouse = "always"

[[window_rule]]
match.title = "^always-away$"
default_floating = true
default_floating_size_px = { width = 300, height = 200 }
default_position = { x = 900, y = 450, anchor = "top_left" }
EOF
"$ANDROMEDA" msg config-reload > /dev/null

spawn always-left
spawn always-right
"$ANDROMEDA" settle
left_x=$(($(field always-left x) + 40))
pointer move "$left_x" 300
await_active always-left 'pointer motion'

"$ANDROMEDA" msg "window-focus:$(field always-right id)" > /dev/null
"$ANDROMEDA" settle
await_active always-right 'keyboard focus while the pointer is still'
pointer move "$((left_x + 1))" 300
await_active always-left 'motion inside the window under the pointer after keyboard focus'

spawn always-away
"$ANDROMEDA" settle
await_active always-away 'a window opening away from a still pointer'
pointer move "$left_x" 300
await_active always-left 'motion inside the window under the pointer after a new window'

# A swap brings always-right under the still pointer.
"$ANDROMEDA" msg window-swap-right > /dev/null
"$ANDROMEDA" settle
await_active always-right 'a swap that moves another window under a still pointer'

# The second window maps beside the first and keeps focus, then a return to this workspace restores that remembered
# focus and hands it to the window under the pointer.
"$ANDROMEDA" msg "workspace-switch:2" > /dev/null
spawn always-ws-a
spawn always-ws-b
"$ANDROMEDA" settle
await_active always-ws-b 'a window opening beside the still pointer'
"$ANDROMEDA" msg "workspace-switch:1" > /dev/null
"$ANDROMEDA" settle
"$ANDROMEDA" msg "workspace-switch:2" > /dev/null
"$ANDROMEDA" settle
await_active always-ws-a 'a workspace switch that leaves another window under a still pointer'
"$ANDROMEDA" msg "workspace-switch:1" > /dev/null
"$ANDROMEDA" settle

"$ANDROMEDA" config set input.cursor.follows_focus true > /dev/null
"$ANDROMEDA" msg "window-focus:$(field always-right id)" > /dev/null
"$ANDROMEDA" settle
pointer relative 1 0
"$ANDROMEDA" settle
await_active always-right 'motion after follows_focus carried the pointer'

echo "always follows_mouse refocuses on every motion and leaves a still pointer's focus alone"
