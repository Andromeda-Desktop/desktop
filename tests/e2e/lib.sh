# Sourced by the E2E scripts: a contained headless Andromeda session with the Noctalia fork running inside it.
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P) # canonical: --trust-binary "$ROOT/" matches /proc/<pid>/exe
ANDROMEDA=${ANDROMEDA:-$ROOT/compositor/build-debug/andromeda}
NOCTALIA=${NOCTALIA:-$ROOT/shell/build-debug/andromeda-shell}
DESKTOP_CLIENT=${DESKTOP_CLIENT:-$ROOT/compositor/build-debug/tests/desktop-client}
RUNTIME=$(mktemp -d /tmp/dsk.XXXX)
source "$ROOT/tests/headless.sh"
trap 'kill $(jobs -p) 2>/dev/null || true; wait 2>/dev/null; rm -rf "$RUNTIME"' EXIT

# boot_headless OUTPUTS: start Andromeda with that many headless outputs; its log goes to $OUT/andromeda.log. The private
# bus is its session and system bus, so the host's power profile, keymap and logind never reach a test.
# SLEEP_ON_TEST_BUS=1: the compositor holds the logind sleep it follows on the private bus.
boot_headless() {
  # A subshell drops the EXIT trap and owns its own jobs: stop them when it exits, or they outlive the test.
  [[ $BASHPID == "$$" ]] || trap 'kill $(jobs -p) 2> /dev/null || true; wait 2> /dev/null' EXIT
  mkdir -p "$OUT" "$RUNTIME/home/.config/andromeda/shell"
  # No first-run wizard: it takes the keyboard from the window under test. No greeter sync: it runs pkexec and
  # writes the test's look into the real greeter of this machine.
  printf '[plugins]\nauto_update = "none"\n\n[shell]\nsetup_wizard_enabled = false\n\n[shell.greeter_sync]\nauto_sync = false\n' \
    > "$RUNTIME/home/.config/andromeda/shell/config.toml"
  # AUTOSTART: TOML string items, for a test that needs a client the compositor spawns itself.
  printf '[general]\nautostart = [%s]\n' "${AUTOSTART:-}" > "$RUNTIME/andromeda.toml"
  # One private bus for the whole session: apps the compositor spawns reach the shell on it, as in a real session.
  dbus-daemon --config-file="$ROOT/tests/e2e/private-bus.conf" --nofork --print-address=3 3> "$RUNTIME/bus.address" &
  for _ in $(seq 100); do [[ -s $RUNTIME/bus.address ]] && break; sleep 0.05; done
  [[ -s $RUNTIME/bus.address ]] || { echo "dbus-daemon did not start" >&2; exit 1; }
  BUS_ADDRESS=$(head -n 1 "$RUNTIME/bus.address")
  # NESTED_PARENT (a parent Wayland socket path): one output as a window on that session, with its GPU and vsync.
  local nested=() session_bus=$BUS_ADDRESS
  [[ -n ${NESTED_PARENT:-} ]] && nested=(WLR_BACKENDS=wayland "WAYLAND_DISPLAY=$NESTED_PARENT")
  # A test that links $RUNTIME/bus to the real user bus gives the compositor the real systemd user manager.
  [[ -S $RUNTIME/bus ]] && session_bus=unix:path=$RUNTIME/bus
  headless_compositor "$RUNTIME" "$1" "$RUNTIME/andromeda.toml" DBUS_SESSION_BUS_ADDRESS="$session_bus" \
    DBUS_SYSTEM_BUS_ADDRESS="$BUS_ADDRESS" XDG_STATE_HOME="$RUNTIME/home/.local/state" \
    ${SLEEP_ON_TEST_BUS:+ANDROMEDA_TEST_SLEEP_HOLD=1} "${nested[@]}" > "$OUT/andromeda.log" 2>&1 &
  # The IPC socket comes after the Wayland socket (server.cpp creates Ipc later); `msg` needs it.
  wait_compositor $! "$RUNTIME/andromeda-wayland-0.sock" 5 || { echo "andromeda did not start" >&2; exit 1; }
}

# rm_runtime: deletes $RUNTIME in a cleanup trap. Helpers the private bus started (thumbnailers, previews) can
# outlive the kill by a moment and write into it, so a failed delete tries again for up to 2 s.
rm_runtime() {
  # A script that sets its own trap still stops the compositor and the clients it started.
  kill $(jobs -p) 2> /dev/null || true
  wait 2> /dev/null
  for _ in $(seq 10); do rm -rf "$RUNTIME" 2> /dev/null && return 0; sleep 0.2; done
  rm -rf "$RUNTIME"
}

