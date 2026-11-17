#!/usr/bin/env bash
# harness: outputs=2
# An output-management apply (output-disable) during a 3 s stalled GPU submit draws the remaining output on the render
# thread and commits after the stall: other IPC answers within 250 ms meanwhile, and the action replies only after
# HEADLESS-2 is disabled.
set -euo pipefail

"$ANDROMEDA" stall-submit 3000 > /dev/null
"$ANDROMEDA" windows --json > /dev/null
done_file="$ANDROMEDA_RUNTIME_DIR/disable.done"
( "$ANDROMEDA" msg output-disable:HEADLESS-2 > /dev/null || { echo "output-disable failed" >&2; exit 1; }
  "$ANDROMEDA" outputs --json | jq -r '.[] | select(.name == "HEADLESS-2") | .enabled' > "$done_file" ) &
slowest=0
start=$(date +%s%N)
while (( $(date +%s%N) - start < 2000000000 )); do
  t0=$(date +%s%N)
  "$ANDROMEDA" windows --json > /dev/null || { echo "windows failed during the stall"; exit 1; }
  ms=$(( ($(date +%s%N) - t0) / 1000000 ))
  (( ms > slowest )) && slowest=$ms
  sleep 0.1
done
(( slowest < 250 )) || { echo "IPC took $slowest ms while output-disable ran during the stalled submit, want < 250"; exit 1; }
for _ in $(seq 100); do
  [[ -s "$done_file" ]] && break
  sleep 0.1
done
[[ -s "$done_file" ]] || { echo "output-disable did not reply after the stalled submit returned"; exit 1; }
[[ $(< "$done_file") == false ]] || { echo "output-disable replied before HEADLESS-2 was disabled"; exit 1; }
timeout 10 "$ANDROMEDA" settle > /dev/null || { echo "the outputs did not settle after the stalled submit returned"; exit 1; }
echo "output-disable during a 3 s stalled submit leaves IPC under 250 ms (slowest $slowest ms) and replies once applied"
