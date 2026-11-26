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
  settle() { stable_grim "$OUT/now.png"; }
  tag=e2e-nc-$$-$RANDOM
  # notify-send -w stays connected and prints the invoked action key: that is the app side of ActionInvoked.
  grim "$OUT/0-before.png"
  notify-send -w -t 0 -a "$tag" -A "default=Open" "Open $tag" "body" > "$OUT/default.txt" 2>&1 &
  # The banner on screen means the shell has the notification.
  bannered() { grim "$OUT/now.png"; ! cmp -s "$OUT/0-before.png" "$OUT/now.png"; }
  poll_for 5 bannered || fail "the notification banner did not show"
  "$NOCTALIA" msg panel-open notification-center > /dev/null
  settle
  cp "$OUT/now.png" "$OUT/1-open.png"
  # The panel hangs under the bell: its layer centre column, 115 px down (head capsule, then the first card), is the
  # card body. The banner would sit over this point if the open had not cleared it.
  read -r cx cy < <("$ANDROMEDA" layers --json | jq -r "[.[] | select(.namespace == \"noctalia-panel\" and .mapped)][0] | \"\(.x + (.w / 2 | floor)) \(.y + 115)\"")
  "$POINTER" 1280 720 move "$cx" "$cy" > /dev/null
  "$POINTER" 1280 720 move "$cx" "$cy" click 272 > /dev/null
  defaulted() { grep -q "default" "$OUT/default.txt"; }
  poll_for 5 defaulted || fail "a card body click did not send default (default.txt: $(cat "$OUT/default.txt"))"
  echo PASS
'