# run CMD...: run inside the session. A private HOME keeps Noctalia's theme templates and gsettings writes out of the
# real user config.
run() { in_session command "$@"; }
# spawn CMD...: run CMD inside the session in the background, with $! the PID of CMD itself. A script forks for a
# backgrounded function, so after `run CMD &` the $! is a bash subshell and a kill of it leaves CMD running. CMD is
# then this shell's child: kill -0 and `wait.sh --pid` see it until `wait` reaps it.
spawn() { (in_session exec "$@") & }
in_session() { # command|exec CMD...
  "$1" env -u DISPLAY -u DBUS_SESSION_BUS_ADDRESS -u XDG_CONFIG_HOME -u XDG_DATA_HOME -u XDG_STATE_HOME -u XDG_CACHE_HOME HOME="$RUNTIME/home" \
    XDG_RUNTIME_DIR="$RUNTIME" WAYLAND_DISPLAY=wayland-0 ANDROMEDA_SOCKET="$RUNTIME/andromeda-wayland-0.sock" "${@:2}"
}

# stable_grim [-g GEOMETRY] FILE: shoot once two grabs 0.2 s apart are identical (fades settle). Exported into with_noctalia.
stable_grim() {
  local g=()
  [[ $1 == -g ]] && { g=(-g "$2"); shift 2; }
  for _ in $(seq 25); do
    grim "${g[@]}" "$1.prev"; sleep 0.2; grim "${g[@]}" "$1"
    cmp -s "$1.prev" "$1" && { rm -f "$1.prev"; return 0; }
  done
  echo "FAIL: the screen did not settle for $1"
  exit 1
}
export -f stable_grim

# start_andromeda_system: start andromeda-system on the private system bus (sets $system). Exported into with_noctalia.
export ANDROMEDA_SYSTEM=${ANDROMEDA_SYSTEM:-$ROOT/system/target/debug/andromeda-system}
start_andromeda_system() {
  ANDROMEDA_SYSTEM_TEST=1 "$ANDROMEDA_SYSTEM" > "$OUT/system.log" 2>&1 &
  system=$!
  for _ in $(seq 100); do busctl --address="$DBUS_SYSTEM_BUS_ADDRESS" status org.andromeda.System1 > /dev/null 2>&1 && return 0; sleep 0.1; done
  echo "FAIL: andromeda-system did not own org.andromeda.System1: $(cat "$OUT/system.log")"
  exit 1
}
export -f start_andromeda_system

source "$ROOT/tests/e2e/wait.sh"

# with_noctalia SCRIPT: run a bash script while Noctalia runs on the session's private bus, used as both session and
# system bus so the test shell never reaches the real BlueZ, NetworkManager, or logind. The script sees $NOCTALIA, $OUT,
# $ANDROMEDA, $DESKTOP_CLIENT, and $RUNTIME, and Noctalia is answering IPC when it starts. $BEFORE_NOCTALIA runs on
# that bus before Noctalia starts (for a mock service the shell must find at startup).
with_noctalia() {
  run env NOCTALIA="$NOCTALIA" OUT="$OUT" BEFORE_NOCTALIA="${BEFORE_NOCTALIA:-}" ANDROMEDA="$ANDROMEDA" DESKTOP_CLIENT="$DESKTOP_CLIENT" RUNTIME="$RUNTIME" \
    DBUS_SESSION_BUS_ADDRESS="$BUS_ADDRESS" bash -c '
      set -euo pipefail
      export DBUS_SYSTEM_BUS_ADDRESS=$DBUS_SESSION_BUS_ADDRESS
      trap '"'"'kill $(jobs -p) 2>/dev/null || true; wait'"'"' EXIT
      eval "${BEFORE_NOCTALIA:-}"
      "$NOCTALIA" > "$OUT/noctalia.log" 2>&1 &
      for _ in $(seq 100); do "$NOCTALIA" msg settings-close > /dev/null 2>&1 && break; sleep 0.1; done
      # The bar reserves its edge when it maps and moves windows aside: a window position read before then is stale.
      for _ in $(seq 100); do
        "$ANDROMEDA" layers --json 2> /dev/null | jq -e "[.. | objects | select((.namespace? // \"\") | startswith(\"noctalia-bar\")) | select(.mapped == true)] | length > 0" > /dev/null && break
        sleep 0.05
      done
      eval "$1"
    ' _ "$1"
}
