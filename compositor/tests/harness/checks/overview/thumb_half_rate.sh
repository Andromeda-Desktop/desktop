#!/usr/bin/env bash
# In the overview, two windows that redraw on every frame callback keep redrawing, and never get their callbacks on
# the same output frame: each thumbnail's buffer upload falls on a different frame.
set -euo pipefail
source "$ANDROMEDA_HARNESS_LIB"
readonly BACKGROUND_CLIENT="${ANDROMEDA_BACKGROUND_CLIENT:-./build-debug/tests/background-client}"
readonly A_LOG="$ANDROMEDA_RUNTIME_DIR/thumb-a.log" B_LOG="$ANDROMEDA_RUNTIME_DIR/thumb-b.log"

"$BACKGROUND_CLIENT" thumb-a > "$A_LOG" 2>&1 &
"$BACKGROUND_CLIENT" thumb-b > "$B_LOG" 2>&1 &
await_events "$A_LOG" mapped 1 thumb-a
await_events "$B_LOG" mapped 1 thumb-b
"$ANDROMEDA" msg overview-open > /dev/null
"$ANDROMEDA" settle > /dev/null

a0=$(events "$A_LOG" frame) b0=$(events "$B_LOG" frame)
await_lines "$A_LOG" frame $((a0 + 30))
await_lines "$B_LOG" frame $((b0 + 30))
times() { grep '^frame ' "$1" | tail -n +"$(($2 + 1))" | cut -d' ' -f2 | sort -u; }
shared=$(comm -12 <(times "$A_LOG" "$a0") <(times "$B_LOG" "$b0") | wc -l)
if ((shared > 0)); then
  echo "both thumbnail clients got their frame callback on $shared of the same output frames"
  exit 1
fi
echo "thumbnail clients redraw on alternate frames: $(($(events "$A_LOG" frame) - a0)) and $(($(events "$B_LOG" frame) - b0)) callbacks"
