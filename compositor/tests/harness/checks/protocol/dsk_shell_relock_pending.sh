#!/usr/bin/env bash
# A lock client that dies before its lock is confirmed leaves the session locked behind the secure blank, and the
# shell gets relock; the desktop must not come back.
set -euo pipefail
source "$ANDROMEDA_HARNESS_LIB"

readonly LAYER_CLIENT="${ANDROMEDA_LAYER_CLIENT:-./build-debug/tests/layer-client}"
readonly LOCK_CLIENT="${ANDROMEDA_LOCK_CLIENT:-./build-debug/tests/lock-client}"
readonly SHELL_CLIENT="${ANDROMEDA_DESKTOP_CLIENT:-./build-debug/tests/desktop-client}"
readonly PROBE="${ANDROMEDA_PIXEL_PROBE:-./build-debug/tests/pixel-probe}"
readonly SHELL_LOG="$ANDROMEDA_RUNTIME_DIR/shell-events.log"
readonly BACKGROUND_LOG="$ANDROMEDA_RUNTIME_DIR/background.log"
readonly PENDING_LOG="$ANDROMEDA_RUNTIME_DIR/lock-pending.log"
readonly SECOND_LOG="$ANDROMEDA_RUNTIME_DIR/lock-second.log"
readonly PENDING_FIFO="$ANDROMEDA_RUNTIME_DIR/lock-pending-control"
readonly SECOND_FIFO="$ANDROMEDA_RUNTIME_DIR/lock-second-control"

pixel() {
  local image="$ANDROMEDA_RUNTIME_DIR/$1.png"
  grim -o HEADLESS-1 "$image"
  "$PROBE" "$image" pixel 32 32
}

cat >> "$ANDROMEDA_CONFIG" <<'EOF'

[colors]
backdrop = "#00000000"
EOF
"$ANDROMEDA" msg config-reload > /dev/null

"$SHELL_CLIENT" shell-events > "$SHELL_LOG" 2>&1 &
await_events "$SHELL_LOG" ready 1 "the shell stand-in"
"$LAYER_CLIENT" HEADLESS-1 0 > "$BACKGROUND_LOG" 2>&1 &
await_events "$BACKGROUND_LOG" ready 1 "the desktop background"
desktop=$(pixel desktop)

mkfifo "$PENDING_FIFO" "$SECOND_FIFO"
exec {pending_fd}<> "$PENDING_FIFO"
"$LOCK_CLIENT" --defer-commit <&"$pending_fd" > "$PENDING_LOG" 2>&1 &
pending_pid=$!
await_events "$PENDING_LOG" configured 1 "the pending lock"
kill -KILL "$pending_pid"
wait "$pending_pid" || true

await_events "$SHELL_LOG" relock 1 "relock after the pending lock client died"
if [[ $(pixel abandoned) != "0 0 0" ]]; then
  echo "the desktop came back after the pending lock client died: desktop $desktop, now $(pixel abandoned-again)"
  exit 1
fi

exec {second_fd}<> "$SECOND_FIFO"
"$LOCK_CLIENT" <&"$second_fd" > "$SECOND_LOG" 2>&1 &
second_pid=$!
await_events "$SECOND_LOG" locked 1 "the replacement lock"
echo unlock >&"$second_fd"
await_events "$SECOND_LOG" unlocked 1 "the unlock"
wait "$second_pid"
echo "a pending lock client's death kept the session locked and asked the shell to relock"
