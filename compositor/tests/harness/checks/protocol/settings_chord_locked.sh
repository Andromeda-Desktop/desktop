#!/usr/bin/env bash
# A chord capture never sees a key typed on the lock screen: the lock surface gets it. After the unlock the capture
# takes the next key (the control).
set -euo pipefail
source "$ANDROMEDA_HARNESS_LIB"

readonly SETTINGS_CLIENT="${ANDROMEDA_DESKTOP_CLIENT:-./build-debug/tests/desktop-client}"
readonly LOCK_CLIENT="${ANDROMEDA_LOCK_CLIENT:-./build-debug/tests/lock-client}"
readonly POINTER="${ANDROMEDA_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
readonly CAPTURE_LOG="$ANDROMEDA_RUNTIME_DIR/chord-capture.log"
readonly LOCK_LOG="$ANDROMEDA_RUNTIME_DIR/lock-client.log"
readonly LOCK_FIFO="$ANDROMEDA_RUNTIME_DIR/lock-control"
readonly KEY_P=25

"$SETTINGS_CLIENT" chord-capture > "$CAPTURE_LOG" 2>&1 &
await_events "$CAPTURE_LOG" capturing 1 "the chord capture"

mkfifo "$LOCK_FIFO"
exec {lock_fd}<> "$LOCK_FIFO"
LOG_LOCK_INPUT=1 "$LOCK_CLIENT" <&"$lock_fd" > "$LOCK_LOG" 2>&1 &
await_events "$LOCK_LOG" locked 1 "the lock"
await_events "$LOCK_LOG" keyboard-enter 1 "the lock surface's keyboard focus"

"$POINTER" 1280 720 pause 50 key-press "$KEY_P" key-release "$KEY_P" pause 100 > /dev/null 2>&1
await_events "$LOCK_LOG" "keyboard-key code=$KEY_P state=pressed" 1 "the key on the lock surface"
if grep -q '^chord [^-]' "$CAPTURE_LOG"; then
  echo "the chord capture saw a key typed while locked: $(tr '\n' '|' < "$CAPTURE_LOG")"
  exit 1
fi

echo unlock >&"$lock_fd"
await_events "$LOCK_LOG" unlocked 1 "the unlock"
"$POINTER" 1280 720 pause 50 key-press "$KEY_P" key-release "$KEY_P" pause 100 > /dev/null 2>&1
await_events "$CAPTURE_LOG" "chord p" 1 "the control: the capture after the unlock"
echo "a key typed while locked went to the lock surface, not to the chord capture"
