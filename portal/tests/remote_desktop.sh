#!/usr/bin/env bash
# RemoteDesktop through the real xdg-desktop-portal frontend, this portal and an isolated compositor with
# backend-boundary input: a denial starts nothing, an allowed session moves the pointer through the portal's EIS
# sender, a remembered grant restores without asking and hands out an EIS fd, revoking it ends the session, and a
# caller other than the frontend cannot create a session.
set -euo pipefail
umask 077

readonly BUILD=$(realpath "${1:?usage: remote_desktop.sh ANDROMEDA_BUILD PORTAL_BUILD}")
readonly PORTAL_BUILD=$(realpath "${2:?portal build directory required}")
readonly COMPOSITOR=$BUILD/tests/input-capture-compositor
readonly PORTAL=$PORTAL_BUILD/xdg-desktop-portal-andromeda
readonly FRONTEND=${XDP_FRONTEND:-/usr/lib/xdg-desktop-portal}
readonly STORE=${XDP_PERMISSION_STORE:-/usr/lib/xdg-permission-store}
readonly HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
readonly CLIENT=$HERE/remote_desktop_client.py
readonly REPO=$(cd "$HERE/../../compositor" && pwd)
readonly WAIT=$HERE/../../tools/wait.sh
readonly RUNTIME=$(mktemp -d "${TMPDIR:-/tmp}/andromeda-remote-desktop.XXXXXXXX")
PIDS=()
SUCCESS=0
cleanup() {
  local pid
  for pid in "${PIDS[@]}"; do kill "$pid" 2> /dev/null || true; done
  for pid in "${PIDS[@]}"; do wait "$pid" 2> /dev/null || true; done
  if ((SUCCESS)); then rm -rf "$RUNTIME"; else echo "RemoteDesktop logs retained in $RUNTIME"; fi
}
trap cleanup EXIT

for executable in "$COMPOSITOR" "$PORTAL" "$FRONTEND" "$STORE" "$BUILD/andromeda"; do
  [[ -x $executable ]] || { echo "Missing executable: $executable"; exit 2; }
done

export XDG_RUNTIME_DIR=$RUNTIME XDG_CONFIG_HOME=$RUNTIME/config XDG_DATA_HOME=$RUNTIME/data-home
export XDG_CACHE_HOME=$RUNTIME/cache XDG_STATE_HOME=$RUNTIME/state XDG_DATA_DIRS=$RUNTIME/data
export DBUS_SESSION_BUS_ADDRESS=unix:path=$RUNTIME/bus DBUS_SYSTEM_BUS_ADDRESS=unix:path=$RUNTIME/no-system-bus
export XDG_CURRENT_DESKTOP=andromeda XDG_SESSION_TYPE=wayland XDG_DESKTOP_PORTAL_DIR=$RUNTIME/portals
export GSETTINGS_BACKEND=memory NO_AT_BRIDGE=1 GIO_USE_VFS=local
unset DISPLAY WAYLAND_SOCKET NOTIFY_SOCKET SYSTEMD_EXEC_PID
mkdir -p "$XDG_DESKTOP_PORTAL_DIR" "$XDG_CONFIG_HOME" "$XDG_DATA_HOME" "$XDG_STATE_HOME"
cp "$HERE/../data/andromeda.portal" "$XDG_DESKTOP_PORTAL_DIR/"
printf '[preferred]\ndefault=andromeda\n' > "$XDG_DESKTOP_PORTAL_DIR/portals.conf"
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
WAYLAND_DISPLAY=remote-desktop-isolated-parent WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 \
  "$COMPOSITOR" "$RUNTIME/config.toml" --trust-binary "$BUILD/" --trust-binary "$PORTAL_BUILD/" \
  <&"$PHYSICAL_FD" > "$RUNTIME/compositor.log" 2>&1 &
PIDS+=("$!")
source "$REPO/tests/harness/lib.sh"
readonly LOG=$RUNTIME/compositor.log
await_events "$LOG" 'ready display=' 1 "isolated compositor"
export WAYLAND_DISPLAY ANDROMEDA_SOCKET
WAYLAND_DISPLAY=$(sed -n 's/^ready display=\([^ ]*\).*/\1/p' "$LOG" | head -1)
ANDROMEDA_SOCKET=$(sed -n 's/^ready .*socket=\([^ ]*\).*/\1/p' "$LOG" | head -1)

dbus-daemon --session --nofork --address="$DBUS_SESSION_BUS_ADDRESS" > "$RUNTIME/bus.log" 2>&1 &
PIDS+=("$!")
await_bus_name() { "$WAIT" --dbus "$1" --timeout 10 > /dev/null || { echo "bus name $1 did not appear"; return 1; }; }
await_bus_name org.freedesktop.DBus
"$STORE" --replace > "$RUNTIME/store.log" 2>&1 &
PIDS+=("$!")
await_bus_name org.freedesktop.impl.portal.PermissionStore
"$PORTAL" > "$RUNTIME/portal.log" 2>&1 &
PIDS+=("$!")
await_bus_name org.freedesktop.impl.portal.desktop.andromeda
XDG_DATA_DIRS=/usr/share "$FRONTEND" --replace > "$RUNTIME/frontend.log" 2>&1 &
PIDS+=("$!")
await_bus_name org.freedesktop.portal.Desktop

