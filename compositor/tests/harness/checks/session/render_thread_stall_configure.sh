#!/usr/bin/env bash
# A reload that changes an output's scale while a GPU submit is stuck for 3 s builds the configured frame once the
# render thread returns the GL context: other IPC answers within 250 ms meanwhile, and the reload replies only after
# the scale is applied.
set -euo pipefail
readonly CLIENT="${ANDROMEDA_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"

"$ANDROMEDA" stall-submit 3000 > /dev/null
FILL_COLOR=0xFFFF0000 "$CLIENT" configure-stall 400 240 > "$ANDROMEDA_RUNTIME_DIR/client.log" 2>&1 &
for _ in $(seq 40); do
  "$ANDROMEDA" windows --json | jq -e 'any(.[]; .title == "configure-stall")' > /dev/null && break
  sleep 0.05
done
printf '\n[output.HEADLESS-1]\nscale = 2\n' >> "$ANDROMEDA_CONFIG"
reload_done="$ANDROMEDA_RUNTIME_DIR/reload.done"
( "$ANDROMEDA" msg config-reload > /dev/null || { echo "config-reload failed" >&2; exit 1; }
  grep -c "output 'HEADLESS-1': applied mode=.* scale=2 " "$ANDROMEDA_LOG" > "$reload_done" || echo 0 > "$reload_done" ) &
slowest=0
start=$(date +%s%N)
while (( $(date +%s%N) - start < 2000000000 )); do
  t0=$(date +%s%N)
  "$ANDROMEDA" windows --json > /dev/null || { echo "windows failed during the stall"; exit 1; }
  ms=$(( ($(date +%s%N) - t0) / 1000000 ))
  (( ms > slowest )) && slowest=$ms
  sleep 0.1
done
(( slowest < 250 )) || { echo "IPC took $slowest ms while a reload changed the scale during the stalled submit, want < 250"; exit 1; }
for _ in $(seq 100); do
  [[ -s "$reload_done" ]] && break
  sleep 0.1
done
[[ -s "$reload_done" ]] || { echo "config-reload did not reply after the stalled submit returned"; exit 1; }
(( $(cat "$reload_done") > 0 )) \
  || { echo "config-reload replied before the scale applied: $(grep "applied mode" "$ANDROMEDA_LOG" | tail -1)"; exit 1; }
timeout 10 "$ANDROMEDA" settle > /dev/null || { echo "the output did not draw after the stalled submit returned"; exit 1; }
echo "a reload that changes the scale during a 3 s stalled submit leaves IPC under 250 ms (slowest $slowest ms) and replies once applied"
