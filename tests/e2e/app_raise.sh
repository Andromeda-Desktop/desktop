#!/usr/bin/env bash
# A second open of a running single-instance app raises its window. Fails when:
#   - a second `spawn andromeda-files` (dock, launcher) leaves Viewer focused,
#   - a second `spawn andromeda-viewer FILE` (Files "Open") leaves Files focused,
#   - with focus_on_activate off, a dock click (layer surface, token with the click serial and no surface) followed by
#     a second Files launch leaves Viewer focused.
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/app-raise}
source "$(dirname "$0")/lib.sh"
FILES=${FILES:-$ROOT/apps/files/build/andromeda-files}
VIEWER=${VIEWER:-$ROOT/apps/viewer/build/andromeda-viewer}
boot_headless 1
trap rm_runtime EXIT
fail() { echo "FAIL: $*"; exit 1; }
PDF=$RUNTIME/home/sample.pdf
python3 -I -c 'import cairo,sys; s=cairo.PDFSurface(sys.argv[1],595,842); cairo.Context(s).show_page(); s.finish()' "$PDF"
run "$ANDROMEDA" log-level debug > /dev/null

focused() { run "$ANDROMEDA" focused-window 2> /dev/null; }
wait_focus() {
  for _ in $(seq 100); do focused | grep -q "$1" && return 0; sleep 0.05; done
  return 1
}
spawn() { run env DBUS_SESSION_BUS_ADDRESS="$BUS_ADDRESS" ANDROMEDA_LOG_LEVEL=debug "$ANDROMEDA" msg spawn "$@" > /dev/null; }

# Each app takes focus from the other: a raise must move focus back.
spawn "$FILES"
wait_focus dev.andromeda.Files || fail "Files did not open"
spawn "$VIEWER" "$PDF"
wait_focus dev.andromeda.Viewer || fail "Viewer did not open"
spawn "$FILES"
wait_focus dev.andromeda.Files || { focused; fail "a second Files launch did not raise its window"; }
spawn "$VIEWER" "$PDF"
wait_focus dev.andromeda.Viewer || { focused; fail "a second Viewer open did not raise its window"; }

# The dock path, as on a desktop with focus_on_activate = false: only trusted tokens raise.
run "$ANDROMEDA" config set general.focus_on_activate false > /dev/null
run "$ROOT/compositor/build-debug/tests/layer-client" HEADLESS-1 40 token-on-click > "$OUT/dock.log" 2>&1 &
for _ in $(seq 100); do run "$ANDROMEDA" layers 2> /dev/null | grep -q . && break; sleep 0.05; done
run "$ROOT/compositor/build-debug/tests/pointer-client" 1280 720 move 640 20 click 272 > /dev/null
for _ in $(seq 100); do grep -q '^token ' "$OUT/dock.log" && break; sleep 0.05; done
TOKEN=$(sed -n 's/^token //p' "$OUT/dock.log" | tail -n 1)
[[ -n $TOKEN ]] || fail "the dock click gave no token"
run env DBUS_SESSION_BUS_ADDRESS="$BUS_ADDRESS" XDG_ACTIVATION_TOKEN="$TOKEN" "$FILES" > "$OUT/files-dock.log" 2>&1
wait_focus dev.andromeda.Files || { focused; fail "a dock launch of the running Files did not raise it"; }
echo PASS
