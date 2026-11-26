#!/usr/bin/env bash
# harness: outputs=2
# A successful wlr-output-management apply is written to displays.toml beside the config, and a later session that
# includes that file starts with the same arrangement. The restart runs on one private instance with the harness's
# containment.
set -euo pipefail
source "$ANDROMEDA_REPO/../tests/headless.sh"

readonly OUTPUT_MANAGEMENT=${ANDROMEDA_OUTPUT_MANAGEMENT_CLIENT:-./build-debug/tests/output-management-client}
readonly SAVED=$(dirname "$ANDROMEDA_CONFIG")/displays.toml
# As the default config does: the compositor saves display state only into an included displays.toml.
printf '\n[include.optional]\nfiles = ["displays.toml"]\n' >> "$ANDROMEDA_CONFIG"
"$ANDROMEDA" msg config-reload > /dev/null

"$OUTPUT_MANAGEMENT" apply enable HEADLESS-2 0 720 > /dev/null
for _ in $(seq 40); do
  [[ -f $SAVED ]] && break
  sleep 0.1
done
if [[ ! -f $SAVED ]]; then
  echo "no displays.toml after a successful apply"
  exit 1
fi
if ! grep -q '^\[output.HEADLESS-2\]' "$SAVED" || ! grep -qE '^position = \[ ?0, 720 ?\]' "$SAVED"; then
  echo "displays.toml does not hold HEADLESS-2 at 0,720:"
  sed 's/^/  | /' "$SAVED"
  exit 1
fi

RUNTIME_DIR=$(mktemp -d /tmp/ums.XXXXXXXX)
SERVER_PID=
cleanup() {
  if [[ -n $SERVER_PID ]] && kill -0 "$SERVER_PID" 2>/dev/null; then
    kill -KILL "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
  fi
  rm -rf "$RUNTIME_DIR"
}
trap cleanup EXIT

CONFIG=$RUNTIME_DIR/config.toml
SOCKET=$RUNTIME_DIR/andromeda-wayland-0.sock
cp "$SAVED" "$RUNTIME_DIR/displays.toml"
cat > "$CONFIG" << 'EOF'
[include.optional]
files = ["displays.toml"]

[general]
xwayland = false
autostart = []
EOF

headless_compositor "$RUNTIME_DIR" 2 "$CONFIG" -uDBUS_SESSION_BUS_ADDRESS \
  > "$RUNTIME_DIR/compositor.log" 2>&1 &
SERVER_PID=$!
if ! wait_compositor "$SERVER_PID" "$SOCKET"; then
  echo "the private compositor never exposed its IPC socket"
  sed 's/^/  | /' "$RUNTIME_DIR/compositor.log"
  exit 1
fi

position=
for _ in $(seq 40); do
  position=$(
    ANDROMEDA_SOCKET=$SOCKET XDG_RUNTIME_DIR=$RUNTIME_DIR WAYLAND_DISPLAY=wayland-0 "$ANDROMEDA" outputs --json \
      | jq -r '.[] | select(.name == "HEADLESS-2") | "\(.position.x),\(.position.y)"'
  )
  [[ $position == "0,720" ]] && break
  sleep 0.1
done
if [[ $position != "0,720" ]]; then
  echo "a session including displays.toml placed HEADLESS-2 at '$position', expected 0,720"
  exit 1
fi

echo "output-management apply saved to displays.toml and restored on the next session"
