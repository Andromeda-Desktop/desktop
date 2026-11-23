#!/usr/bin/env bash
# harness: outputs=1
# The compositor holds each sleep through System1 (a mock on a private system bus) until the session may sleep.
# Unlocked, the shell's sleep_ready ends the hold at once. A lock client that locks during the sleep ends it once its
# frames are presented ("session is locked" before "ready for sleep" in the compositor log). With neither, the
# compositor locks behind the blank 1 s before the budget (1.2 s) ends, then sends SleepReady. Locked, the hold
# ends at once. Each SleepReady comes with Activity "sleeping-locked", except after the shell's sleep_ready.
set -euo pipefail

readonly SHELL_CLIENT=${ANDROMEDA_DESKTOP_CLIENT:-./build-debug/tests/desktop-client}
readonly LOCK_CLIENT=${ANDROMEDA_LOCK_CLIENT:-./build-debug/tests/lock-client}
readonly MOCK=${ANDROMEDA_SYSTEM1_MOCK:-./tests/harness/clients/system1_login_mock.py}
readonly USER_BUS=/run/user/$(id -u)/bus
if [[ ! -S $USER_BUS ]]; then
  echo "SKIP: needs the user bus"
  exit 0
fi

RUNTIME_DIR=$(mktemp -d /tmp/shb.XXXXXXXX)
PIDS=()
cleanup() {
  local pid
  for pid in "${PIDS[@]}"; do kill -KILL "$pid" 2> /dev/null || true; done
  for pid in "${PIDS[@]}"; do wait "$pid" 2> /dev/null || true; done
  rm -rf "$RUNTIME_DIR"
}
trap cleanup EXIT
# Prints to stderr: ready_ms runs in a command substitution.
fail() {
  {
    echo "$*"
    sed 's/^/  system1 | /' "$RUNTIME_DIR/system1.log"
    grep -i 'sleep' "$RUNTIME_DIR/compositor.log" | sed 's/^/  compositor | /' || true
  } >&2
  exit 1
}
# Waits up to 5 s for a command's output to equal the wanted text.
await() {
  local wanted=$1 out=""
  shift
  for _ in $(seq 100); do
    out=$("$@" 2> /dev/null || true)
    [[ $out == "$wanted" ]] && return 0
    sleep 0.05
  done
  fail "$* gave '$out', wanted '$wanted'"
}

readonly CONFIG=$RUNTIME_DIR/config.toml
readonly BUS_NAME=org.andromeda.Compositor1.SleepHold$$
ln -s "$USER_BUS" "$RUNTIME_DIR/bus"
printf '[general]\nxwayland = false\nautostart = []\n' > "$CONFIG"

dbus-daemon --session --nofork --print-address=3 3> "$RUNTIME_DIR/addr" &
PIDS+=($!)
await 1 sh -c "grep -c unix: '$RUNTIME_DIR/addr'"
SYSTEM_BUS=$(head -n1 "$RUNTIME_DIR/addr")
DBUS_SYSTEM_BUS_ADDRESS=$SYSTEM_BUS python3 "$MOCK" > "$RUNTIME_DIR/system1.log" 2>&1 &
PIDS+=($!)
await up head -n1 "$RUNTIME_DIR/system1.log"

env -u WAYLAND_DISPLAY -u DISPLAY -u DBUS_SESSION_BUS_ADDRESS \
  XDG_RUNTIME_DIR="$RUNTIME_DIR" \
  DBUS_SYSTEM_BUS_ADDRESS="$SYSTEM_BUS" \
  ANDROMEDA_TEST_SLEEP_HOLD=1 \
  ANDROMEDA_COMPOSITOR_BUS_NAME="$BUS_NAME" \
  WLR_BACKENDS=headless \
  WLR_LIBINPUT_NO_DEVICES=1 \
  WLR_HEADLESS_OUTPUTS=1 \
  "$ANDROMEDA" -c "$CONFIG" > "$RUNTIME_DIR/compositor.log" 2>&1 &
PIDS+=($!)
activity() {
  busctl --address="unix:path=$USER_BUS" get-property "$BUS_NAME" /org/andromeda/Compositor1/Apps \
    org.andromeda.Compositor1.Apps Activity
}
await 's "active"' activity
await 1 grep -c '^hold$' "$RUNTIME_DIR/system1.log"

readonly SHELL_FIFO=$RUNTIME_DIR/shell-control
mkfifo "$SHELL_FIFO"
exec {shell_fd}<> "$SHELL_FIFO"
XDG_RUNTIME_DIR=$RUNTIME_DIR WAYLAND_DISPLAY=wayland-0 DESKTOP_CLIENT_SHELL_VERSION=6 \
  "$SHELL_CLIENT" shell-events <&"$shell_fd" > "$RUNTIME_DIR/shell.log" 2>&1 &
PIDS+=($!)
await 1 grep -c "^ready$" "$RUNTIME_DIR/shell.log"

sleep_signal() {
  busctl --address="$SYSTEM_BUS" call org.andromeda.System1 /org/andromeda/System1/Login test.Login Sleep b "$1"
}
# The ms of the Nth SleepReady.
ready_ms() {
  await "$1" grep -c '^ready ' "$RUNTIME_DIR/system1.log"
  grep '^ready ' "$RUNTIME_DIR/system1.log" | sed -n "${1}p" | cut -d' ' -f2
}

sleep_signal true
await 's "sleeping"' activity
echo sleep-ready >&"$shell_fd"
ms=$(ready_ms 1)
((ms < 150)) || fail "SleepReady came ${ms} ms after sleep_ready's sleep, not at once"
[[ $(activity) == 's "sleeping"' ]] || fail "sleep_ready changed Activity to '$(activity)'"
sleep_signal false
await 's "active"' activity

sleep_signal true
await 's "sleeping"' activity
readonly LOCK_FIFO=$RUNTIME_DIR/lock-control
mkfifo "$LOCK_FIFO"
exec {lock_fd}<> "$LOCK_FIFO"
XDG_RUNTIME_DIR=$RUNTIME_DIR WAYLAND_DISPLAY=wayland-0 "$LOCK_CLIENT" <&"$lock_fd" > "$RUNTIME_DIR/lock.log" 2>&1 &
PIDS+=($!)
ready_ms 2 > /dev/null
await 1 grep -c '^locked$' "$RUNTIME_DIR/lock.log"
awk '/session is locked/ { locked = NR } /ready for sleep/ { ready = NR } END { exit !(locked && locked < ready) }' \
  "$RUNTIME_DIR/compositor.log" || fail "SleepReady before the lock client's frames were presented"
[[ $(activity) == 's "sleeping-locked"' ]] || fail "SleepReady while Activity is '$(activity)'"
sleep_signal false
await 's "locked"' activity
echo unlock >&"$lock_fd"
await 1 grep -c '^unlocked$' "$RUNTIME_DIR/lock.log"
await 's "active"' activity

sleep_signal true
ms=$(ready_ms 3)
((ms >= 150)) || fail "SleepReady came ${ms} ms into an unlocked sleep without sleep_ready, before the late lock"
[[ $(activity) == 's "sleeping-locked"' ]] || fail "SleepReady while Activity is '$(activity)'"
sleep_signal false
await 's "locked"' activity

sleep_signal true
ms=$(ready_ms 4)
((ms < 150)) || fail "a locked session's SleepReady came after ${ms} ms"
[[ $(activity) == 's "sleeping-locked"' ]] || fail "SleepReady while Activity is '$(activity)'"
sleep_signal false

echo "SleepReady: at once after sleep_ready, after a lock client's presented lock, after the late lock, at once when locked"
