#!/usr/bin/env bash
# libdesk-system core/process: a callback-based process::runShell completes through process::AsyncProcessManager,
# polled by the shell's main loop over a pidfd, instead of a helper thread per call. A [hooks] theme_mode_changed chain
# runs `sleep 1`, then a command that touches a marker: the hook chain starts the second command only from the first
# one's exit callback. Switching the theme mode fires it. Asserts that the marker appears and that the shell's peak
# thread count while the hook runs is no higher than before it. Writes thread-count.txt to $OUT
# (default ./artifacts/process-async-callback).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/process-async-callback}
source "$(dirname "$0")/lib.sh"
boot_headless 1

rm -f "$OUT/hook-done"
printf '\n[hooks]\ntheme_mode_changed = ["sleep 1", "touch %s/hook-done"]\n' "$OUT" \
  >> "$RUNTIME/home/.config/andromeda/shell/config.toml"

with_noctalia '
  noctalia_pid=$(pgrep -f "^$NOCTALIA\$" | head -1)
  [[ -n $noctalia_pid ]] || { echo "FAIL: noctalia not running"; exit 1; }
  threads_before=$(awk "/^Threads:/ {print \$2}" "/proc/$noctalia_pid/status")
  "$NOCTALIA" msg theme-mode-toggle > /dev/null # a toggle always changes the resolved mode

  threads_during=0
  ok=0
  for _ in $(seq 100); do
    [[ -f "$OUT/hook-done" ]] && { ok=1; break; }
    t=$(awk "/^Threads:/ {print \$2}" "/proc/$noctalia_pid/status")
    (( t > threads_during )) && threads_during=$t
    sleep 0.1
  done
  [[ $ok -eq 1 ]] || { echo "FAIL: the hook chain marker never appeared"; tail -n 30 "$OUT/noctalia.log"; exit 1; }

  echo "threads_before=$threads_before threads_during=$threads_during" | tee "$OUT/thread-count.txt"
  (( threads_during <= threads_before )) \
    || { echo "FAIL: thread count grew while the hook ran ($threads_before -> $threads_during)"; exit 1; }
'

grep -q "^threads_before=" "$OUT/thread-count.txt" || { echo "FAIL: no thread-count recorded"; exit 1; }
echo "PASS: the hook chain callback fired with no thread growth; $(cat "$OUT/thread-count.txt"); artifacts: $OUT"
