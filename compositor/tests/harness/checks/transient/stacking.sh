#!/usr/bin/env bash
# A parented XDG toplevel stays above a floating, pinned, or fullscreen parent.
set -euo pipefail

source "$ANDROMEDA_HARNESS_LIB"

readonly CLIENT="${ANDROMEDA_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly POINTER="${ANDROMEDA_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
readonly CLIENT_LOG="$ANDROMEDA_RUNTIME_DIR/transient-stacking.log"

windows() {
  "$ANDROMEDA" windows --json
}

field_of() {
  local id=$1 field=$2
  windows | jq -r --arg id "$id" --arg field "$field" '.[] | select(.id == $id) | .[$field]'
}

field_is() { [[ $(field_of "$1" "$2") == "$3" ]]; }

expect_field() {
  await_until "$AWAIT_TRIES" "$AWAIT_STEP" field_is "$1" "$2" "$3" && return 0
  echo "expected $1 $2=$3, got: $(windows)"
  return 1
}

wait_for_fullscreen() {
  local id=$1 want=$2
  for _ in $(seq 60); do
    if [[ $("$ANDROMEDA" tearing --json \
      | jq -r --arg id "$id" '.surfaces[] | select(.id == $id) | .fullscreen') == "$want" ]]; then
      return 0
    fi
    sleep 0.1
  done
  echo "expected $id fullscreen=$want, got: $("$ANDROMEDA" tearing --json)"
  return 1
}

focus_at() {
  local x=$1 y=$2 id=$3 context=$4
  "$POINTER" 1280 720 move "$x" "$y"
  if ! expect_field "$id" focused true; then
    echo "$context: $(windows)"
    exit 1
  fi
}

cat >> "$ANDROMEDA_CONFIG" <<'EOF'

[animation]
enabled = false

[input.focus]
follows_mouse = true

[[window_rule]]
match.title = "^transient-parent$"
default_floating = true
default_position = { x = 100, y = 110, anchor = "top_left" }

[[window_rule]]
match.title = "^transient-child$"
default_floating = true
default_position = { x = 500, y = 110, anchor = "top_left" }
EOF
"$ANDROMEDA" msg config-reload > /dev/null

TRANSIENT_SUITE=1 "$CLIENT" transient-child 600 500 > "$CLIENT_LOG" 2>&1 &
await_count 3

snapshot=$(windows)
parent_id=$(jq -r '.[] | select(.title == "transient-parent") | .id' <<< "$snapshot")
child_id=$(jq -r '.[] | select(.title == "transient-child") | .id' <<< "$snapshot")
if [[ -z $parent_id || -z $child_id ]]; then
  echo "could not resolve transient-suite windows: $snapshot"
  exit 1
fi
expect_field "$parent_id" x 100
expect_field "$parent_id" y 110
expect_field "$child_id" x 500
expect_field "$child_id" y 110

# The first point is only inside the parent. The second is inside both windows.
# Every assertion observes a focus transition caused by real scene hit-testing.
focus_at 200 360 "$parent_id" "could not focus the exposed parent"
expect_field "$child_id" focused false
focus_at 600 360 "$child_id" "focusing the floating parent buried its transient"

focus_at 200 360 "$parent_id" "could not refocus the parent before pinning"
"$ANDROMEDA" msg window-toggle-pinned > /dev/null
expect_field "$child_id" focused false
focus_at 600 360 "$child_id" "the pinned parent covered its transient"

focus_at 200 360 "$parent_id" "could not refocus the pinned parent"
"$ANDROMEDA" msg window-toggle-pinned > /dev/null
"$ANDROMEDA" msg window-toggle-fullscreen > /dev/null
wait_for_fullscreen "$parent_id" true
wait_for_fullscreen "$child_id" false
expect_field "$child_id" focused false
focus_at 600 360 "$child_id" "the fullscreen parent covered its transient"

echo "transient dialogs stay above floating, pinned, and fullscreen parents"
