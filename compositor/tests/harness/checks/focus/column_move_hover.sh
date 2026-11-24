#!/usr/bin/env bash
# Moving the focused column away can swap an unfocused column beneath a stationary pointer. Motion inside the swapped
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
default_extent_fraction = 0.5

[animation]
duration_ms = 1
curve = "linear"

[input.focus]
follows_mouse = true
EOF
"$ANDROMEDA" msg config-reload > /dev/null

spawn_client scene-move-left
await_count 1
spawn_client scene-move-right
await_count 2
sleep 0.1

left_x=$("$ANDROMEDA" windows --json | jq -r '.[] | select(.title == "scene-move-left") | .x')
pointer_x=$((left_x + 50))
"$POINTER" "$OUTPUT_W" "$OUTPUT_H" move "$pointer_x" 360
await_active scene-move-left

"$ANDROMEDA" msg column-move-right > /dev/null
sleep 0.1
await_active scene-move-left

"$POINTER" "$OUTPUT_W" "$OUTPUT_H" move "$((pointer_x + 1))" 360
await_active scene-move-right

echo "motion refreshed hover focus after a column move swapped another column beneath the pointer"
