#!/usr/bin/env bash
# Captures across a session lock: running output and window captures get black frames while locked and the window's
# content again after unlock, without a new session; a window capture started while locked is stopped at once.
set -euo pipefail
source "$ANDROMEDA_HARNESS_LIB"

readonly WINDOW_LOG="$ANDROMEDA_RUNTIME_DIR/lock-capture-window.log"
readonly OUTPUT_CAPTURE_LOG="$ANDROMEDA_RUNTIME_DIR/lock-capture-output.log"
readonly WINDOW_CAPTURE_LOG="$ANDROMEDA_RUNTIME_DIR/lock-capture-running.log"
readonly NEW_CAPTURE_LOG="$ANDROMEDA_RUNTIME_DIR/lock-capture-new.log"
readonly LOCK_LOG="$ANDROMEDA_RUNTIME_DIR/lock-capture-lock.log"
readonly BLACK='pixels .* uniform=1 color=ff000000'
readonly CONTENT='pixels .* color=ff\(ff0000\|0000ff\)'

pids=()
trap 'kill "${pids[@]}" 2> /dev/null || true' EXIT

"$ANDROMEDA_SUBSURFACE_CLIENT" lock-capture > "$WINDOW_LOG" 2>&1 &
pids+=($!)
await_lines "$WINDOW_LOG" mapped 1
"$ANDROMEDA" settle
window_id=$("$ANDROMEDA" windows --json | jq -r '.[] | select(.app_id == "lock-capture") | .id')

CAPTURE_PIXELS=1 "$ANDROMEDA_CAPTURE_CLIENT" --output HEADLESS-1 > "$OUTPUT_CAPTURE_LOG" 2>&1 &
pids+=($!)
CAPTURE_PIXELS=1 "$ANDROMEDA_CAPTURE_CLIENT" --window "$window_id" > "$WINDOW_CAPTURE_LOG" 2>&1 &
pids+=($!)
await_lines "$OUTPUT_CAPTURE_LOG" 'pixels ' 1
await_lines "$WINDOW_CAPTURE_LOG" "$CONTENT" 1
if [[ $(grep -c "$BLACK" "$WINDOW_CAPTURE_LOG" || true) -ne 0 ]]; then
  echo "the window capture is black before the lock: $(grep '^pixels' "$WINDOW_CAPTURE_LOG" | tail -3 | tr '\n' '|')"
  exit 1
fi

# The lock surface is 0xff102030, so a uniform black output frame is the redaction, not the lock surface.
output_before=$(grep -c "$BLACK" "$OUTPUT_CAPTURE_LOG" || true)
window_red=$(grep -c "$CONTENT" "$WINDOW_CAPTURE_LOG" || true)
mkfifo "$ANDROMEDA_RUNTIME_DIR/lock-capture.fifo"
exec {lock_fd}<> "$ANDROMEDA_RUNTIME_DIR/lock-capture.fifo"
"$ANDROMEDA_LOCK_CLIENT" <&"$lock_fd" > "$LOCK_LOG" 2>&1 &
lock_pid=$!
await_lines "$LOCK_LOG" locked 1

# The lock surface and the desktop behind it: an output capture shows neither.
if ! await_lines "$OUTPUT_CAPTURE_LOG" "$BLACK" $((output_before + 1)) > /dev/null; then
  echo "a running output capture shows the locked screen: $(grep '^pixels' "$OUTPUT_CAPTURE_LOG" | tail -3 | tr '\n' '|')"
  exit 1
fi
if ! await_lines "$WINDOW_CAPTURE_LOG" "$BLACK" 1 > /dev/null; then
  echo "a running window capture shows the window while locked: $(grep '^pixels' "$WINDOW_CAPTURE_LOG" | tail -3 | tr '\n' '|')"
  exit 1
fi

CAPTURE_PIXELS=1 "$ANDROMEDA_CAPTURE_CLIENT" --window "$window_id" > "$NEW_CAPTURE_LOG" 2>&1 &
pids+=($!)
if ! await_lines "$NEW_CAPTURE_LOG" session-stopped 1 > /dev/null; then
  echo "a window capture started while locked was not stopped: $(tail -3 "$NEW_CAPTURE_LOG" | tr '\n' '|')"
  exit 1
fi
if [[ $(grep -c '^pixels' "$NEW_CAPTURE_LOG" || true) -ne 0 ]]; then
  echo "a window capture started while locked got frames: $(grep '^pixels' "$NEW_CAPTURE_LOG" | tail -3 | tr '\n' '|')"
  exit 1
fi

printf 'unlock\n' >&"$lock_fd"
wait "$lock_pid"
exec {lock_fd}>&-
"$ANDROMEDA" settle
if ! await_lines "$WINDOW_CAPTURE_LOG" "$CONTENT" $((window_red + 1)) > /dev/null; then
  echo "the running window capture stays black after unlock: $(grep '^pixels' "$WINDOW_CAPTURE_LOG" | tail -3 | tr '\n' '|')"
  exit 1
fi
if grep -q session-stopped "$WINDOW_CAPTURE_LOG" "$OUTPUT_CAPTURE_LOG"; then
  echo "the lock stopped a running capture"
  exit 1
fi
echo "locked: output and window captures black, a new window capture stopped; unlocked: the window capture resumed"