physical() { printf '%s\n' "$@" >&"$PHYSICAL_FD"; }
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
pointer() { status | sed -n 's/.* x=\([0-9.]*\) y=\([0-9.]*\).*/\1 \2/p'; }
client() {
  mkfifo "$RUNTIME/$1.fifo"
  exec {fd}<> "$RUNTIME/$1.fifo"
  eval "send_$1() { printf '%s\\n' \"\$@\" >&$fd; }"
  python3 -I "$CLIENT" <&"$fd" > "$RUNTIME/$1.log" 2>&1 &
  PIDS+=("$!")
  await_events "$RUNTIME/$1.log" ready 1 "$1 client"
}

# Only the frontend may create sessions: a direct caller gets response 2 and no session object.
gdbus call --session --dest org.freedesktop.impl.portal.desktop.andromeda --object-path /org/freedesktop/portal/desktop \
  --method org.freedesktop.impl.portal.RemoteDesktop.CreateSession /org/x/request/1 /org/x/session/1 spoof '{}' \
  > "$RUNTIME/direct.log" 2>&1
grep -q '^(uint32 2,' "$RUNTIME/direct.log" || { echo "a direct caller created a session: $(cat "$RUNTIME/direct.log")"; exit 1; }

# A denial starts nothing; Notify on it fails.
client denied
send_denied 'session 0 -'
await_events "$RUNTIME/denied.log" 'selected response=0' 1 "denied: devices selected"
send_denied start
wait_confirmation
physical 'tap 1'
await_events "$RUNTIME/denied.log" 'started response=1' 1 "Escape denies"
send_denied 'move 5 5'
await_events "$RUNTIME/denied.log" error 1 "Notify after a denial fails"

# Allow once: NotifyPointerMotion moves the pointer by the sent delta through the portal's EIS sender.
client once
send_once 'session 0 -'
await_events "$RUNTIME/once.log" 'selected response=0' 1 "once: devices selected"
send_once start
wait_confirmation
physical 'tap 28'
await_events "$RUNTIME/once.log" 'started response=0 devices=3 token=-' 1 "Enter allows once"
physical 'at 100 100'
read -r x0 y0 < <(pointer)
send_once 'move 40 30'
await_events "$RUNTIME/once.log" moved 1 "motion sent"
for _ in $(seq 50); do
  read -r x1 y1 < <(pointer)
  [[ $x1 != "$x0" ]] && break
done
awk -v a="$x0" -v b="$x1" -v c="$y0" -v d="$y1" 'BEGIN { exit !((b - a) == 40 && (d - c) == 30) }' ||
  { echo "the pointer moved from $x0,$y0 to $x1,$y1, not by 40,30"; exit 1; }

# Remember: R stores a grant; the frontend's token restores it without the panel, and ConnectToEIS gives a socket.
client remember
send_remember 'session 2 -'
await_events "$RUNTIME/remember.log" 'selected response=0' 1 "remember: devices selected"
send_remember start
wait_confirmation
physical 'tap 19'
await_events "$RUNTIME/remember.log" 'started response=0 devices=3 token=' 1 "R remembers"
token=$(sed -n 's/^started response=0 devices=3 token=//p' "$RUNTIME/remember.log" | tail -1)
[[ -n $token && $token != - ]] || { echo "the frontend returned no restore token"; exit 1; }
client restored
send_restored "session 2 $token"
await_events "$RUNTIME/restored.log" 'selected response=0' 1 "restored: devices selected"
send_restored start
await_events "$RUNTIME/restored.log" 'started response=0 devices=3' 1 "the token restores without asking"
status | grep -q 'confirmation=0' || { echo "a restored session asked again"; exit 1; }
send_restored eis
await_events "$RUNTIME/restored.log" 'eis-fd socket' 1 "ConnectToEIS returns a socket"

# Revoking every grant closes the restored session: Notify on it then fails.
client revoked
send_revoked "session 2 $(sed -n 's/^started response=0 devices=3 token=//p' "$RUNTIME/restored.log" | tail -1)"
await_events "$RUNTIME/revoked.log" 'selected response=0' 1 "revoked: devices selected"
send_revoked start
await_events "$RUNTIME/revoked.log" 'started response=0 devices=3' 1 "second restore"
send_revoked 'move 1 1'
await_events "$RUNTIME/revoked.log" moved 1 "motion before revoke"
"$BUILD/andromeda" input-capture-revoke all > "$RUNTIME/revoke.log" 2>&1
await_events "$RUNTIME/revoked.log" session-closed 1 "revoke closes the session"
send_revoked 'move 1 1'
await_events "$RUNTIME/revoked.log" error 1 "Notify after the revoke fails"
SUCCESS=1
echo "remote desktop portal: frontend check, consent, EIS motion, restore, ConnectToEIS and revoke hold"
