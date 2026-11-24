#!/usr/bin/env bash
# harness: outputs=2
# The config `enabled` key must disable an output live: the commit turns the monitor off, its windows move to a live
# output, and re-enabling restores it. That needs a second monitor, which the header directive above asks the harness
# for.
set -euo pipefail

source "$ANDROMEDA_HARNESS_LIB"

# The disable and re-enable phases replace the whole config rather than
# appending, because re-enabling means the key is gone again. Keeping the
# instance's pristine config in a variable avoids restating the [general] block
# that keeps Xwayland out of this instance.
BASELINE=$(< "$ANDROMEDA_CONFIG")

log_mark() { wc -l < "$ANDROMEDA_LOG"; }

spawn_client() {
  foot --title=output-disable-rehome sh -c 'sleep 120' > /dev/null 2>&1 &
}

spawn_client
await_sole_workspace 'HEADLESS-2:1'
native_refuge=$("$ANDROMEDA" workspaces --json | jq -r '.[] | select(.output == "HEADLESS-1" and .index == 1) | .id')

# Disable the output through a live reload.
mark=$(log_mark)
{
  printf '%s\n' "$BASELINE"
  printf '\n[output.HEADLESS-2]\nenabled = false\n'
} > "$ANDROMEDA_CONFIG"
"$ANDROMEDA" msg config-reload > /dev/null
if ! await_log_since "$mark" "output 'HEADLESS-2': disabled by config"; then
  echo "output was not disabled on reload"
  tail -8 "$ANDROMEDA_LOG" | sed 's/^/  | /'
  exit 1
fi
await_temporary_workspace HEADLESS-1 "$native_refuge"

# Re-enable: the compositor must survive the disable and come back with a mode.
mark=$(log_mark)
printf '%s\n' "$BASELINE" > "$ANDROMEDA_CONFIG"
"$ANDROMEDA" msg config-reload > /dev/null
if ! await_log_since "$mark" "output 'HEADLESS-2': applied mode="; then
  echo "output was not re-enabled on reload"
  tail -8 "$ANDROMEDA_LOG" | sed 's/^/  | /'
  exit 1
fi

await_sole_workspace 'HEADLESS-2:1'

echo "output disabled and re-enabled through live reload, and its window came back"
