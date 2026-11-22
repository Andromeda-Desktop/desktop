#!/usr/bin/env bash
# A launch icon whose output goes away before the launch ends must not touch that output: the launch expires (5 s
# without a window) and the compositor keeps running.
set -euo pipefail
source "$ANDROMEDA_HARNESS_LIB"

readonly LAYER_CLIENT="${ANDROMEDA_LAYER_CLIENT:-./build-debug/tests/layer-client}"
readonly TOKEN_CLIENT="${ANDROMEDA_ACTIVATION_TOKEN_CLIENT:-./build-debug/tests/activation-token-client}"
readonly LOG="$ANDROMEDA_RUNTIME_DIR/dsk-launch-from.log"
readonly FIFO="$ANDROMEDA_RUNTIME_DIR/dsk-launch-from.fifo"

"$ANDROMEDA" output-create HEADLESS-2 > /dev/null
for _ in $(seq 40); do
  "$ANDROMEDA" outputs | grep -q HEADLESS-2 && break
  sleep 0.05
done

mkfifo "$FIFO"
exec {control_fd}<> "$FIFO"
LAYER_DSK_VERSION=11 LAYER_CONTROL=1 "$LAYER_CLIENT" HEADLESS-2 40 <&"$control_fd" > "$LOG" 2>&1 &
client_pid=$!
trap 'kill "$client_pid" 2> /dev/null || true' EXIT
await_lines "$LOG" ready 1
printf '%s\n' "dsk-launch-from icon 100 0 40 40" >&"$control_fd"
await_lines "$LOG" 'done icon' 1
"$TOKEN_CLIENT" launch-from-app > /dev/null

"$ANDROMEDA" output-destroy HEADLESS-2 > /dev/null
# The launch limit is 5 s on the real clock; its expiry ends the icon.
for _ in $(seq 70); do
  "$ANDROMEDA" outputs > /dev/null || { echo "the compositor died after the launch output went away"; exit 1; }
  sleep 0.1
done
"$ANDROMEDA" outputs > /dev/null || { echo "the compositor died when the launch expired"; exit 1; }
echo "the launch icon expired after its output went away, and the compositor kept running"
