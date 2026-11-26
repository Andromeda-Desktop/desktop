#!/usr/bin/env bash
# Settings › Software Update, Andromeda card (mock_update.py, MOCK_ANDROMEDA): "Update Andromeda" works while the Arch
# news still blocks the system update, calls UpgradeAndromeda, and a refusal for newer system packages says so.
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/settings-update-andromeda}
source "$(dirname "$0")/lib.sh"
export POINTER=$ROOT/compositor/build-debug/tests/pointer-client
boot_headless 1
: > "$OUT/calls.txt"
MOCK="$(cd "$(dirname "$0")" && pwd)/mock_update.py"
export MOCK_NEWS=1 MOCK_ANDROMEDA=1
mkdir -p "$OUT/bin"
printf '#!/bin/sh\necho "flatpak $*" >> "%s"\n' "$OUT/calls.txt" > "$OUT/bin/flatpak"
chmod +x "$OUT/bin/flatpak"
BEFORE_NOCTALIA='
  export PATH="$OUT/bin:$PATH"
  python3 '"$MOCK"' "$OUT/calls.txt" > "$OUT/mock.log" 2>&1 &
  WAIT_S=5 wait_for "mock update did not start" grep -q ready "$OUT/mock.log"
  start_andromeda_system
' with_noctalia '
  "$NOCTALIA" msg settings-open software-update > /dev/null
  wait_calls GetAll 1
  "$NOCTALIA" msg theme-mode-set dark > /dev/null
  sleep 0.5
  grim "$OUT/andromeda-available-dark.png"
  # Position on the 1280x720 headless output (the screenshot shows it).
  "$POINTER" 1280 720 move 975 225 click 272 > /dev/null   # Update Andromeda
  wait_calls "^UpgradeAndromeda" 1
  sleep 1
  grim "$OUT/andromeda-needs-system-dark.png"
  grep -q "^Upgrade " "$OUT/calls.txt" && { echo "FAIL: Update Andromeda started the full upgrade"; exit 1; }
  grep -q "^flatpak" "$OUT/calls.txt" && { echo "FAIL: Update Andromeda updated apps"; exit 1; }
  true
'
grep -n "software-update" "$OUT/noctalia.log" | grep -i -E "warn|error" && { echo "FAIL: software-update warnings"; exit 1; }
echo PASS
