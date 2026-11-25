#!/usr/bin/env bash
# Terminal and Files survive a compositor crash in the same process. Fails when, after kill -SEGV of the compositor
# and a new compositor on the same socket name:
#   - the Terminal or Files process is gone (a new pid, or no process),
#   - either window does not map again within 10 s,
#   - the terminal's shell is a new process, or typed keys do not reach it,
#   - with a compositor that accepts but never answers, an app is still running 15 s later (no time limit).
# Screenshots: $OUT/before.png, $OUT/after.png.
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/app-reconnect}
source "$(dirname "$0")/lib.sh"
FILES=${FILES:-$ROOT/apps/files/build/andromeda-files}
TERMINAL_APP=${TERMINAL_APP:-$ROOT/apps/terminal/build/andromeda-terminal}
HOME_DIR=$RUNTIME/home
mkdir -p "$OUT" "$HOME_DIR/Documents"
fail() { echo "FAIL: $*"; exit 1; }
boot_headless 1
# env execs the compositor, so the background job pid is its pid.
compositor=$!

bus_info=$(dbus-daemon --config-file="$ROOT/tests/e2e/private-bus.conf" --fork --print-address=1 --print-pid=1)
BUS=$(sed -n 1p <<< "$bus_info")
BUS_PID=$(sed -n 2p <<< "$bus_info")
trap 'kill "$BUS_PID" 2> /dev/null || true; kill $(jobs -p) 2> /dev/null || true; wait 2> /dev/null; rm_runtime' EXIT

# The shell records its pid, so the check can tell a surviving shell from a new one.
cat > "$RUNTIME/test-shell" <<EOF
#!/bin/sh
echo \$\$ > "$RUNTIME/shell.pid"
cd "$RUNTIME"
exec /bin/sh
EOF
chmod +x "$RUNTIME/test-shell"

app() {
  run env DBUS_SESSION_BUS_ADDRESS="$BUS" HOME="$HOME_DIR" XDG_DATA_HOME="$HOME_DIR/.local/share" \
    XDG_STATE_HOME="$HOME_DIR/.local/state" XDG_CONFIG_HOME="$HOME_DIR/.config" XDG_CACHE_HOME="$HOME_DIR/.cache" \
    SHELL="$RUNTIME/test-shell" ANDROMEDA_LOG_LEVEL=debug "$@"
}
mapped() { [[ $(run "$ANDROMEDA" windows --json | jq "[.. | objects | select(.app_id? // \"\" | test(\"$1\"))] | length") -gt 0 ]]; }
await_mapped() {
  for _ in $(seq 200); do mapped "$1" && return 0; sleep 0.05; done
  fail "no $1 window $2"
}
shot() { run "$ANDROMEDA" settle > /dev/null; run grim "$OUT/$1.png"; }

app "$FILES" "$HOME_DIR" > "$OUT/files.log" 2>&1 &
FILES_PID=$!
await_mapped Files "at start"
app "$TERMINAL_APP" > "$OUT/terminal.log" 2>&1 &
TERMINAL_PID=$!
await_mapped Terminal "at start"
"$ROOT/tools/wait.sh" --file "$RUNTIME/shell.pid" --timeout 5 > /dev/null || fail "the terminal started no shell"
SHELL_PID=$(cat "$RUNTIME/shell.pid")
files=$(pgrep -P "$FILES_PID" || echo "$FILES_PID")
term=$(pgrep -P "$TERMINAL_PID" || echo "$TERMINAL_PID")
type_into_terminal() {
  run "$ANDROMEDA" msg "focus-or-spawn:dev.andromeda.Terminal $TERMINAL_APP" > /dev/null
  run "$ANDROMEDA" settle > /dev/null
  # A new wtype connection can lose its first key, and long text can garble: short text, leading spaces sh ignores.
  run wtype "  echo 1 > typed-$1" -k Return
  "$ROOT/tools/wait.sh" --file "$RUNTIME/typed-$1" --timeout 5 > /dev/null || { shot "typed-$1"; run "$ANDROMEDA" windows --json > "$OUT/windows-$1.json"; fail "keys typed $1 the restart did not reach the terminal's shell"; }
}
type_into_terminal before
shot before

