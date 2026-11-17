#!/usr/bin/env bash
# An output destroyed while the render thread holds the context frees its frame-log GPU timers without a main-thread
# GL call. Needs ANDROMEDA_FRAME_LOG in the harness environment (bench path); without it the check has nothing to test.
set -euo pipefail
[[ -n ${ANDROMEDA_FRAME_LOG:-} ]] || { echo "SKIP: needs ANDROMEDA_FRAME_LOG"; exit 0; }
"$ANDROMEDA" output-create TIMER-1 800x600@60 > /dev/null
"$ANDROMEDA" settle > /dev/null
"$ANDROMEDA" stall-submit 1500 > /dev/null
"$ANDROMEDA" output-destroy TIMER-1 > /dev/null
timeout 10 "$ANDROMEDA" settle > /dev/null
if grep -E -m1 'GL call while the render thread holds the context|eglMakeCurrent failed' "$ANDROMEDA_LOG"; then
  echo "destroying an output during a stalled submit ran GL on the main thread"; exit 1
fi
echo "an output destroyed during a stalled submit freed its timers without main-thread GL"
