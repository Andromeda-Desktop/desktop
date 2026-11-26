#!/usr/bin/env bash
# A finger scroll that ends while Super is held still ends at the client: the stop carries no direction, so a
# Mod+Wheel bind must not take it. Without the stop, a browser keeps its swipe-back arrow on screen.
set -euo pipefail

source "$ANDROMEDA_HARNESS_LIB"

readonly POINTER="${ANDROMEDA_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
readonly SEAT_LOG="${ANDROMEDA_SEAT_LOG_CLIENT:-./build-debug/tests/seat-log-client}"
readonly LOG="$ANDROMEDA_RUNTIME_DIR/scroll.log"

cat >> "$ANDROMEDA_CONFIG" <<'EOF'

[keybinds]
"Mod+WheelDown" = "workspace-next"
EOF
"$ANDROMEDA" msg config-reload > /dev/null

LOG_POINTER_INPUT=1 "$SEAT_LOG" scroll > "$LOG" 2>&1 &
await_count 1

"$POINTER" 1280 720 move 640 360 axis vertical 10 pause 100 mod logo axis-stop vertical pause 100 mod none

for _ in $(seq 40); do
  grep -q '^pointer-axis-stop axis=0$' "$LOG" && break
  sleep 0.05
done
if ! grep -q '^pointer-axis-stop axis=0$' "$LOG"; then
  echo "the scroll stop sent with Super held never reached the client: $(< "$LOG")"
  exit 1
fi
if [[ $("$ANDROMEDA" windows --json | jq -r '.[] | select(.title == "scroll") | .active') != true ]]; then
  echo "the scroll stop ran the Mod+WheelDown bind: $("$ANDROMEDA" windows --json)"
  exit 1
fi
echo "a scroll stop with Super held reaches the client and runs no wheel bind"
