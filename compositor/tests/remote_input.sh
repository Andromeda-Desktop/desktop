#!/usr/bin/env bash
# RemoteDesktop bridge (andromeda_remote_input_v1) against an isolated compositor with backend-boundary input:
# no fd before consent or after a denial, an allowed session moves the pointer
# through EIS, a remembered grant restores without asking, and revoking it ends the session and its EIS client.
set -euo pipefail
umask 077

readonly BUILD=$(realpath "${1:?usage: remote_input.sh ANDROMEDA_BUILD}")
readonly COMPOSITOR=$BUILD/tests/input-capture-compositor
readonly CLIENT=$BUILD/tests/remote-input-client
readonly REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
readonly RUNTIME=$(mktemp -d "${TMPDIR:-/tmp}/andromeda-remote-input.XXXXXXXX")
PIDS=()
SUCCESS=0
cleanup() {
  local pid
  for pid in "${PIDS[@]}"; do kill "$pid" 2> /dev/null || true; done
  for pid in "${PIDS[@]}"; do wait "$pid" 2> /dev/null || true; done
  if ((SUCCESS)); then rm -rf "$RUNTIME"; else echo "RemoteInput logs retained in $RUNTIME"; fi
}
trap cleanup EXIT

export XDG_RUNTIME_DIR=$RUNTIME XDG_CONFIG_HOME=$RUNTIME/config XDG_DATA_HOME=$RUNTIME/data-home
export XDG_CACHE_HOME=$RUNTIME/cache XDG_STATE_HOME=$RUNTIME/state XDG_DATA_DIRS=$RUNTIME/data
export DBUS_SESSION_BUS_ADDRESS=unix:path=$RUNTIME/no-bus DBUS_SYSTEM_BUS_ADDRESS=unix:path=$RUNTIME/no-system-bus
export GSETTINGS_BACKEND=memory NO_AT_BRIDGE=1
unset DISPLAY WAYLAND_SOCKET NOTIFY_SOCKET SYSTEMD_EXEC_PID
cat > "$RUNTIME/config.toml" << 'CONFIG'
[general]
xwayland = false
autostart = []
[animation]
enabled = false
[input.keyboard]
layout = "us"
numlock_toggle = false
CONFIG

mkfifo "$RUNTIME/physical.fifo"
exec {PHYSICAL_FD}<> "$RUNTIME/physical.fifo"
WAYLAND_DISPLAY=remote-input-isolated-parent WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 \
  "$COMPOSITOR" "$RUNTIME/config.toml" --trust-binary "$BUILD/" <&"$PHYSICAL_FD" > "$RUNTIME/compositor.log" 2>&1 &
PIDS+=("$!")
source "$REPO/tests/harness/lib.sh"
readonly LOG=$RUNTIME/compositor.log
await_events "$LOG" 'ready display=' 1 "isolated compositor"
export WAYLAND_DISPLAY ANDROMEDA_SOCKET
WAYLAND_DISPLAY=$(sed -n 's/^ready display=\([^ ]*\).*/\1/p' "$LOG" | head -1)
ANDROMEDA_SOCKET=$(sed -n 's/^ready .*socket=\([^ ]*\).*/\1/p' "$LOG" | head -1)

physical() { printf '%s\n' "$@" >&"$PHYSICAL_FD"; }
# Asks for a status line and prints it (confirmation, cursor x, y).
status() {
  local before
  before=$(events "$LOG" status)
  physical status
  await_events "$LOG" status "$((before + 1))" "status"
  grep '^status ' "$LOG" | tail -1
}
wait_confirmation() {
  for _ in $(seq 100); do
    status | grep -q 'confirmation=1' && return 0
  done
  echo "the consent panel did not show"
  return 1
}
# client <name>: starts a client fed from a fifo; send_<name> writes one command.
client() {
  mkfifo "$RUNTIME/$1.fifo"
  exec {fd}<> "$RUNTIME/$1.fifo"
  eval "send_$1() { printf '%s\\n' \"\$@\" >&$fd; }"
  "${2:-$CLIENT}" <&"$fd" > "$RUNTIME/$1.log" 2>&1 &
  PIDS+=("$!")
}

# An untrusted copy of the client sees no manager.
cp "$CLIENT" "$RUNTIME/untrusted-client"
"$RUNTIME/untrusted-client" < /dev/null > "$RUNTIME/untrusted.log" 2>&1 || true
grep -q '^no-manager' "$RUNTIME/untrusted.log" || { echo "an untrusted binary bound the remote input manager"; exit 1; }

