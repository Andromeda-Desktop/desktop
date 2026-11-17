#!/usr/bin/env bash
# harness: outputs=2
# An output-management commit that fails rolls the outputs back on the render thread: while the rollback's GPU submit
# is stuck for 3 s, other IPC answers within 250 ms, the action replies after the rollback, and HEADLESS-2 stays on.
set -euo pipefail

"$ANDROMEDA" output-fail-next-commit 3000 > /dev/null
done_file="$ANDROMEDA_RUNTIME_DIR/disable.done"
( "$ANDROMEDA" msg output-disable:HEADLESS-2 > /dev/null 2>&1 || true
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
(( slowest < 250 )) || { echo "IPC took $slowest ms while the rollback's submit stalled, want < 250"; exit 1; }
for _ in $(seq 100); do
  [[ -s "$done_file" ]] && break
  sleep 0.1
done
[[ -s "$done_file" ]] || { echo "output-disable did not reply after the rollback"; exit 1; }
[[ $(< "$done_file") == true ]] || { echo "HEADLESS-2 is off after its failed disable was rolled back"; exit 1; }
if grep -q "GL call while the render thread holds the context\|failed to restore all outputs" "$ANDROMEDA_LOG"; then
  echo "the rollback failed: $(grep -m1 "GL call while\|failed to restore" "$ANDROMEDA_LOG")"
  exit 1
fi
timeout 10 "$ANDROMEDA" settle > /dev/null || { echo "the outputs did not settle after the rollback"; exit 1; }
echo "a failed output-disable rolls back during a 3 s stalled submit with IPC under 250 ms (slowest $slowest ms)"
