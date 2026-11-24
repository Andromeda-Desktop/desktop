#!/usr/bin/env bash
# A parent request sent before the parent maps must still make the child open floating.
set -euo pipefail

source "$ANDROMEDA_HARNESS_LIB"

readonly CLIENT="${ANDROMEDA_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly CLIENT_LOG="$ANDROMEDA_RUNTIME_DIR/unmapped-transient-parent.log"

TRANSIENT_SUITE=unmapped-parent "$CLIENT" transient-child 600 500 > "$CLIENT_LOG" 2>&1 &
client_pid=$!

await_count 2

windows=$("$ANDROMEDA" windows --json)
if ! jq -e '
  length == 2
  and (.[] | select(.title == "transient-parent") | .floating) == false
  and (.[] | select(.title == "transient-child") | .floating) == true
' <<< "$windows" > /dev/null; then
  echo "an early-parented child did not open floating: $windows"
  echo "client log: $(< "$CLIENT_LOG")"
  exit 1
fi

kill "$client_pid"
wait "$client_pid" 2>/dev/null || true
await_count 0

TRANSIENT_SUITE=unmapped-parent-cleared "$CLIENT" transient-child 600 500 > "$CLIENT_LOG" 2>&1 &
await_count 2

windows=$("$ANDROMEDA" windows --json)
if ! jq -e '
  length == 2
  and all(.[]; .floating == false)
' <<< "$windows" > /dev/null; then
  echo "clearing an early parent request did not leave the child tiled: $windows"
  echo "client log: $(< "$CLIENT_LOG")"
  exit 1
fi

echo "early parent requests float children and explicit cancellation keeps them tiled"