# No fd before start; a denial gives no fd either.
client denied
await_events "$RUNTIME/denied.log" ready 1 "denied client"
send_denied connect
await_events "$RUNTIME/denied.log" 'result status=2' 1 "connect before start fails"
send_denied 'start 0 -'
wait_confirmation
physical 'tap 1'
await_events "$RUNTIME/denied.log" 'started status=1 capabilities=0' 1 "Escape denies"
send_denied connect
await_events "$RUNTIME/denied.log" 'result status=2' 2 "connect after a denial fails"

# Allow once: the seat offers both capabilities and the pointer moves by the emulated motion.
client once
await_events "$RUNTIME/once.log" ready 1 "once client"
send_once 'start 0 -'
wait_confirmation
physical 'tap 28'
await_events "$RUNTIME/once.log" 'started status=0 capabilities=3' 1 "Enter allows once"
send_once connect
await_events "$RUNTIME/once.log" eis-fd 1 "granted session gets an EIS fd"
await_events "$RUNTIME/once.log" 'seat keyboard=1 pointer=1' 1 "granted seat"
await_events "$RUNTIME/once.log" pointer-ready 1 "pointer device resumed"
physical 'at 100 100'
read -r x0 y0 < <(status | sed -n 's/.* x=\([0-9.]*\) y=\([0-9.]*\).*/\1 \2/p')
send_once 'move 40 30'
await_events "$RUNTIME/once.log" moved 1 "motion sent"
for _ in $(seq 50); do
  read -r x1 y1 < <(status | sed -n 's/.* x=\([0-9.]*\) y=\([0-9.]*\).*/\1 \2/p')
  [[ $x1 != "$x0" ]] && break
done
awk -v a="$x0" -v b="$x1" -v c="$y0" -v d="$y1" 'BEGIN { exit !((b - a) == 40 && (d - c) == 30) }' ||
  { echo "the pointer moved from $x0,$y0 to $x1,$y1, not by 40,30"; exit 1; }

# An emulated Enter from the granted keyboard cannot answer another session's consent panel.
await_events "$RUNTIME/once.log" keyboard-ready 1 "keyboard device resumed"
client probe
await_events "$RUNTIME/probe.log" ready 1 "probe client"
send_probe 'start 0 -'
wait_confirmation
send_once 'key 28'
await_events "$RUNTIME/once.log" keyed 1 "emulated Enter sent"
for _ in $(seq 20); do
  status | grep -q 'confirmation=1' || { echo "an emulated key closed the consent panel"; exit 1; }
done
grep -q '^started' "$RUNTIME/probe.log" && { echo "an emulated Enter answered the consent panel"; exit 1; }
physical 'tap 1'
await_events "$RUNTIME/probe.log" 'started status=1 capabilities=0' 1 "Escape denies the probe"

# Remember: R stores a grant; a new session restores it without the panel.
client remember
await_events "$RUNTIME/remember.log" ready 1 "remember client"
send_remember 'start 2 -'
wait_confirmation
physical 'tap 19'
await_events "$RUNTIME/remember.log" 'grant mode=2 token=' 1 "R remembers"
await_events "$RUNTIME/remember.log" 'started status=0 capabilities=3' 1 "remembered session starts"
token=$(sed -n 's/^grant mode=2 token=//p' "$RUNTIME/remember.log" | tail -1)
client restored
await_events "$RUNTIME/restored.log" ready 1 "restored client"
send_restored "start 2 $token"
await_events "$RUNTIME/restored.log" 'started status=0 capabilities=3' 1 "the token restores without asking"
status | grep -q 'confirmation=0' || { echo "a restored session asked again"; exit 1; }
send_restored connect
await_events "$RUNTIME/restored.log" pointer-ready 1 "restored session controls the pointer"

# Revoking every grant closes the restored session and disconnects its EIS client.
"$BUILD/andromeda" input-capture-revoke all > "$RUNTIME/revoke.log" 2>&1
await_events "$RUNTIME/restored.log" closed 1 "revoke closes the session"
await_events "$RUNTIME/restored.log" ei-disconnected 1 "revoke disconnects EIS"
SUCCESS=1
echo "remote input: consent, EIS motion, remember and revoke hold"
