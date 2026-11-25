#!/usr/bin/env bash
# harness: outputs=1
# Compositor1.Apps `Activity` follows the session: "locked" while a lock client holds it, "active" after the unlock,
# and `Idle` is true while locked. Runs one private instance on the user bus, where the property lives.
set -euo pipefail

readonly LOCK_CLIENT=${ANDROMEDA_LOCK_CLIENT:-./build-debug/tests/lock-client}
readonly USER_BUS=/run/user/$(id -u)/bus
if [[ ! -S $USER_BUS ]]; then
  echo "SKIP: needs the user bus"
  exit 0
fi

RUNTIME_DIR=$(mktemp -d /tmp/sab.XXXXXXXX)
SERVER_PID=
cleanup() {
  [[ -n ${LOCK_PID:-} ]] && kill "$LOCK_PID" 2> /dev/null || true
  if [[ -n $SERVER_PID ]] && kill -0 "$SERVER_PID" 2> /dev/null; then
    kill -KILL "$SERVER_PID" 2> /dev/null || true
    wait "$SERVER_PID" 2> /dev/null || true
  fi
  rm -rf "$RUNTIME_DIR"
}
trap cleanup EXIT

readonly CONFIG=$RUNTIME_DIR/config.toml
readonly SOCKET=$RUNTIME_DIR/andromeda-wayland-0.sock
readonly BUS_NAME=org.andromeda.Compositor1.Activity$$
ln -s "$USER_BUS" "$RUNTIME_DIR/bus"
cat > "$CONFIG" << 'EOF'
[general]
xwayland = false
autostart = []
EOF

env -u WAYLAND_DISPLAY -u DISPLAY -u DBUS_SESSION_BUS_ADDRESS \
  XDG_RUNTIME_DIR="$RUNTIME_DIR" \
  ANDROMEDA_COMPOSITOR_BUS_NAME="$BUS_NAME" \
  WLR_BACKENDS=headless \
  WLR_LIBINPUT_NO_DEVICES=1 \
  WLR_HEADLESS_OUTPUTS=1 \
  "$ANDROMEDA" -c "$CONFIG" --trust-binary "$ANDROMEDA_REPO/" --trust-binary /usr/bin/ > "$RUNTIME_DIR/compositor.log" 2>&1 &
SERVER_PID=$!
prop() {
  busctl --address="unix:path=$USER_BUS" get-property "$BUS_NAME" /org/andromeda/Compositor1/Apps \
    org.andromeda.Compositor1.Apps "$1" 2> /dev/null
}
for _ in $(seq 40); do
  [[ -S $SOCKET ]] && [[ -n $(prop Activity) ]] && break
  sleep 0.25
done
if [[ $(prop Activity) != 's "active"' ]]; then
  echo "the private compositor never exposed Activity \"active\": '$(prop Activity)'"
  sed 's/^/  | /' "$RUNTIME_DIR/compositor.log"
  exit 1
fi

readonly LOCK_LOG=$RUNTIME_DIR/lock-client.log
readonly LOCK_FIFO=$RUNTIME_DIR/lock-control
mkfifo "$LOCK_FIFO"
exec {lock_fd}<> "$LOCK_FIFO"
XDG_RUNTIME_DIR=$RUNTIME_DIR WAYLAND_DISPLAY=wayland-0 "$LOCK_CLIENT" <&"$lock_fd" > "$LOCK_LOG" 2>&1 &
LOCK_PID=$!
for _ in $(seq 100); do
  grep -q '^locked$' "$LOCK_LOG" && break
  sleep 0.05
done
grep -q '^locked$' "$LOCK_LOG" || { echo "the session never locked: $(cat "$LOCK_LOG")"; exit 1; }
[[ $(prop Activity) == 's "locked"' ]] || { echo "Activity is '$(prop Activity)' while locked"; exit 1; }
[[ $(prop Idle) == 'b true' ]] || { echo "Idle is '$(prop Idle)' while locked"; exit 1; }

echo unlock >&"$lock_fd"
for _ in $(seq 100); do
  grep -q '^unlocked$' "$LOCK_LOG" && break
  sleep 0.05
done
grep -q '^unlocked$' "$LOCK_LOG" || { echo "the session never unlocked: $(cat "$LOCK_LOG")"; exit 1; }
[[ $(prop Activity) == 's "active"' ]] || { echo "Activity is '$(prop Activity)' after the unlock"; exit 1; }
[[ $(prop Idle) == 'b false' ]] || { echo "Idle is '$(prop Idle)' after the unlock"; exit 1; }

echo "Activity went active -> locked -> active with the lock client; Idle was true only while locked"
