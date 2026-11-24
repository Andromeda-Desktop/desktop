#!/usr/bin/env bash
# Leaving fullscreen can reveal an unfocused tiled window beneath a stationary pointer. Motion inside that tile must
# refresh hover focus without requiring a border crossing.
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

spawn_client scene-fullscreen-left
await_count 1
spawn_client scene-fullscreen-right
await_count 2
sleep 0.1

"$POINTER" "$OUTPUT_W" "$OUTPUT_H" move 100 360
await_active scene-fullscreen-left
"$ANDROMEDA" msg window-toggle-fullscreen > /dev/null
sleep 0.1

# The fullscreen window still owns this position, so the motion consumes no geometric enter into the hidden tile.
"$POINTER" "$OUTPUT_W" "$OUTPUT_H" move 1000 360
await_active scene-fullscreen-left
"$ANDROMEDA" msg window-toggle-fullscreen > /dev/null
sleep 0.1
await_active scene-fullscreen-left

"$POINTER" "$OUTPUT_W" "$OUTPUT_H" move 1001 360
await_active scene-fullscreen-right

echo "motion refreshed hover focus after fullscreen exit revealed another tile beneath the pointer"
