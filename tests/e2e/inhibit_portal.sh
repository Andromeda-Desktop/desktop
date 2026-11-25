#!/usr/bin/env bash
# The shell serves org.freedesktop.impl.portal.Inhibit, only to the portal frontend: a direct caller gets
# AccessDenied; an Inhibit call with the idle and suspend flags takes logind idle and sleep block inhibitors (a mock
# login1 on the private system bus records them), Request.Close releases both, and CreateMonitor answers 0 and emits
# StateChanged with session-state 1. Writes calls.txt and signals.txt to $OUT (default ./artifacts/inhibit-portal).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/inhibit-portal}
source "$(dirname "$0")/lib.sh"
boot_headless 1
: > "$OUT/calls.txt"

MOCK="$(cd "$(dirname "$0")" && pwd)/mock_logind.py"
export REQ=/org/freedesktop/portal/desktop/request/1_1/e2e
# logind runs before any user session, so the mock starts before the shell.
BEFORE_NOCTALIA='python3 '"$MOCK"' "$OUT/calls.txt" > "$OUT/mock.log" 2>&1 &
  for _ in $(seq 50); do grep -q ready "$OUT/mock.log" && break; sleep 0.1; done' \
FRONTEND_CALL="$ROOT/tests/e2e/portal_frontend_call.py" \
INHIBIT_ARGS="(objectpath \"$REQ\", \"e2e.app\", \"\", uint32 12, @a{sv} {})" \
MONITOR_ARGS='(objectpath "/org/freedesktop/portal/desktop/request/1_1/m", objectpath "/org/freedesktop/portal/desktop/session/1_1/m", "e2e.app", "")' \
with_noctalia '
  # The impl answers only the frontend: a stand-in owns its name for each call. Args: method [gvariant [path]].
  portal() { python3 -I "$FRONTEND_CALL" org.freedesktop.impl.portal.desktop.andromeda.shell "org.freedesktop.impl.portal.$1" "${@:2}"; }
  wait_line() { for _ in $(seq 30); do grep -qx "$1" "$OUT/calls.txt" && return 0; sleep 0.1; done
    echo "FAIL: logind never saw \"$1\"; calls: $(tr "\n" "," < "$OUT/calls.txt")"; exit 1; }

  denied=$(gdbus call --session --dest org.freedesktop.impl.portal.desktop.andromeda.shell \
    --object-path /org/freedesktop/portal/desktop --method org.freedesktop.impl.portal.Inhibit.Inhibit \
    "$REQ" e2e.app "" 12 "{}" 2>&1 || true)
  [[ $denied == *"portal frontend"* ]] || { echo "FAIL: a direct caller got Inhibit: $denied"; exit 1; }
  portal Inhibit.Inhibit "$INHIBIT_ARGS" > /dev/null
  wait_line "Inhibit idle e2e.app block"
  wait_line "Inhibit sleep e2e.app block"
  grep -q "^Release" "$OUT/calls.txt" && { echo "FAIL: released before Close"; exit 1; }
  portal Request.Close "" "$REQ" > /dev/null
  wait_line "Release idle e2e.app"
  wait_line "Release sleep e2e.app"

  gdbus monitor --session --dest org.freedesktop.impl.portal.desktop.andromeda.shell > "$OUT/signals.txt" &
  sleep 0.3
  response=$(portal Inhibit.CreateMonitor "$MONITOR_ARGS")
  [[ $response == "[0]" ]] || { echo "FAIL: CreateMonitor answered $response"; exit 1; }
  for _ in $(seq 30); do grep -q "StateChanged" "$OUT/signals.txt" && break; sleep 0.1; done
  grep -A1 "StateChanged" "$OUT/signals.txt" | grep -q "session-state.*uint32 1" \
    || { echo "FAIL: no StateChanged running; signals: $(cat "$OUT/signals.txt")"; exit 1; }
  kill %2 %3 2>/dev/null || true
'
cat "$OUT/calls.txt"
echo "PASS; artifacts: $OUT"