kill -SEGV "$compositor"
wait "$compositor" 2> /dev/null || true
# The crash leaves wayland-0 (apps retry while it refuses) and the IPC socket boot_headless waits for.
rm -f "$RUNTIME/andromeda-wayland-0.sock"
mv "$OUT/andromeda.log" "$OUT/andromeda-1.log"
boot_headless 1
compositor=$!

await_mapped Files "after the restart"
await_mapped Terminal "after the restart"
kill -0 "$files" 2> /dev/null || fail "Files process $files died"
kill -0 "$term" 2> /dev/null || fail "Terminal process $term died"
grep -q "reconnected to the compositor" "$OUT/files.log" || fail "Files did not reconnect in-process"
grep -q "reconnected to the compositor" "$OUT/terminal.log" || fail "Terminal did not reconnect in-process"
kill -0 "$SHELL_PID" 2> /dev/null || fail "the terminal's shell $SHELL_PID died"

shot after
type_into_terminal after

# Socket activation as andromeda.socket does it: a supervisor holds wayland-0 across the crash and starts the
# compositor on it again (Restart=on-failure). The apps connect into the backlog and wait for the new compositor.
kill -SEGV "$compositor"
wait "$compositor" 2> /dev/null || true
rm -f "$RUNTIME/andromeda-wayland-0.sock"
cat > "$RUNTIME/held-socket.py" <<'PY'
import os, socket, subprocess, sys
path, log = sys.argv[1], sys.argv[2]
os.unlink(path)
server = socket.socket(socket.AF_UNIX)
server.bind(path)
server.listen(16)
fd = os.dup(server.fileno())  # above 3, so the shell redirect never closes it
command = ["/bin/sh", "-c", f'export LISTEN_PID=$$ LISTEN_FDS=1; exec "$@" 3<&{fd} {fd}<&-', "held", *sys.argv[3:]]
with open(log, "a") as out:
    while subprocess.run(command, pass_fds=(fd,), stdout=out, stderr=out).returncode != 0:
        pass
PY
headless_compositor "$RUNTIME" 1 "$RUNTIME/andromeda.toml" -uDBUS_SESSION_BUS_ADDRESS XDG_STATE_HOME="$RUNTIME/home/.local/state" \
  -- python3 -I "$RUNTIME/held-socket.py" "$RUNTIME/wayland-0" "$OUT/andromeda-held.log" &
SUPERVISOR=$!
await_mapped Files "on the socket-activated compositor"
await_mapped Terminal "on the socket-activated compositor"
held_compositor() { pgrep -P "$SUPERVISOR" -n; }
first=$(held_compositor)
kill -SEGV "$first"
for _ in $(seq 100); do [[ $(held_compositor) != "$first" && -n $(held_compositor) ]] && break; sleep 0.05; done
await_mapped Files "after the socket-activated restart"
await_mapped Terminal "after the socket-activated restart"
for log in files terminal; do
  [[ $(grep -c "reconnected to the compositor" "$OUT/$log.log") -ge 3 ]] || fail "$log did not reconnect through the held socket"
done
kill -0 "$files" 2> /dev/null && kill -0 "$term" 2> /dev/null || fail "an app died across the socket-activated restart"
type_into_terminal held

# A hung compositor: the socket accepts and never answers. The apps give up at their 10 s deadline and exit.
kill -KILL "$SUPERVISOR"
wait "$SUPERVISOR" 2> /dev/null || true
kill -KILL "$(pgrep -n -f "^$ANDROMEDA -c $RUNTIME/andromeda.toml")" 2> /dev/null || true
python3 -I -c '
import os, socket, sys
path = sys.argv[1]
os.unlink(path)
server = socket.socket(socket.AF_UNIX)
server.bind(path)
server.listen(8)
held = [server.accept()[0] for _ in range(8)]
' "$RUNTIME/wayland-0" &
for pid in "$files" "$term"; do
  "$ROOT/tools/wait.sh" --pid "$pid" --timeout 15 > /dev/null || fail "app $pid still runs 15 s after the compositor hung"
done
grep -q "timed out" "$OUT/files.log" || fail "Files did not report the reconnect time-out"
echo "pass app_reconnect"
