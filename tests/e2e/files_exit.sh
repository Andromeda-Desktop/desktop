#!/usr/bin/env bash
# Files quits on SIGTERM (session logout, systemd stop). Fails when:
#   - a Files window with its drives, index search and Link client up does not exit within 3 s of SIGTERM,
#   - it exits by a signal or abort instead of status 0.
# On a hang it sends SIGQUIT, so systemd-coredump keeps every thread's stack (coredumpctl info <pid>).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/files-exit}
source "$(dirname "$0")/lib.sh"
FILES=${FILES:-$ROOT/apps/files/build/andromeda-files}
APP=dev.andromeda.Files
HOME_DIR=$RUNTIME/home
mkdir -p "$OUT" "$HOME_DIR/Documents"
fail() { echo "FAIL: $*"; exit 1; }
boot_headless 1

bus_info=$(dbus-daemon --config-file="$ROOT/tests/e2e/private-bus.conf" --fork --print-address=1 --print-pid=1)
BUS=$(sed -n 1p <<< "$bus_info")
BUS_PID=$(sed -n 2p <<< "$bus_info")
trap 'kill "$BUS_PID" 2> /dev/null || true; kill $(jobs -p) 2> /dev/null || true; wait 2> /dev/null; rm_runtime' EXIT

spawn env DBUS_SESSION_BUS_ADDRESS="$BUS" HOME="$HOME_DIR" XDG_DATA_HOME="$HOME_DIR/.local/share" \
  XDG_STATE_HOME="$HOME_DIR/.local/state" XDG_CONFIG_HOME="$HOME_DIR/.config" XDG_CACHE_HOME="$HOME_DIR/.cache" \
  ANDROMEDA_LOG_LEVEL=debug "$FILES" "$HOME_DIR" > "$OUT/files.log" 2>&1
PID=$!
mapped() { [[ $(run "$ANDROMEDA" windows --json | jq "[.. | objects | select(.app_id? == \"$APP\")] | length") -gt 0 ]]; }
for _ in $(seq 200); do mapped && break; sleep 0.05; done
mapped || fail "the Files window did not map"
# The drive monitor starts 300 ms after the first window: let it come up so its teardown runs too.
for _ in $(seq 40); do grep -q "sidebar" "$OUT/files.log" && break; sleep 0.05; done
sleep 0.5
run "$ANDROMEDA" settle > /dev/null
kill -TERM "$PID"
# Files is this shell's child, so kill -0 sees it until wait reaps it: a watchdog sends SIGQUIT after 3 s instead.
(sleep 3; kill -QUIT "$PID" 2> /dev/null) &
WATCHDOG=$!
status=0
wait "$PID" || status=$?
kill "$WATCHDOG" 2> /dev/null || true
((status != 128 + 3)) || fail "Files did not exit within 3 s of SIGTERM (core: coredumpctl info $PID)"
[[ $status -eq 0 ]] || fail "Files exited with status $status after SIGTERM (coredumpctl info $PID)"
echo "pass files_exit"
