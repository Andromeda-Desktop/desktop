#!/usr/bin/env bash
# Returning to a workspace replaces the scene under a stationary pointer. With follow-mouse on, the window under that
# pointer must take focus without a pointer motion.
set -euo pipefail

source "$ANDROMEDA_HARNESS_LIB"

readonly OUTPUT_W=1280
readonly OUTPUT_H=720
readonly POINTER="${ANDROMEDA_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
readonly CLIENT="${ANDROMEDA_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"

pointer() {
  "$POINTER" "$OUTPUT_W" "$OUTPUT_H" "$@"
}

spawn_client() {
  "$CLIENT" "$1" 1200 700 > /dev/null 2>&1 &
}

active_title() {
  "$ANDROMEDA" windows --json | jq -r '[.[] | select(.active) | .title] | if length == 1 then .[0] else "none" end'
}

cat >> "$ANDROMEDA_CONFIG" <<'EOF'

[layout]
mode = "scrolling"

[layout.scrolling]
default_extent_fraction = 0.5

[animation]
duration_ms = 1
curve = "linear"

[input.focus]
follows_mouse = true
EOF
"$ANDROMEDA" msg config-reload > /dev/null

spawn_client "workspace-hover-left"
await_count 1
spawn_client "workspace-hover-right"
await_count 2
await_active "workspace-hover-right"
sleep 0.1

# Clear pointer focus on an empty workspace, then return without moving the pointer. Workspace focus history restores
# the right window, but the pointer rests over the left one: follow-mouse focus must move there with no motion.
"$ANDROMEDA" msg workspace-switch:2 > /dev/null
pointer move 64 64
"$ANDROMEDA" msg workspace-switch:1 > /dev/null
await_active "workspace-hover-left"

echo "the window under a resting pointer takes focus after a workspace switch"
