#!/usr/bin/env bash
# harness: outputs=2
# The magnifier pass and a mirror output's frame queue their submits behind the scene's, like every frame: while a
# submit is stuck for 3 s (another process holding the kernel buffer locks), a zoomed source output that gets new
# damage and the output mirroring it never make the main loop wait, so IPC answers within 250 ms throughout.
set -euo pipefail
readonly CLIENT="${ANDROMEDA_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"

printf '\n[animation]\nduration_ms = 1\ncurve = "linear"\n\n[output.HEADLESS-2]\nmirror = "HEADLESS-1"\n' >> "$ANDROMEDA_CONFIG"
"$ANDROMEDA" msg config-reload > /dev/null
for _ in $(seq 40); do
  grep -q "output 'HEADLESS-2': mirroring 'HEADLESS-1'" "$ANDROMEDA_LOG" && break
  sleep 0.1
done
grep -q "output 'HEADLESS-2': mirroring 'HEADLESS-1'" "$ANDROMEDA_LOG" || { echo "HEADLESS-2 did not start mirroring"; exit 1; }
for _ in 1 2 3; do "$ANDROMEDA" msg zoom-in > /dev/null; done
"$ANDROMEDA" settle > /dev/null
factor=$("$ANDROMEDA" zoom --json | jq -r 'map(.factor) | max')
awk -v f="$factor" 'BEGIN {exit !(f > 1)}' || { echo "zoom-in gave factor $factor"; exit 1; }

"$ANDROMEDA" stall-submit 3000 > /dev/null
FILL_COLOR=0xFFFF0000 "$CLIENT" zoom-mirror-stall 400 240 > "$ANDROMEDA_RUNTIME_DIR/client.log" 2>&1 &
start=$(date +%s%N)
slowest=0
while (( $(date +%s%N) - start < 3500000000 )); do
  t0=$(date +%s%N)
  "$ANDROMEDA" windows --json > /dev/null
  ms=$(( ($(date +%s%N) - t0) / 1000000 ))
  (( ms > slowest )) && slowest=$ms
  sleep 0.1
done
(( slowest < 250 )) || { echo "IPC took $slowest ms during the stalled submit with zoom and a mirror, want < 250"; exit 1; }
grep -q 'GL call while the render thread holds the context' "$ANDROMEDA_LOG" \
  && { echo "a main-thread GL call ran while the context was lent: $(grep -m1 'GL call while' "$ANDROMEDA_LOG")"; exit 1; }
timeout 10 "$ANDROMEDA" settle > /dev/null || { echo "the outputs did not draw after the stalled submit returned"; exit 1; }
echo "a 3 s stalled submit with a zoomed source and a mirror leaves IPC under 250 ms (slowest $slowest ms)"
