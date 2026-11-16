#!/usr/bin/env bash
# Scrolls Files through a big folder in a headless session and prints the Files CPU time for the scroll and the
# DESK_FRAME_STATS lines. Usage: tools/files-scroll-bench.sh <files-binary> <folder> [runs]; logs in $OUT.
set -euo pipefail
OUT=${OUT:-/tmp/files-scroll-bench}
source "$(dirname "$0")/../tests/e2e/lib.sh"
FILES=$(realpath "$1")
DIR=$(realpath "$2")
RUNS=${3:-3}
POINTER=$ROOT/compositor/build-debug/tests/pointer-client
boot_headless 1
printf '\n[output.HEADLESS-1]\nmode = "1920x1080@60"\nscale = 1\n' >> "$RUNTIME/andromeda.toml"
export FILES DIR RUNS POINTER
with_noctalia '
  "$ANDROMEDA" msg config-reload > /dev/null
  "$NOCTALIA" msg config-set dock.enabled false > /dev/null || true
  ticks() { awk "{print \$14 + \$15}" "/proc/$1/stat"; }
  hz=$(getconf CLK_TCK)
  # 400 finger steps of 30 px, one per 16 ms: about 12000 px, so new rows (new strings) every frame.
  steps=""
  for _ in $(seq 400); do steps+=" axis vertical 30 pause 16"; done
  for r in $(seq "$RUNS"); do
    DESK_FRAME_STATS=1 LC_ALL=C.UTF-8 "$FILES" "$DIR" > "$OUT/files-$r.log" 2>&1 &
    pid=$!
    for _ in $(seq 200); do grep -q "first frame" "$OUT/files-$r.log" 2> /dev/null && break; sleep 0.05; done
    "$ANDROMEDA" settle > /dev/null
    "$POINTER" 1920 1080 keyboard-only tap 119 pause 100 mod control tap 3 mod none > /dev/null
    sleep 1
    "$ANDROMEDA" settle > /dev/null
    "$POINTER" 1920 1080 move 960 600 pause 200 > /dev/null
    before=$(ticks $pid)
    start=$(date +%s.%N)
    "$POINTER" 1920 1080 move 960 600 $steps axis-stop vertical > /dev/null
    "$ANDROMEDA" settle > /dev/null
    sleep 0.5
    after=$(ticks $pid)
    grim "$OUT/end-$r.png"
    end=$(date +%s.%N)
    echo "run $r cpu_s $(echo "scale=3; ($after - $before) / $hz" | bc) wall_s $(echo "scale=2; $end - $start" | bc)"
    grep -E "p99_cpu_ms" "$OUT/files-$r.log" | tail -n 3 || true
    kill $pid; wait $pid 2> /dev/null || true
  done
'
