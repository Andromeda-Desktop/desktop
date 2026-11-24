#!/usr/bin/env bash
# A pointer pushed against the screen edge sits in the layout gap, outside every window. It must still reach the
# window at that edge, so edge-triggered UI (auto-hide sidebars) opens without aiming for a thin strip.
set -euo pipefail

source "$ANDROMEDA_HARNESS_LIB"

readonly OUTPUT_W=1280
readonly OUTPUT_H=720
readonly POINTER="${ANDROMEDA_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
readonly CLIENT="${ANDROMEDA_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly LAYER_CLIENT="${ANDROMEDA_LAYER_CLIENT:-./build-debug/tests/layer-client}"

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
gap = 8

[layout.scrolling]
default_extent_fraction = 0.5

[animation]
duration_ms = 1
curve = "linear"

[input.focus]
follows_mouse = true
EOF
"$ANDROMEDA" msg config-reload > /dev/null

# A wallpaper covers the gap, as on a real desktop; the gap must still reach the window.
"$LAYER_CLIENT" HEADLESS-1 0 > /dev/null 2>&1 &
for _ in $(seq 60); do
  [[ $("$ANDROMEDA" layers --json | jq 'length') -ge 1 ]] && break
  sleep 0.1
done

pointer move 900 360
spawn_client "edge-left"
await_count 1
spawn_client "edge-right"
await_count 2
await_active "edge-right"

# x = 1 is inside the 8 px gap left of the left tile.
pointer move 1 360
await_active "edge-left"

echo "a pointer in the screen-edge gap reaches the window at that edge"
