#!/usr/bin/env bash
# harness: outputs=2
# Master output moves transfer the focused master or stack area as one column.
set -euo pipefail

source "$ANDROMEDA_HARNESS_LIB"

readonly WORKSPACE="${ANDROMEDA_WORKSPACE_CLIENT:-./build-debug/tests/workspace-client}"

accepts() {
  if ! out=$("$ANDROMEDA" msg "$1" 2>&1); then
    echo "expected '$1' to be accepted, got: $out"
    return 1
  fi
}

spawn_client() {
  foot --title="$1" sh -c 'sleep 120' > /dev/null 2>&1 &
}

field_of() {
  "$ANDROMEDA" windows --json | jq -r --arg title "$1" --arg field "$2" \
    '.[] | select(.title == $title) | .[$field]'
}

workspace_id_named() {
  "$WORKSPACE" --all | awk -F'\t' -v name="$1" '$2 == name { print $1; exit }'
}

cat >> "$ANDROMEDA_CONFIG" <<'EOF'

[layout]
mode = "master"

[animation]
duration_ms = 1
curve = "linear"

[output.HEADLESS-1]
position = [0, 0]
workspaces = ["ONE"]

[output.HEADLESS-2]
position = [1280, 0]
workspaces = ["RIGHT_ONE"]
EOF
"$ANDROMEDA" msg config-reload > /dev/null
one_id=$(workspace_id_named ONE)
right_one_id=$(workspace_id_named RIGHT_ONE)
if [[ -z $one_id || -z $right_one_id ]]; then
  echo "expected workspace ids for both outputs"
  exit 1
fi
accepts "workspace-switch:ONE/HEADLESS-1"

spawn_client master-a
await_count 1
spawn_client stack-b
await_count 2
spawn_client stack-c
await_count 3
stack_c_id=$(field_of stack-c id)

accepts column-move-to-output-right
await_workspace master-a "$one_id"
await_workspace stack-b "$right_one_id"
await_workspace stack-c "$right_one_id"

accepts column-move-to-output-left
await_workspace master-a "$one_id"
await_workspace stack-b "$one_id"
await_workspace stack-c "$one_id"

accepts "window-focus:$stack_c_id"
accepts window-move-or-output-right
await_workspace master-a "$one_id"
await_workspace stack-b "$right_one_id"
await_workspace stack-c "$right_one_id"

echo "master column and edge output moves transferred the full focused area"
