#!/usr/bin/env bash
# RemoteDesktop through the real xdg-desktop-portal frontend, this portal and an isolated compositor with
# backend-boundary input: a denial starts nothing, an allowed session moves the pointer through the portal's EIS
# sender, a remembered grant restores without asking and hands out an EIS fd, revoking it ends the session, a
# caller other than the frontend cannot create a session, a session with sources streams the chooser's output and
# places absolute motion on it, a remembered token restores devices and sources without panel or chooser, a
# mode-1 grant ends when its app leaves the bus, and the clipboard moves both ways only for a session whose consent
# covered it, pauses while locked and ends with the session.
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
for command in pipewire wl-copy wl-paste; do
  command -v "$command" > /dev/null || { echo "Missing test command: $command"; exit 2; }
done
[[ -x $BUILD/tests/lock-client ]] || { echo "Missing executable: $BUILD/tests/lock-client"; exit 2; }

export XDG_RUNTIME_DIR=$RUNTIME XDG_CONFIG_HOME=$RUNTIME/config XDG_DATA_HOME=$RUNTIME/data-home
export XDG_CACHE_HOME=$RUNTIME/cache XDG_STATE_HOME=$RUNTIME/state XDG_DATA_DIRS=$RUNTIME/data
export DBUS_SESSION_BUS_ADDRESS=unix:path=$RUNTIME/bus DBUS_SYSTEM_BUS_ADDRESS=unix:path=$RUNTIME/no-system-bus
export XDG_CURRENT_DESKTOP=andromeda XDG_SESSION_TYPE=wayland XDG_DESKTOP_PORTAL_DIR=$RUNTIME/portals
export GSETTINGS_BACKEND=memory NO_AT_BRIDGE=1 GIO_USE_VFS=local
export PIPEWIRE_RUNTIME_DIR=$RUNTIME PIPEWIRE_REMOTE=pipewire-0
unset DISPLAY WAYLAND_SOCKET NOTIFY_SOCKET SYSTEMD_EXEC_PID PIPEWIRE_CONFIG_DIR PIPEWIRE_CONFIG_NAME
mkdir -p "$XDG_DESKTOP_PORTAL_DIR" "$XDG_CONFIG_HOME/xdg-desktop-portal-andromeda" "$XDG_DATA_HOME" "$XDG_STATE_HOME"
# The chooser counts its runs, so a restore can prove it skipped the chooser.
cat > "$RUNTIME/choose" << CHOOSER
#!/usr/bin/env bash
cat > /dev/null
echo run >> "$RUNTIME/chooser.runs"
printf '%s\n' '{"selections":[{"kind":"monitor","output":"HEADLESS-1"}]}'
CHOOSER
chmod +x "$RUNTIME/choose"
printf '[screencast]\nchooser_cmd = "%s"\n' "$RUNTIME/choose" > "$XDG_CONFIG_HOME/xdg-desktop-portal-andromeda/config.toml"
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
  --trust-binary "$(command -v wl-copy)" --trust-binary "$(command -v wl-paste)" \
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
pipewire > "$RUNTIME/pipewire.log" 2>&1 &
PIDS+=("$!")
for _ in $(seq 200); do [[ -S $RUNTIME/pipewire-0 ]] && break; sleep 0.025; done
[[ -S $RUNTIME/pipewire-0 ]] || { echo "PipeWire did not start"; exit 1; }
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
chooser_runs() { [[ -f $RUNTIME/chooser.runs ]] && wc -l < "$RUNTIME/chooser.runs" || echo 0; }
grants() { "$BUILD/andromeda" input-capture-permissions --json | jq length; }
# Waits until the pointer rests at $1,$2 in layout pixels.
await_pointer() {
  local x y
  for _ in $(seq 50); do
    read -r x y < <(pointer)
    awk -v a="$x" -v b="$1" -v c="$y" -v d="$2" 'BEGIN { exit !(a == b && c == d) }' && return 0
  done
  echo "the pointer is at $x,$y, not at $1,$2"
  return 1
}
client() {
  mkfifo "$RUNTIME/$1.fifo"
  exec {fd}<> "$RUNTIME/$1.fifo"
  eval "send_$1() { printf '%s\\n' \"\$@\" >&$fd; }"
  python3 -I "$CLIENT" <&"$fd" > "$RUNTIME/$1.log" 2>&1 &
  PIDS+=("$!")
  await_events "$RUNTIME/$1.log" ready 1 "$1 client"
}

