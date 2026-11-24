#!/usr/bin/env bash
# A real keyboard-backed XDG token records the source scratchpad. The destination client has no private inherited
# launch token and is created only after another pad opens, so neither environment fallback nor current-pad capture
# can produce the expected placement.
set -euo pipefail

source "$ANDROMEDA_HARNESS_LIB"

readonly CLIENT="${ANDROMEDA_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly POINTER="${ANDROMEDA_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
readonly SOURCE_LOG="$ANDROMEDA_RUNTIME_DIR/pad-token-source.log"
readonly TARGET_LOG="$ANDROMEDA_RUNTIME_DIR/pad-token-target.log"
readonly TOKEN_FILE="$ANDROMEDA_RUNTIME_DIR/pad-token"
readonly SOURCE_FIFO="$ANDROMEDA_RUNTIME_DIR/pad-token-source-control"
readonly TARGET_FIFO="$ANDROMEDA_RUNTIME_DIR/pad-token-target-control"

cat >> "$ANDROMEDA_CONFIG" <<'EOF'

[animation]
enabled = false

[input.focus]
follows_mouse = false

[[scratchpad]]
name = "input"

[[scratchpad]]
name = "other"
EOF
"$ANDROMEDA" msg config-reload > /dev/null

mkfifo "$SOURCE_FIFO" "$TARGET_FIFO"
exec {source_fd}<>"$SOURCE_FIFO"
exec {target_fd}<>"$TARGET_FIFO"
REMAP_ON_STDIN=1 ACTIVATION_TOKEN_FILE="$TOKEN_FILE" "$CLIENT" pad-token-source 300 200 \
  <&"$source_fd" > "$SOURCE_LOG" 2>&1 &
await_field pad-token-source active true
"$ANDROMEDA" msg window-move-to-scratchpad:input > /dev/null
"$ANDROMEDA" msg scratchpad-toggle:input > /dev/null
await_field pad-token-source active true
"$POINTER" 1280 720 tap 30 pause 5000 > "$ANDROMEDA_RUNTIME_DIR/pad-token-pointer.log" 2>&1 &
for _ in $(seq 80); do
  grep -q '^key 30 1$' "$SOURCE_LOG" && break
  sleep 0.1
done
grep -q '^key 30 1$' "$SOURCE_LOG"
printf i >&"$source_fd"
for _ in $(seq 80); do
  [[ -s $TOKEN_FILE ]] && grep -q '^input-activation-token-written$' "$SOURCE_LOG" && break
  sleep 0.1
done
[[ -s $TOKEN_FILE ]]
grep -q '^input-activation-token-written$' "$SOURCE_LOG"

"$ANDROMEDA" msg scratchpad-toggle:other > /dev/null
env -u ANDROMEDA_LAUNCH_TOKEN XDG_ACTIVATION_TOKEN="$(< "$TOKEN_FILE")" ACTIVATE_ON_START=1 MAP_ON_STDIN=1 \
  "$CLIENT" pad-token-target 300 200 <&"$target_fd" > "$TARGET_LOG" 2>&1 &
for _ in $(seq 80); do
  grep -q '^map-pending$' "$TARGET_LOG" 2>/dev/null && break
  sleep 0.1
done
grep -q '^map-pending$' "$TARGET_LOG"
printf m >&"$target_fd"
await_field pad-token-target scratchpad input
await_field pad-token-target active false
await_field pad-token-source active false

"$ANDROMEDA" msg \
  "spawn:exec '$CLIENT' pad-token-other 300 200 > '$ANDROMEDA_RUNTIME_DIR/pad-token-other.log' 2>&1" > /dev/null
await_field pad-token-other scratchpad other
await_field pad-token-other active true

echo "keyboard-backed XDG launch retains its hidden source pad without inherited-token fallback or focus theft"
