#!/usr/bin/env bash
# A client data-device drag held at the scrolling edge must move the strip far enough to reveal an offscreen target.
# Moving inward over that newly revealed client and releasing must complete a real Wayland drop.
set -euo pipefail

source "$ANDROMEDA_HARNESS_LIB"

readonly OUTPUT_W=1280
readonly OUTPUT_H=720
readonly LEFT_BUTTON=272
readonly DRAG_CLIENT="${ANDROMEDA_DRAG_CLIENT:-./build-debug/tests/drag-client}"
readonly CLIENT="${ANDROMEDA_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly SOURCE_LOG="$ANDROMEDA_RUNTIME_DIR/data-drag-edge-source.log"
readonly TARGET_LOG="$ANDROMEDA_RUNTIME_DIR/data-drag-edge-target.log"

window_count() {
  "$ANDROMEDA" windows --json | jq 'length'
}

expect_log() {
  await_log_count "$1" "$2" > /dev/null || { echo "$3 did not report '$2': $(< "$1")"; return 1; }
}

spawn_client() {
  local title=$1
  "$CLIENT" "$title" 624 700 > "$ANDROMEDA_RUNTIME_DIR/$title.log" 2>&1 &
}

cat >> "$ANDROMEDA_CONFIG" <<'EOF'

[layout.scrolling]
default_extent_fraction = 0.5
center_focused = "never"
EOF
"$ANDROMEDA" msg config-reload > /dev/null

spawn_client data-drag-edge-a
await_count 1
spawn_client data-drag-edge-b
await_count 2
spawn_client data-drag-edge-c
await_count 3
"$DRAG_CLIENT" target data-drag-edge-target > "$TARGET_LOG" 2>&1 &
TARGET_PID=$!
expect_log "$TARGET_LOG" '^ready$' "drop target"
await_count 4

"$ANDROMEDA" msg column-focus-first > /dev/null
"$ANDROMEDA" settle

windows=$("$ANDROMEDA" windows --json)
initial_first_x=$(jq -r '.[] | select(.title == "data-drag-edge-a") | .x' <<< "$windows")
initial_target_x=$(jq -r '.[] | select(.title == "data-drag-edge-target") | .x' <<< "$windows")
if ((initial_target_x < OUTPUT_W)); then
  echo "drop target began inside the viewport instead of offscreen: $windows"
  exit 1
fi

"$DRAG_CLIENT" > "$SOURCE_LOG" 2>&1 &
SOURCE_PID=$!
expect_log "$SOURCE_LOG" '^ready$' "drag source"

pointer_hold "$OUTPUT_W" "$OUTPUT_H" \
  move 32 32 press "$LEFT_BUTTON" \
  -- move 1279 360 mark edge hold move 1000 360 mark target hold release "$LEFT_BUTTON"
expect_log "$SOURCE_LOG" '^drag-started$' "drag source"

pointer_step edge
moved=false
first_x=$initial_first_x
target_x=$initial_target_x
for _ in $(seq 120); do
  windows=$("$ANDROMEDA" windows --json)
  first_x=$(jq -r '.[] | select(.title == "data-drag-edge-a") | .x' <<< "$windows")
  target_x=$(jq -r '.[] | select(.title == "data-drag-edge-target") | .x' <<< "$windows")
  if ((first_x < initial_first_x && target_x <= 700)); then
    moved=true
    break
  fi
  sleep 0.05
done
if [[ $moved != true ]]; then
  echo "data-device drag at the right edge did not reveal the offscreen target: first x $initial_first_x -> $first_x, target x $initial_target_x -> $target_x"
  exit 1
fi
expect_log "$TARGET_LOG" '^drag-enter$' "stationary edge drop target"

pointer_step target
pointer_release

expect_log "$TARGET_LOG" '^drop-received$' "drop target"
expect_log "$SOURCE_LOG" '^drag-finished$' "drag source"
wait "$TARGET_PID"
wait "$SOURCE_PID"

echo "data-device drag scrolled to an offscreen window and completed the drop"
