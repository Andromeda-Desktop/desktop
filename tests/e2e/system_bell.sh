#!/usr/bin/env bash
# xdg-system-bell end to end: a window rings the bell and the shell receives it over dsk_shell_v1, naming the app; the
# shell then plays the theme's "bell" sound, which this headless run has no PipeWire to hear. Writes bell.txt and
# noctalia.log to $OUT (default ./artifacts/system-bell).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/system-bell}
source "$(dirname "$0")/lib.sh"
boot_headless 1
CLIENT=${CLIENT:-$ROOT/compositor/build-debug/tests/surface-protocols-client}
export ANDROMEDA_LOG_LEVEL=debug

with_noctalia '
  "'"$CLIENT"'" bell terminal-app > "$OUT/client.log" 2>&1 &
  poll_for 5 grep -q "^rang$" "$OUT/client.log" || { echo "FAIL: the client never rang: $(cat "$OUT/client.log")"; exit 1; }
  poll_for 5 grep -q "system bell from .terminal-app." "$OUT/noctalia.log" || true
  grep "system bell from" "$OUT/noctalia.log" > "$OUT/bell.txt" || { echo "FAIL: the shell never heard the bell"; exit 1; }
'
cat "$OUT/bell.txt"
echo "PASS; artifacts: $OUT"
