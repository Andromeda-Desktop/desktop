#!/usr/bin/env bash
# A client that asks for dsk_surface_v1 after its toplevel mapped and the compositor settled gets the current presence
# (shown, 0) at once. It commits nothing after the ask, so no presence sync follows: only the lookup at get_surface
# can send it.
set -euo pipefail
source "$ANDROMEDA_HARNESS_LIB"

readonly CLIENT="${ANDROMEDA_BACKGROUND_CLIENT:-./build-debug/tests/background-client}"
readonly LOG="$ANDROMEDA_RUNTIME_DIR/late-dsk.log"
readonly TRIGGER="$ANDROMEDA_RUNTIME_DIR/late-dsk.ask"

LATE_DSK="$TRIGGER" "$CLIENT" late-dsk > "$LOG" 2>&1 &
await_events "$LOG" mapped 1 "the late dsk client"
"$ANDROMEDA" settle
touch "$TRIGGER"
await_lines "$LOG" "presence 0" 1
