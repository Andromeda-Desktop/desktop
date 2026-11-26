#!/usr/bin/env bash
# An exclusive-keyboard layer surface (a launcher, a lock-style prompt) on HEADLESS-2 holds keyboard focus. When
# HEADLESS-2 becomes a mirror of HEADLESS-1 its layer surfaces are no longer shown, so they must be closed, as when
# the output is unplugged, and focus must return to the window on HEADLESS-1 instead of staying on an invisible
# surface. Writes focus.txt, the layer client's log, and the compositor log to $OUT (default
# ./artifacts/mirror-layer-focus).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/mirror-layer-focus}
source "$(dirname "$0")/lib.sh"
boot_headless 2
LAYER=$ROOT/compositor/build-debug/tests/layer-client
POINTER=$ROOT/compositor/build-debug/tests/pointer-client
WINDOW=$ROOT/compositor/build-debug/tests/unmap-client
: > "$OUT/focus.txt"
result() { echo "$1" >> "$OUT/focus.txt"; }
# The window stays activated under a layer by design (8c3b57788), so the checks read keyboard focus, not `active`.
enters() { grep -c "^keyboard-enter$" "$1" || true; }
enters_reached() { [[ $(enters "$1") -ge $2 ]]; }
wait_enters() {
  poll_for 5 enters_reached "$1" "$2"
}

# Headless has no keyboard; a virtual one gives the seat keyboard focus to hand around, as a real session has.
run "$POINTER" 1 1 keyboard-only mod none mark ready pause 86400000 > "$OUT/keyboard.log" 2>&1 &
poll_for 5 grep -q '^ready$' "$OUT/keyboard.log" || true
run "$WINDOW" keep > "$OUT/window.log" 2>&1 &
wait_enters "$OUT/window.log" 1 && result "PASS the window on HEADLESS-1 has focus" || result "FAIL the window never took focus"
run "$LAYER" HEADLESS-2 40 keyboard=exclusive > "$OUT/layer.log" 2>&1 &
layer=$!
poll_for 5 grep -q '^ready$' "$OUT/layer.log" || true
wait_enters "$OUT/layer.log" 1 && grep -q '^keyboard-leave$' "$OUT/window.log" \
  && result "PASS the exclusive layer on HEADLESS-2 took keyboard focus" \
  || result "FAIL the exclusive layer did not take keyboard focus"

run "$DESKTOP_CLIENT" mirror HEADLESS-2 HEADLESS-1
layer_gone() { ! kill -0 "$layer" 2> /dev/null; }
poll_for 5 layer_gone || true
kill -0 "$layer" 2> /dev/null && result "FAIL the layer surface on the mirror was not closed" \
  || result "PASS the layer surface on the mirror was closed"
wait_enters "$OUT/window.log" 2 && result "PASS focus returned to the window on HEADLESS-1" \
  || result "FAIL focus stayed on the hidden layer surface"
run "$ANDROMEDA" layers > "$OUT/layers.txt" 2>&1 || true
run "$ANDROMEDA" windows --json > "$OUT/windows.json"

cat "$OUT/focus.txt"
! grep -q '^FAIL' "$OUT/focus.txt" || { echo "FAIL; artifacts: $OUT"; exit 1; }
echo "PASS; artifacts: $OUT"
