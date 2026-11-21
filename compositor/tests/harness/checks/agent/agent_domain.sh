#!/usr/bin/env bash
# A client on an agent session socket sees exactly its session's seat and no privileged globals; a user client still
# sees only seat0. A stopped session takes no new clients.
set -euo pipefail
source "$ANDROMEDA_HARNESS_LIB"

readonly GLOBAL_CLIENT="$ANDROMEDA_GLOBAL_CLIENT"

started=$("$ANDROMEDA" agent-session-start --json)
id=$(jq -r '.id' <<< "$started")
socket=$(jq -r '.socket' <<< "$started")
[[ -S $socket ]] || { echo "agent socket missing: $started"; exit 1; }

user_seats=$("$GLOBAL_CLIENT" seats)
[[ $user_seats == seat0 ]] || { echo "user client seats: '$user_seats', expected seat0"; exit 1; }

agent_seats=$(WAYLAND_DISPLAY=$socket "$GLOBAL_CLIENT" seats)
[[ $agent_seats == "agent-$id" ]] || { echo "agent client seats: '$agent_seats', expected agent-$id"; exit 1; }

for global in zwlr_virtual_pointer_manager_v1 zwp_virtual_keyboard_manager_v1 zwlr_screencopy_manager_v1 \
  zwlr_layer_shell_v1 zwlr_foreign_toplevel_manager_v1; do
  WAYLAND_DISPLAY=$socket "$GLOBAL_CLIENT" "$global" absent
done
WAYLAND_DISPLAY=$socket "$GLOBAL_CLIENT" xdg_wm_base present
"$GLOBAL_CLIENT" zwlr_virtual_pointer_manager_v1 present

"$ANDROMEDA" agent-session-stop "$id" > /dev/null
[[ ! -e $socket ]] || { echo "stopped session left its socket"; exit 1; }
"$ANDROMEDA" agent-sessions --json | jq -e ".[] | select(.id == $id) | .state == \"ended\"" > /dev/null
echo "agent clients see only their session seat and no privileged globals"
