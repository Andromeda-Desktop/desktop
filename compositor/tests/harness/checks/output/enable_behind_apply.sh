#!/usr/bin/env bash
# harness: outputs=3
# An output action queued behind an output-management apply builds its configuration when it starts, so it keeps the
# position that apply set instead of the layout from the moment it was queued.
set -euo pipefail

readonly OUTPUT_MANAGEMENT=${ANDROMEDA_OUTPUT_MANAGEMENT_CLIENT:-./build-debug/tests/output-management-client}

position() {
  "$ANDROMEDA" outputs --json | jq -r '.[] | select(.name == "HEADLESS-2") | "\(.position.x) \(.position.y)"'
}

read -r x y < <(position)
target=$((x + 512))
"$ANDROMEDA" stall-submit 3000 > /dev/null
timeout 10s "$OUTPUT_MANAGEMENT" apply enable HEADLESS-2 "$target" "$y" > /dev/null 2>&1 &
apply=$!
sleep 0.3 # real time: the apply reaches the compositor and waits on the stalled submit; no IPC reports it
timeout 10s "$ANDROMEDA" msg output-disable:HEADLESS-3 > /dev/null
wait "$apply" || { echo "the output-management apply failed"; exit 1; }
timeout 10 "$ANDROMEDA" settle > /dev/null
read -r nx ny < <(position)
[[ $nx == "$target" && $ny == "$y" ]] \
  || { echo "HEADLESS-2 at $nx,$ny after output-disable queued behind the apply to $target,$y"; exit 1; }
