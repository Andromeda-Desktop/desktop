#!/usr/bin/env bash
# A hidden window that cannot be suspended (xdg_wm_base below version 6) keeps a slow frame tick while it waits for
# callbacks, and the compositor stops waking for it once it stops asking.
set -euo pipefail
source "$ANDROMEDA_HARNESS_LIB"

readonly CLIENT="${ANDROMEDA_BACKGROUND_CLIENT:-./build-debug/tests/background-client}"
readonly LOG="$ANDROMEDA_RUNTIME_DIR/old-client.log"
readonly STOP="$ANDROMEDA_RUNTIME_DIR/old-client.stop"
rm -f "$STOP"

WM_BASE_VERSION=5 STOP_FILE="$STOP" "$CLIENT" old-client > "$LOG" 2>&1 &
await_events "$LOG" mapped 1
"$ANDROMEDA" settle
"$ANDROMEDA" msg workspace-switch:2 > /dev/null
"$ANDROMEDA" settle

before=$(events "$LOG" frame)
sleep 1 # real time: the background tick runs on a wall-clock timer
hidden=$(($(events "$LOG" frame) - before))
if ((hidden < 5 || hidden > 12)); then
  echo "hidden old client got $hidden frames in 1 s, want about 10"
  exit 1
fi

touch "$STOP"
await_events "$LOG" stopped 1
comp_pid=$ANDROMEDA_SERVER_PID
switches() { awk '/^voluntary_ctxt_switches/ {print $2}' "/proc/$comp_pid/status"; }
wake_before=$(switches)
sleep 2 # real time: count compositor wakeups while nothing asks for frames
wakeups=$(($(switches) - wake_before))
if ((wakeups > 8)); then
  echo "compositor woke $wakeups times in 2 s with only a stopped hidden client"
  exit 1
fi
echo "hidden old client: $hidden frames/s while asking; $wakeups compositor wakeups in 2 s after it stopped"