# Only the frontend may call the impl objects: a direct caller gets AccessDenied and starts nothing.
direct() {
  gdbus call --session --dest org.freedesktop.impl.portal.desktop.andromeda --object-path /org/freedesktop/portal/desktop \
    --method "org.freedesktop.impl.portal.$1" "${@:2}" > "$RUNTIME/direct.log" 2>&1 && true
  grep -q 'AccessDenied' "$RUNTIME/direct.log" || { echo "a direct caller reached $1: $(cat "$RUNTIME/direct.log")"; exit 1; }
}
direct RemoteDesktop.CreateSession /org/x/request/1 /org/x/session/1 spoof '{}'
direct ScreenCast.CreateSession /org/x/request/2 /org/x/session/2 spoof '{}'
direct Screenshot.Screenshot /org/x/request/3 spoof '' '{}'
direct Screenshot.PickColor /org/x/request/4 spoof '' '{}'

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
await_events "$RUNTIME/once.log" 'started response=0 devices=3 streams=- token=-' 1 "Enter allows once"
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
await_events "$RUNTIME/remember.log" 'started response=0 devices=3 streams=- token=' 1 "R remembers"
token=$(sed -n 's/^started response=0 devices=3 streams=- token=//p' "$RUNTIME/remember.log" | tail -1)
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
send_revoked "session 2 $(sed -n 's/^started response=0 devices=3 streams=- token=//p' "$RUNTIME/restored.log" | tail -1)"
await_events "$RUNTIME/revoked.log" 'selected response=0' 1 "revoked: devices selected"
send_revoked start
await_events "$RUNTIME/revoked.log" 'started response=0 devices=3' 1 "second restore"
send_revoked 'move 1 1'
await_events "$RUNTIME/revoked.log" moved 1 "motion before revoke"
"$BUILD/andromeda" input-capture-revoke all > "$RUNTIME/revoke.log" 2>&1
await_events "$RUNTIME/revoked.log" session-closed 1 "revoke closes the session"
send_revoked 'move 1 1'
await_events "$RUNTIME/revoked.log" error 1 "Notify after the revoke fails"

# Devices with sources: the chooser runs after consent, Start returns its stream, and absolute motion in stream
# pixels lands at the same layout pixels on HEADLESS-1 (1280x720 at the origin, scale 1).
client shared
send_shared 'session 2 -'
await_events "$RUNTIME/shared.log" 'selected response=0' 1 "shared: devices selected"
send_shared sources
await_events "$RUNTIME/shared.log" 'sources response=0' 1 "shared: sources selected"
send_shared start
wait_confirmation
physical 'tap 19'
await_events "$RUNTIME/shared.log" 'started response=0 devices=3 streams=[1-9][0-9]* token=' 1 "Start streams the source"
(($(chooser_runs) == 1)) || { echo "the chooser ran $(chooser_runs) times, not once"; exit 1; }
read -r node token < <(sed -n 's/^started response=0 devices=3 streams=\([0-9]*\) token=\(.*\)/\1 \2/p' "$RUNTIME/shared.log")
[[ -n $token && $token != - ]] || { echo "the shared session returned no restore token"; exit 1; }
send_shared "abs $node 300 200"
await_events "$RUNTIME/shared.log" absed 1 "absolute motion sent"
await_pointer 300 200

# The token restores devices and sources: no panel, no chooser, and the new stream takes absolute motion.
client reshared
send_reshared "session 2 $token"
await_events "$RUNTIME/reshared.log" 'selected response=0' 1 "reshared: devices selected"
send_reshared sources
await_events "$RUNTIME/reshared.log" 'sources response=0' 1 "reshared: sources selected"
send_reshared start
await_events "$RUNTIME/reshared.log" 'started response=0 devices=3 streams=[1-9]' 1 "the token restores the source"
status | grep -q 'confirmation=0' || { echo "a restored shared session asked again"; exit 1; }
(($(chooser_runs) == 1)) || { echo "the restore ran the chooser"; exit 1; }
node=$(sed -n 's/^started response=0 devices=3 streams=\([0-9]*\) .*/\1/p' "$RUNTIME/reshared.log")
send_reshared "abs $node 900 500"
await_events "$RUNTIME/reshared.log" absed 1 "absolute motion on the restored stream"
await_pointer 900 500

# Mode 1 keeps a grant only while its app is on the bus: when the client quits, the compositor forgets it.
"$BUILD/andromeda" input-capture-revoke all > /dev/null
client transient
send_transient 'session 1 -'
await_events "$RUNTIME/transient.log" 'selected response=0' 1 "transient: devices selected"
send_transient start
wait_confirmation
physical 'tap 19'
await_events "$RUNTIME/transient.log" 'started response=0 devices=3 streams=- token=' 1 "R remembers for the run"
(($(grants) == 1)) || { echo "the mode-1 grant was not stored: $("$BUILD/andromeda" input-capture-permissions --json)"; exit 1; }
send_transient quit
for _ in $(seq 50); do
  (($(grants) == 0)) && break
  status > /dev/null
done
(($(grants) == 0)) || { echo "the mode-1 grant outlived its app: $("$BUILD/andromeda" input-capture-permissions --json)"; exit 1; }

