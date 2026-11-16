#!/usr/bin/env bash
# Activity window cost at worst-case data (rulebook §16): N mock app units with 5 real sleep processes each in a
# fake cgroup tree, CPU time growing on every unit. Opens Activity for $SECS s, then closes it.
# Prints main-thread ms per tick ("live", "details"), rebuild ms, frame draw ms, and shell PSS before, open, after.
# Usage: [PERF=1] tools/activity-bench.sh [units=300] [secs=30]. Output in $OUT (default ./artifacts/activity-bench).
set -euo pipefail
N=${1:-300} SECS=${2:-30}
OUT=${OUT:-$(pwd)/artifacts/activity-bench}
source "$(dirname "$0")/../tests/e2e/lib.sh"
trap 'kill $(jobs -p) 2>/dev/null || true; pkill -P $$ sleep 2>/dev/null || true; wait 2>/dev/null; rm_runtime' EXIT
boot_headless 1
printf '\n[output.HEADLESS-1]\nmode = "1720x1000@60"\n[[window_rule]]\nmatch.app_id = "^dev.andromeda.Activity$"\ndefault_floating = true\ndefault_floating_size_px = { width = 1100, height = 720 }\n' >> "$RUNTIME/andromeda.toml"
CG=$RUNTIME/cgroup
BASE=$CG/user.slice/user-1000.slice/user@1000.service/app.slice
UNITS=()
for i in $(seq "$N"); do
  u="app-bench$i@$i.service" d=$BASE/app-bench$i@$i.service
  mkdir -p "$d"
  printf 'usage_usec %s\nuser_usec 1\nsystem_usec 1\n' $((i * 1000000)) > "$d/cpu.stat"
  echo $((i * 1024 * 1024)) > "$d/memory.current"
  echo 'some avg10=0.00 avg60=0.00 avg300=0.00 total=1' > "$d/memory.pressure"
  echo 5 > "$d/pids.current"
  echo '8:0 rbytes=0 wbytes=0 rios=1 wios=1 dbytes=0 dios=0' > "$d/io.stat"
  for _ in 1 2 3 4 5; do sleep 3600 & echo $! >> "$d/cgroup.procs"; done
  UNITS+=("$u")
done
( while :; do
    for i in $(seq "$N"); do f=$BASE/app-bench$i@$i.service/cpu.stat; read -r _ now < "$f"
      printf 'usage_usec %s\nuser_usec 1\nsystem_usec 1\n' $((now + i * 1000)) > "$f"; done
    sleep 1
  done ) &
export POINTER=$ROOT/compositor/build-debug/tests/pointer-client ANDROMEDA_CGROUP_ROOT=$CG ANDROMEDA_LOG_LEVEL=debug ANDROMEDA_PROFILE=1 UNITS_LIST="${UNITS[*]}" SECS
MOCK="$(cd "$(dirname "$0")/../tests/e2e" && pwd)/mock_systemd1.py"
BEFORE_NOCTALIA='python3 '"$MOCK"' "$OUT/calls.txt" > "$OUT/mock.log" 2>&1 &
  for _ in $(seq 50); do grep -q ready "$OUT/mock.log" && break; sleep 0.1; done'
with_noctalia '
  call() { dbus-send --session --print-reply --dest=org.freedesktop.systemd1 /org/freedesktop/systemd1 "org.andromeda.MockSystemd.$@" > /dev/null; }
  pss() { awk "/^Pss:/{print \$2}" /proc/$(pgrep -n -f "$NOCTALIA")/smaps_rollup; }
  "$ANDROMEDA" msg config-reload > /dev/null
  for u in $UNITS_LIST; do call Start string:"$u" string:app.slice; done
  sleep 3
  echo "pss_before_kb $(pss)" > "$OUT/pss.txt"
  : > "$OUT/mark"; t0=$(date +%s%N)
  "$NOCTALIA" msg activity-open "" > /dev/null
  for _ in $(seq 100); do "$NOCTALIA" msg activity-dump | jq -e ".rows | length > 0" > /dev/null 2>&1 && break; sleep 0.1; done
  echo "first_data_ms $(( ($(date +%s%N) - t0) / 1000000 ))" >> "$OUT/pss.txt"
  if [[ -n ${PERF:-} ]]; then # PERF=1: call-graph profile of the shell for 12 s while the window is open
    perf record -g -F 2000 -o "$OUT/perf.data" -p "$(pgrep -n -f "$NOCTALIA")" -- sleep 12 > /dev/null 2>&1
  fi
  sleep "$SECS"
  echo "pss_open_kb $(pss)" >> "$OUT/pss.txt"
  "$POINTER" 1720 1000 keyboard-only tap 119 pause 100 mod control tap 17 mod none > /dev/null # Ctrl+W closes Activity
  sleep 10
  echo "pss_closed_kb $(pss)" >> "$OUT/pss.txt"
'
stats() { # label: p50/p99/max of the values after "label " (or "label=") in the shell log
  grep -oP "$1[ =]\K[0-9.]+" "$OUT/noctalia.log" | sort -n | awk -v l="$1" '{a[NR]=$1} END {if (!NR) {print l": none"; exit}
    printf "%s n=%d p50=%.2f p99=%.2f max=%.2f ms\n", l, NR, a[(int(NR*.5)+1>NR)?NR:int(NR*.5)+1], a[(int(NR*.99)+1>NR)?NR:int(NR*.99)+1], a[NR]}'
}
{ cat "$OUT/pss.txt"; stats live; stats details; stats rebuild; stats draw_ms; } | tee "$OUT/summary.txt"
