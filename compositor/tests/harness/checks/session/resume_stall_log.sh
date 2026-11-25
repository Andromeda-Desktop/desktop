#!/usr/bin/env bash
# A resume redraws every shown output; 5 s later the compositor logs whether each one presented a frame. A GPU submit
# stuck across the resume names the output and its frame state, so a black screen after sleep leaves a log line.
set -euo pipefail
source "$ANDROMEDA_HARNESS_LIB"
AWAIT_TRIES=400 AWAIT_STEP=0.025

mark=$(wc -l < "$ANDROMEDA_LOG")
"$ANDROMEDA" sleep-signal resume > /dev/null
await_log_since "$mark" 'resume check: every shown output presented a frame'
grep -q 'no frame presented' "$ANDROMEDA_LOG" && { echo "an idle output was reported stalled after resume"; exit 1; }

mark=$(wc -l < "$ANDROMEDA_LOG")
"$ANDROMEDA" stall-submit 6000 > /dev/null
"$ANDROMEDA" sleep-signal resume > /dev/null
await_log_since "$mark" "no frame presented 5000 ms after resume (frame_state=submitting"
echo "resume check: quiet on a healthy resume, names the stuck output: $(grep -m1 -o "output '[^']*': no frame presented.*" "$ANDROMEDA_LOG")"