# Clipboard: RequestClipboard before Start puts it in the consent; Enter enables it.
"$BUILD/andromeda" input-capture-revoke all > /dev/null
client clip
send_clip 'session 0 -'
await_events "$RUNTIME/clip.log" 'selected response=0' 1 "clip: devices selected"
send_clip clipboard
await_events "$RUNTIME/clip.log" clipboard-requested 1 "RequestClipboard"
send_clip start
wait_confirmation
physical 'tap 28'
await_events "$RUNTIME/clip.log" clipboard-enabled 1 "the consent enables the clipboard"
await_events "$RUNTIME/clip.log" 'started response=0 devices=3' 1 "clip: started"

# A local copy reaches the session, and SelectionRead returns it.
wl-copy --foreground --type text/plain 'local text' > "$RUNTIME/wl-copy.log" 2>&1 &
PIDS+=("$!")
await_events "$RUNTIME/clip.log" 'owner-changed owner=0 types=text/plain' 1 "a local copy reaches the session"
send_clip 'read text/plain'
await_events "$RUNTIME/clip.log" 'read local text' 1 "SelectionRead returns the local copy"

# SetSelection makes the session the owner; a local paste gets its text through SelectionTransfer.
send_clip 'set text/plain remote text'
await_events "$RUNTIME/clip.log" 'owner-changed owner=1 types=text/plain' 1 "SetSelection makes the session the owner"
pasted=$(timeout 10 wl-paste --no-newline --type text/plain) || { echo "wl-paste failed"; exit 1; }
[[ $pasted == "remote text" ]] || { echo "a local paste got '$pasted', not the session's text"; exit 1; }
await_events "$RUNTIME/clip.log" 'transferred text/plain ok=1' 1 "the session answered the transfer"

# A session that did not ask for the clipboard can neither read it nor hear of changes.
client noclip
send_noclip 'session 0 -'
await_events "$RUNTIME/noclip.log" 'selected response=0' 1 "noclip: devices selected"
send_noclip start
wait_confirmation
physical 'tap 28'
await_events "$RUNTIME/noclip.log" 'started response=0 devices=3' 1 "noclip: started"
send_noclip 'read text/plain'
await_events "$RUNTIME/noclip.log" error 1 "SelectionRead without clipboard access fails"
send_noclip 'set text/plain stolen'
await_events "$RUNTIME/noclip.log" error 2 "SetSelection without clipboard access fails"
(($(events "$RUNTIME/noclip.log" owner-changed) == 0)) || { echo "a session without clipboard access heard a change"; exit 1; }

# A lock pauses the clipboard; unlocking announces the selection again.
mkfifo "$RUNTIME/lock.fifo"
exec {LOCK_FD}<> "$RUNTIME/lock.fifo"
"$BUILD/tests/lock-client" <&"$LOCK_FD" > "$RUNTIME/lock.log" 2>&1 &
lock_pid=$!
PIDS+=("$lock_pid")
await_events "$RUNTIME/lock.log" locked 1 "session lock"
errors=$(events "$RUNTIME/clip.log" error)
send_clip 'read text/plain'
await_events "$RUNTIME/clip.log" error "$((errors + 1))" "SelectionRead while locked fails"
printf 'unlock\n' >&"$LOCK_FD"
wait "$lock_pid"
await_events "$RUNTIME/clip.log" 'owner-changed owner=1 types=text/plain' 2 "unlocking announces the selection"

# Closing the owning session withdraws its selection.
send_clip close
await_events "$RUNTIME/clip.log" closed 1 "clip: closed"
withdrawn=0
for _ in $(seq 50); do
  if ! timeout 5 wl-paste --list-types > "$RUNTIME/types.log" 2>&1 && grep -q 'Nothing is copied' "$RUNTIME/types.log"; then
    withdrawn=1
    break
  fi
  sleep 0.1
done
(( withdrawn )) || { echo "the closed session still owns the selection: $(cat "$RUNTIME/types.log")"; exit 1; }

# Model: random sequences of local copies and pastes, session sets and reads, races and locks over two sessions.
for name in model_a model_b; do
  client "$name"
  "send_$name" 'session 0 -'
  await_events "$RUNTIME/$name.log" 'selected response=0' 1 "$name: devices selected"
  "send_$name" clipboard
  "send_$name" start
  wait_confirmation
  physical 'tap 28'
  await_events "$RUNTIME/$name.log" clipboard-enabled 1 "$name: clipboard enabled"
  await_events "$RUNTIME/$name.log" owner-changed 1 "$name: the start announces the selection"
done
for seed in ${CLIPBOARD_SEEDS:-1 2 3}; do
  python3 -I "$HERE/clipboard_model.py" "$seed" "${CLIPBOARD_STEPS:-30}" "$BUILD/tests/lock-client" "$RUNTIME" \
    model_a model_b
done
SUCCESS=1
echo "remote desktop portal: frontend check, consent, EIS motion, restore, ConnectToEIS, revoke, sources," \
  "absolute motion, source restore, mode-1 expiry and the clipboard hold"
