#!/usr/bin/env bash
# harness: render_thread
# A GPU submit stuck for 6 s (another process holding the kernel buffer locks) stops only the render thread: the
# main loop answers IPC within 100 ms throughout, and the output draws again once the submit returns.
set -euo pipefail

"$ANDROMEDA" stall-submit 6000 > /dev/null
start=$(date +%s%N)
slowest=0
while (( $(date +%s%N) - start < 5000000000 )); do
  t0=$(date +%s%N)
  "$ANDROMEDA" outputs --json > /dev/null
  ms=$(( ($(date +%s%N) - t0) / 1000000 ))
  (( ms > slowest )) && slowest=$ms
  sleep 0.2
done
(( slowest < 100 )) || { echo "IPC took $slowest ms during the stalled submit, want < 100"; exit 1; }
grep -q 'GL call while the render thread holds the context' "$ANDROMEDA_LOG" \
  && { echo "a main-thread GL call ran while the context was lent: $(grep -m1 'GL call while' "$ANDROMEDA_LOG")"; exit 1; }

timeout 10 "$ANDROMEDA" settle > /dev/null || { echo "the output did not draw after the stalled submit returned"; exit 1; }
echo "a 6 s stalled submit leaves IPC under 100 ms (slowest $slowest ms) and frames resume after it"
