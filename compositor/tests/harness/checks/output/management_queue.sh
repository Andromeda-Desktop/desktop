#!/usr/bin/env bash
# harness: outputs=2
# Output-management applies run one at a time; the others queue. Every queued
# configuration must still get succeeded, failed or cancelled, also when two
# or more wait behind the one in flight.
set -euo pipefail

readonly OUTPUT_MANAGEMENT=${ANDROMEDA_OUTPUT_MANAGEMENT_CLIENT:-./build-debug/tests/output-management-client}

read -r x y < <(
  "$ANDROMEDA" outputs --json \
    | jq -r '.[] | select(.name == "HEADLESS-2") | "\(.position.x) \(.position.y)"'
)

for round in 1 2 3; do
  pids=()
  for step in 0 1 2 3; do
    timeout 10s "$OUTPUT_MANAGEMENT" apply enable HEADLESS-2 "$((x + step * 16))" "$y" > /dev/null 2>&1 &
    pids+=($!)
  done
  for pid in "${pids[@]}"; do
    status=0
    wait "$pid" || status=$?
    if [[ $status -eq 124 ]]; then
      echo "round $round: an output-management apply got no answer in 10 s"
      exit 1
    fi
  done
done
