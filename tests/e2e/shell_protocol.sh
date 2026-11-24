#!/usr/bin/env bash
# dsk_shell_v1 end to end: a compositor `shell:` action opens a shell panel without starting a process, Caps Lock
# set as compositor lock state (harness command) reaches the shell over dsk_shell_v1, and the idle shell's wakeups are counted
# with the lock-keys OSD enabled (it polled LEDs every 200 ms before). Writes result.txt to $OUT (default
# ./artifacts/shell-protocol).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/shell-protocol}
source "$(dirname "$0")/lib.sh"
boot_headless 1

with_noctalia '
  layer_present() { "$ANDROMEDA" layers | grep -q "$1"; }
  "$ANDROMEDA" msg "shell:panel-open launcher" > /dev/null
  WAIT_S=5 wait_for "shell action did not open the launcher panel" layer_present noctalia-panel
  "$NOCTALIA" msg panel-close > /dev/null

  pid=$(pgrep -f "^$NOCTALIA\$" | head -1)
  wakeups() { cat /proc/"$pid"/task/*/status | awk "/^voluntary_ctxt_switches/ {s += \$2} END {print s}"; }
  # Startup work ends with the panel prewarm, which waits for the launcher to close.
  WAIT_S=15 wait_for "the panel prewarm did not finish" grep -q "panel prewarm done" "$OUT/noctalia.log"
  sleep 2 # real time: the launcher icon prefetch runs 1 s after a panel closes
  before=$(wakeups)
  sleep 5 # real time: idle measurement window
  idle=$(( ($(wakeups) - before) / 5 ))
  echo "idle shell wakeups/s: $idle" | tee "$OUT/result.txt"
  ((idle <= 10)) || { echo "FAIL: idle shell woke $idle times per second"; exit 1; }

  # Lock state comes from physical keyboards only, and headless has none: the harness command sets it.
  # The OSD shows in the center island (noctalia-osd is only the fallback for outputs without one).
  grim -g "440,0 400x64" "$OUT/island-before.png"
  "$ANDROMEDA" lock-keys-test caps > /dev/null
  shown=0
  for _ in $(seq 50); do
    sleep 0.1
    grim -g "440,0 400x64" "$OUT/island-caps.png"
    cmp -s "$OUT/island-before.png" "$OUT/island-caps.png" || { shown=1; break; }
  done
  ((shown)) || layer_present noctalia-osd || { echo "FAIL: Caps Lock did not reach the shell"; exit 1; }
'
echo "PASS; artifacts: $OUT"
