#!/usr/bin/env bash
# Notification center panel: opening it clears the island banner that covered the column, and a click on a card body
# sends the "default" action to the app and closes the panel. Every step waits on state with a timeout.
# Shots go to $OUT (default ./artifacts/notifications-tab-actions).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/notifications-tab-actions}
source "$(dirname "$0")/lib.sh"
mkdir -p "$OUT"
export OUT POINTER=$ROOT/compositor/build-debug/tests/pointer-client
boot_headless 1

with_noctalia '
  fail() { echo "FAIL: $*" >&2; exit 1; }
  wait_for() { local t=$1; shift; for _ in $(seq $((t * 5))); do "$@" && return 0; sleep 0.2; done; return 1; }
  settle() {
    local i
    for i in $(seq 25); do
      grim "$OUT/prev.png"; sleep 0.2; grim "$OUT/now.png"
      cmp -s "$OUT/prev.png" "$OUT/now.png" && return 0
    done
    fail "the screen did not settle"
  }
  tag=e2e-nc-$$-$RANDOM
  # notify-send -w stays connected and prints the invoked action key: that is the app side of ActionInvoked.
  grim "$OUT/0-before.png"
  notify-send -w -t 0 -a "$tag" -A "default=Open" "Open $tag" "body" > "$OUT/default.txt" 2>&1 &
  # The banner on screen means the shell has the notification.
  bannered() { grim "$OUT/now.png"; ! cmp -s "$OUT/0-before.png" "$OUT/now.png"; }
  wait_for 5 bannered || fail "the notification banner did not show"
  "$NOCTALIA" msg panel-open notification-center > /dev/null
  settle
  cp "$OUT/now.png" "$OUT/1-open.png"
  # Column w 360 centred under the island, head capsule 48..96, first card from 104: its body, left of the clear mark.
  # The banner would sit over this point if the open had not cleared it.
  "$POINTER" 1280 720 move 560 140 > /dev/null
  "$POINTER" 1280 720 move 560 140 click 272 > /dev/null
  defaulted() { grep -q "default" "$OUT/default.txt"; }
  wait_for 5 defaulted || fail "a card body click did not send default (default.txt: $(cat "$OUT/default.txt"))"
  echo PASS
'
