#!/usr/bin/env bash
# harness: outputs=2
# Workspace move bindings remove the focused scratchpad member, restore its
# saved layout state, and obey follow/silent behavior without moving its peers.
set -euo pipefail

source "$ANDROMEDA_HARNESS_LIB"

readonly CLIENT="${ANDROMEDA_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"

cat >> "$ANDROMEDA_CONFIG" <<'EOF'

[animation]
enabled = false

[input.focus]
follows_mouse = false

[input.cursor]
follows_focus = false

[output.HEADLESS-1]
workspaces = ["ONE", "TWO", "THREE"]

[output.HEADLESS-2]
workspaces = ["RIGHT"]
EOF
"$ANDROMEDA" msg config-reload > /dev/null

field() {
  "$ANDROMEDA" windows --json | jq -r --arg title "$1" --arg key "$2" '.[] | select(.title == $title) | .[$key]'
}

workspace_id() {
  "$ANDROMEDA" workspaces --json | jq -r --arg name "$1" '.[] | select(.name == $name) | .id'
}

accepts() { "$ANDROMEDA" msg "$1" > /dev/null; }

one=$(workspace_id ONE)
two=$(workspace_id TWO)
three=$(workspace_id THREE)
right=$(workspace_id RIGHT)

accepts workspace-switch:ONE/HEADLESS-1
"$CLIENT" scratch-move-anchor > "$ANDROMEDA_RUNTIME_DIR/anchor.log" 2>&1 &
await_field scratch-move-anchor active true
"$CLIENT" scratch-move-member > "$ANDROMEDA_RUNTIME_DIR/member.log" 2>&1 &
await_field scratch-move-member active true
member=$(field scratch-move-member id)

prepare() {
  accepts "window-focus:$member"
  accepts window-move-to-workspace:ONE/HEADLESS-1
  if [[ $(field scratch-move-member floating) != "$1" ]]; then
    accepts window-toggle-floating
  fi
  accepts window-move-to-scratchpad
  await_field scratch-move-member scratchpad default
  await_field scratch-move-member workspace ""
  accepts "window-focus:$member"
  await_field scratch-move-member active true
}

check_move() {
  local action=$1 destination=$2 floating=$3 follow=$4
  prepare "$floating"
  accepts "$action"
  await_field scratch-move-member workspace "$destination"
  await_field scratch-move-member scratchpad ""
  await_field scratch-move-member floating "$floating"
  await_field scratch-move-anchor workspace "$one"
  await_field scratch-move-member active "$follow"
  if [[ $follow == false ]]; then
    await_field scratch-move-anchor active true
    [[ $("$ANDROMEDA" workspaces --json | jq -r '.[] | select(.output == "HEADLESS-1" and .active) | .id') == "$one" ]]
  fi
}

for floating in false true; do
  check_move window-move-to-workspace:2 "$two" "$floating" true
  check_move window-move-to-workspace:RIGHT/HEADLESS-2 "$right" "$floating" true
  check_move window-move-to-workspace-silent:TWO "$two" "$floating" false
  check_move window-move-to-workspace-silent:RIGHT/HEADLESS-2 "$right" "$floating" false
  check_move window-move-to-workspace-next "$two" "$floating" true
  check_move window-move-to-workspace-silent-next "$two" "$floating" false
  # Adjacent moves use the pad's output even when the pointer selects another.
  prepare "$floating"
  accepts workspace-switch:RIGHT/HEADLESS-2
  accepts "window-focus:$member"
  accepts window-move-to-workspace-next
  await_field scratch-move-member workspace "$two"
  await_field scratch-move-member scratchpad ""
  await_field scratch-move-member active true

  prepare "$floating"
  accepts workspace-switch:THREE/HEADLESS-1
  accepts window-move-to-workspace-previous
  await_field scratch-move-member workspace "$two"
  await_field scratch-move-member scratchpad ""
  await_field scratch-move-member floating "$floating"
  await_field scratch-move-member active true

  prepare "$floating"
  accepts workspace-switch:THREE/HEADLESS-1
  accepts window-move-to-workspace-silent-previous
  await_field scratch-move-member workspace "$two"
  await_field scratch-move-member scratchpad ""
  await_field scratch-move-member floating "$floating"
  await_field scratch-move-member active false
  [[ $("$ANDROMEDA" workspaces --json | jq -r '.[] | select(.output == "HEADLESS-1" and .active) | .id') == "$three" ]]
done

# A rejected selector must leave scratchpad membership intact.
prepare false
if "$ANDROMEDA" msg window-move-to-workspace:MISSING > "$ANDROMEDA_RUNTIME_DIR/rejected.log" 2>&1; then
  echo "an unknown workspace was accepted"
  exit 1
fi
await_field scratch-move-member scratchpad default
await_field scratch-move-member workspace ""
await_field scratch-move-member active true

# Returning to the currently active workspace still removes membership. A peer
# stays in the pad, and later toggles cannot pull the moved member back in.
"$CLIENT" scratch-move-peer > "$ANDROMEDA_RUNTIME_DIR/peer.log" 2>&1 &
await_field scratch-move-peer active true
accepts window-move-to-scratchpad
await_field scratch-move-peer scratchpad default
accepts "window-focus:$member"
accepts window-move-to-workspace-silent:TWO
await_field scratch-move-member workspace "$two"
await_field scratch-move-member scratchpad ""
await_field scratch-move-member active false
await_field scratch-move-peer scratchpad default
await_field scratch-move-peer active true
prepare false
accepts window-move-to-workspace:ONE
await_field scratch-move-member workspace "$one"
await_field scratch-move-member scratchpad ""
await_field scratch-move-member active true
await_field scratch-move-peer scratchpad default
accepts scratchpad-toggle
accepts scratchpad-toggle
await_field scratch-move-member workspace "$one"
await_field scratch-move-peer scratchpad default

echo "workspace moves release scratchpad membership and preserve follow/silent behavior and saved layout state"
