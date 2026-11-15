#!/usr/bin/env bash
# Terminal and Files survive a compositor crash in the same process. Fails when, after kill -SEGV of the compositor
# and a new compositor on the same socket name:
#   - the Terminal or Files process is gone (a new pid, or no process),
#   - either window does not map again within 10 s,
#   - the terminal's shell is a new process, or typed keys do not reach it.
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
for _ in $(seq 100); do [[ -s $RUNTIME/shell.pid ]] && break; sleep 0.05; done
SHELL_PID=$(cat "$RUNTIME/shell.pid")
files=$(pgrep -P "$FILES_PID" || echo "$FILES_PID")
term=$(pgrep -P "$TERMINAL_PID" || echo "$TERMINAL_PID")
type_into_terminal() {
  run "$ANDROMEDA" msg "focus-or-spawn:dev.andromeda.Terminal $TERMINAL_APP" > /dev/null
  run "$ANDROMEDA" settle > /dev/null
  # A new wtype connection can lose its first key, and long text can garble: short text, leading spaces sh ignores.
  run wtype "  touch typed-$1" -k Return
  for _ in $(seq 100); do [[ -e $RUNTIME/typed-$1 ]] && break; sleep 0.05; done
  [[ -e $RUNTIME/typed-$1 ]] || { shot "typed-$1"; run "$ANDROMEDA" windows --json > "$OUT/windows-$1.json"; fail "keys typed $1 the restart did not reach the terminal's shell"; }
}
type_into_terminal before
shot before

kill -SEGV "$compositor"
wait "$compositor" 2> /dev/null || true
rm -f "$RUNTIME/wayland-0" "$RUNTIME/wayland-0.lock" "$RUNTIME/andromeda-wayland-0.sock"
mv "$OUT/andromeda.log" "$OUT/andromeda-1.log"
boot_headless 1

await_mapped Files "after the restart"
await_mapped Terminal "after the restart"
kill -0 "$files" 2> /dev/null || fail "Files process $files died"
kill -0 "$term" 2> /dev/null || fail "Terminal process $term died"
grep -q "reconnected to the compositor" "$OUT/files.log" || fail "Files did not reconnect in-process"
grep -q "reconnected to the compositor" "$OUT/terminal.log" || fail "Terminal did not reconnect in-process"
kill -0 "$SHELL_PID" 2> /dev/null || fail "the terminal's shell $SHELL_PID died"

shot after
type_into_terminal after
echo "pass app_reconnect"
