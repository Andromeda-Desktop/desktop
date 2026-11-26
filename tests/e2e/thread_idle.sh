#!/usr/bin/env bash
# Counts the threads the shell created ("dsk-" prefix, via `noctalia msg debug-threads`) on an idle desktop (a headless
# Andromeda, a clock-only bar). Library threads are not counted. Allowed at idle: none. The IPC call answers only after
# startup, so the check polls it until the count settles (two equal answers in a row) or a timeout, then asserts 0.
# On-demand workers (dsk-worker) exit after their idle timeout, so the poll runs up to 60 s. Writes threads.txt to $OUT
# (default ./artifacts/thread-idle); threads-early.txt holds the first answer.
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/thread-idle}
# dsk-log: the permanent log writer, asleep on its queue (no wakeups at idle).
export ALLOWED=${ALLOWED:-dsk-log}
source "$(dirname "$0")/lib.sh"
boot_headless 1

with_noctalia '
  poll_for 10 "$NOCTALIA" msg debug-threads > /dev/null 2>&1 || true
  "$NOCTALIA" msg debug-threads > "$OUT/threads-early.txt"
  idle_now() { [[ $("$NOCTALIA" msg debug-threads | tail -n +2 | grep -vxcE "${ALLOWED:-^$}" || true) == 0 ]]; }
  poll_for 60 idle_now || true
  "$NOCTALIA" msg debug-threads > "$OUT/threads.txt"
'
cat "$OUT/threads.txt"
bad=$(tail -n +2 "$OUT/threads.txt" | grep -vxE "${ALLOWED:-^$}" || true)
[[ -z "$bad" ]] || { echo "FAIL: dsk- threads alive at idle: $(echo "$bad" | tr '\n' ' ')"; exit 1; }
echo "PASS; artifacts: $OUT"
