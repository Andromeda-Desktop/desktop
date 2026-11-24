#!/usr/bin/env bash
# Shrinking the focused column can expose its neighbor beneath a stationary pointer. Motion inside that newly exposed
# column must refresh hover focus without requiring a border crossing.
set -euo pipefail

source "$ANDROMEDA_HARNESS_LIB"

readonly OUTPUT_W=1280
readonly OUTPUT_H=720
readonly POINTER="${ANDROMEDA_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
readonly CLIENT="${ANDROMEDA_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"

spawn_client() { "$CLIENT" "$1" 1200 700 > /dev/null 2>&1 & }

cat >> "$ANDROMEDA_CONFIG" <<'EOF'

[layout]
mode = "scrolling"

[layout.scrolling]
default_extent_fraction = 0.6

[animation]
duration_ms = 1
curve = "linear"

[input.focus]
follows_mouse = true
EOF
"$ANDROMEDA" msg config-reload > /dev/null

spawn_client scene-resize-left
await_count 1
spawn_client scene-resize-right
await_count 2
sleep 0.1

# Reveal the left column first, then derive the test point from the settled boundary. Reading it before the focus reveal
# would use the strip's old offset and could leave the pointer inside the left column after the resize.
"$POINTER" "$OUTPUT_W" "$OUTPUT_H" move 100 360
await_active scene-resize-left
sleep 0.1
right_x=$("$ANDROMEDA" windows --json | jq -r '.[] | select(.title == "scene-resize-right") | .x')
pointer_x=$((right_x - 50))
"$POINTER" "$OUTPUT_W" "$OUTPUT_H" move "$pointer_x" 360
await_active scene-resize-left

# A 0.2-viewport shrink moves the right column left by about 252 pixels, well past this pointer position.
"$ANDROMEDA" msg window-modify-primary-extent:-0.2 > /dev/null
sleep 0.1
await_active scene-resize-left

"$POINTER" "$OUTPUT_W" "$OUTPUT_H" move "$((pointer_x + 1))" 360
await_active scene-resize-right

echo "motion refreshed hover focus after a resize exposed another column beneath the pointer"
