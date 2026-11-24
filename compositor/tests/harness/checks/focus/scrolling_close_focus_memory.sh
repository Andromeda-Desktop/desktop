#!/usr/bin/env bash
# Closing a temporary scrolling-layout column restores the most recently focused member of the nearest surviving
# column. Visual row order must not replace that focus memory with the column's top window.
set -euo pipefail

source "$ANDROMEDA_HARNESS_LIB"

readonly CLIENT="${ANDROMEDA_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"

spawn_client() {
  local title=$1
  "$CLIENT" "$title" 1200 700 > "$ANDROMEDA_RUNTIME_DIR/$title.log" 2>&1 &
}

windows() { "$ANDROMEDA" windows --json; }

field_of() {
  windows | jq -r --arg title "$1" --arg field "$2" '.[] | select(.title == $title) | .[$field]'
}

wait_for_stack() {
  for _ in $(seq 40); do
    if windows | jq -e '
      [.[] | select(.title == "scroll-close-top")] as $top
      | [.[] | select(.title == "scroll-close-bottom")] as $bottom
      | ($top | length == 1)
        and ($bottom | length == 1)
        and ($top[0].x == $bottom[0].x)
        and ($top[0].y < $bottom[0].y)
    ' > /dev/null; then
      return 0
    fi
    sleep 0.05
  done
  echo "expected a top and bottom stack: $(windows)"
  return 1
}

cat >> "$ANDROMEDA_CONFIG" <<'EOF'

[animation]
enabled = false

[layout]
mode = "scrolling"
EOF
"$ANDROMEDA" msg config-reload > /dev/null

spawn_client scroll-close-top
await_count 1
spawn_client scroll-close-bottom
await_count 2
"$ANDROMEDA" msg window-consume-left > /dev/null
wait_for_stack

bottom_id=$(field_of scroll-close-bottom id)
"$ANDROMEDA" msg "window-focus:$bottom_id" > /dev/null
await_active scroll-close-bottom

spawn_client scroll-close-temporary
await_count 3
await_active scroll-close-temporary

temporary_id=$(field_of scroll-close-temporary id)
"$ANDROMEDA" msg "window-close:$temporary_id" > /dev/null
for _ in $(seq 40); do
  grep -q '^unmapped$' "$ANDROMEDA_RUNTIME_DIR/scroll-close-temporary.log" && break
  sleep 0.1
done
if ! grep -q '^unmapped$' "$ANDROMEDA_RUNTIME_DIR/scroll-close-temporary.log"; then
  echo "temporary window did not unmap: $(< "$ANDROMEDA_RUNTIME_DIR/scroll-close-temporary.log")"
  exit 1
fi
await_count 2
await_active scroll-close-bottom

echo "closing a temporary column restores its nearest column's most recently focused row"
