#!/usr/bin/env bash
# harness: watchdog
# systemd kills the session when WATCHDOG=1 stops for WATCHDOG_USEC. A GPU submit stuck for 6 s stops only the render
# thread, so the main loop keeps pinging every WATCHDOG_USEC / 2 (1 s here) through the stall.
set -euo pipefail
readonly NOTIFY_LOG=$ANDROMEDA_RUNTIME_DIR/notify.log

grep -q ' READY=1$' "$NOTIFY_LOG" || { echo "no READY=1 on the notify socket: $(cat "$NOTIFY_LOG")"; exit 1; }
"$ANDROMEDA" stall-submit 6000 > /dev/null
start=$(( $(date +%s%N) / 1000000 ))
timeout 1 "$ANDROMEDA" settle > /dev/null 2>&1 && { echo "the output settled during the stall: no submit was stalled"; exit 1; }
sleep 5.5 # real time: the pings are counted over the 6 s stall itself
grep -q 'GL call while the render thread holds the context' "$ANDROMEDA_LOG" \
  && { echo "a main-thread GL call ran while the context was lent"; exit 1; }

pings=$(awk -v s="$start" '$2 == "WATCHDOG=1" && $1 >= s { print $1 }' "$NOTIFY_LOG")
count=$(wc -l <<< "$pings")
gap=$(awk -v s="$start" 'BEGIN { p = s } { if ($1 - p > g) g = $1 - p; p = $1 } END { print g + 0 }' <<< "$pings")
(( count >= 5 && gap <= 1500 )) \
  || { echo "$count WATCHDOG=1 pings in the 6.5 s stall, largest gap $gap ms; want >= 5 and <= 1500 ms"; exit 1; }
echo "WATCHDOG=1 kept arriving through a 6 s stalled submit: $count pings, largest gap $gap ms"
