#!/usr/bin/env bash
# harness: outputs=1
# A default_scratchpad window rule stores a matching window without showing or
# focusing it, preserves its opening floating position, and lets the ordinary
# scratchpad action summon it afterwards. With default_focused = true the rule
# summons the scratchpad and focuses the window instead, also when a late title
# selects the rule after map. Toggling an empty scratchpad runs its
# spawn_when_empty command and shows the arriving window, unless a second toggle
# hid the pending launch first.
set -euo pipefail

source "$ANDROMEDA_HARNESS_LIB"

readonly CLIENT="${ANDROMEDA_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly CLIENT_APP_ID=scratchpad-terminal
readonly TITLE=default-scratchpad-rule

windows() { "$ANDROMEDA" windows --json; }

wait_for_state() {
  local active=$1 state=
  for _ in $(seq 80); do
    state=$(windows)
    if jq -e --arg app_id "$CLIENT_APP_ID" --argjson active "$active" '
      any(.[];
        .app_id == $app_id
        and .scratchpad == "term"
        and .workspace == ""
        and .floating == true
        and .active == $active
        and .focused == false)
    ' <<< "$state" > /dev/null; then
      return 0
    fi
    sleep 0.1
  done
  echo "expected '$CLIENT_APP_ID' in the term scratchpad with active=$active: $state"
  return 1
}

wait_for_geometry() {
  local x=$1 y=$2 width=$3 height=$4 state=
  for _ in $(seq 80); do
    state=$(windows)
    if jq -e \
      --arg app_id "$CLIENT_APP_ID" \
      --argjson x "$x" \
      --argjson y "$y" \
      --argjson width "$width" \
      --argjson height "$height" '
      any(.[];
        .app_id == $app_id
        and .x == $x
        and .y == $y
        and .w == $width
        and .h == $height)
    ' <<< "$state" > /dev/null; then
      return 0
    fi
    sleep 0.1
  done
  echo "expected '$CLIENT_APP_ID' at ${width}x${height}+${x}+${y}: $state"
  return 1
}

scratchpad_state_is() {
  windows | jq -e --arg title "$1" --arg scratchpad "$2" --argjson active "$3" '
    any(.[]; .title == $title and .scratchpad == $scratchpad and .active == $active)
  ' > /dev/null
}

expect_scratchpad_state() {
  await_until "$AWAIT_TRIES" "$AWAIT_STEP" scratchpad_state_is "$1" "$2" "$3" && return 0
  echo "expected '$1' in scratchpad '$2' with active=$3: $(windows)"
  return 1
}

cat >> "$ANDROMEDA_CONFIG" <<EOF

[animation]
enabled = false

[[scratchpad]]
name = "term"

[[scratchpad]]
name = "spawned"
spawn_when_empty = "sh -c '(\"$CLIENT\" scratchpad-spawned 480 300 &)' > '$ANDROMEDA_RUNTIME_DIR/scratchpad-spawned.log' 2>&1"

[[scratchpad]]
name = "late"
spawn_when_empty = "while [ ! -e '$ANDROMEDA_RUNTIME_DIR/late-release' ]; do sleep 0.05; done; exec '$CLIENT' scratchpad-late 480 300 > '$ANDROMEDA_RUNTIME_DIR/scratchpad-late.log' 2>&1"

[[scratchpad]]
name = "summoned"

[[scratchpad]]
name = "retitled"

[[window_rule]]
match.app_id = "^scratchpad-terminal$"
default_scratchpad = "term"
default_floating = true
default_floating_size = { width = 0.6, height = 0.5 }
default_position = { x = 0, y = 8, anchor = "top" }

[[window_rule]]
match.title = "^scratchpad-spawned$"
default_scratchpad = "spawned"

[[window_rule]]
match.title = "^scratchpad-late$"
default_scratchpad = "late"

[[window_rule]]
match.title = "^scratchpad-summoned$"
default_scratchpad = "summoned"
default_focused = true

[[window_rule]]
match.title = "^scratchpad-retitled$"
default_scratchpad = "retitled"
default_focused = true
EOF
"$ANDROMEDA" msg config-reload > /dev/null

APP_ID="$CLIENT_APP_ID" "$CLIENT" "$TITLE" 480 300 > "$ANDROMEDA_RUNTIME_DIR/$TITLE.log" 2>&1 &
wait_for_state false
wait_for_geometry 400 8 480 300

"$ANDROMEDA" msg scratchpad-toggle:term > /dev/null
wait_for_state true
wait_for_geometry 400 8 480 300

"$ANDROMEDA" msg scratchpad-toggle:spawned > /dev/null
expect_scratchpad_state scratchpad-spawned spawned true

"$ANDROMEDA" msg scratchpad-toggle:spawned > /dev/null
expect_scratchpad_state scratchpad-spawned spawned false

"$ANDROMEDA" msg scratchpad-toggle:spawned > /dev/null
expect_scratchpad_state scratchpad-spawned spawned true
if [[ $(windows | jq '[.[] | select(.title == "scratchpad-spawned")] | length') != 1 ]]; then
  echo "toggling a populated scratchpad ran spawn_when_empty again: $(windows)"
  exit 1
fi

"$ANDROMEDA" msg scratchpad-toggle:late > /dev/null
"$ANDROMEDA" msg scratchpad-toggle:late > /dev/null
touch "$ANDROMEDA_RUNTIME_DIR/late-release"
expect_scratchpad_state scratchpad-late late false

"$ANDROMEDA" msg scratchpad-toggle:late > /dev/null
expect_scratchpad_state scratchpad-late late true

"$CLIENT" scratchpad-summoned 480 300 > "$ANDROMEDA_RUNTIME_DIR/scratchpad-summoned.log" 2>&1 &
expect_scratchpad_state scratchpad-summoned summoned true

readonly RETITLE_FIFO="$ANDROMEDA_RUNTIME_DIR/retitle.fifo"
mkfifo "$RETITLE_FIFO"
exec {retitle_fd}<>"$RETITLE_FIFO"
TITLE_AFTER_MAP=scratchpad-retitled "$CLIENT" scratchpad-placeholder 480 300 <&"$retitle_fd" \
  > "$ANDROMEDA_RUNTIME_DIR/scratchpad-retitled.log" 2>&1 &
expect_scratchpad_state scratchpad-placeholder "" true
printf 'u' >&"$retitle_fd"
expect_scratchpad_state scratchpad-retitled retitled true

echo "default scratchpad rule and spawn_when_empty verified"

