#!/usr/bin/env bash
# Under sustained disk write load, typical desktop use (terminal output, clipboard copies and pastes, panels,
# notifications) never blocks the compositor or shell main loop. The session HOME lives on the disk the load hits,
# not in /tmp, so state writes see the same writeback as on a real desktop. Fails on any main-loop stall line in
# andromeda.log or noctalia.log. LOAD_DIR (default /var/tmp) must be a disk filesystem; DURATION seconds (default 60);
# LOAD_WRITERS (3) writers of LOAD_MB (512) MiB each.
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/disk-load-stalls}
DURATION=${DURATION:-60}
source "$(dirname "$0")/lib.sh"
DISK=$(mktemp -d "${LOAD_DIR:-/var/tmp}/disk-load.XXXX")
trap 'kill $(jobs -p) 2>/dev/null || true; wait 2>/dev/null; rm -rf "$RUNTIME" "$DISK"' EXIT
[[ $(stat -f -c %T "$DISK") != tmpfs ]] || { echo "FAIL: LOAD_DIR is tmpfs; the test needs a disk"; exit 1; }
mkdir -p "$DISK/home"
ln -s "$DISK/home" "$RUNTIME/home"
boot_headless 1

# Pressure: LOAD_WRITERS parallel dd writers (fdatasync each pass) plus one busy loop per CPU, as during a build.
end=$((SECONDS + DURATION))
loads=()
for w in $(seq "${LOAD_WRITERS:-3}"); do
  (while ((SECONDS < end)); do
    dd if=/dev/zero of="$DISK/load-$w.bin" bs=1M count="${LOAD_MB:-512}" conv=fdatasync status=none
  done) &
  loads+=($!)
done
for _ in $(seq "$(nproc)"); do (while ((SECONDS < end)); do :; done) & loads+=($!); done

DURATION=$DURATION with_noctalia '
  foot -- sh -c "while :; do date +%N; sleep 0.016; done" > /dev/null 2>&1 &
  end=$((SECONDS + DURATION))
  i=0
  while ((SECONDS < end)); do
    i=$((i + 1))
    printf "copy %s %s\n" "$i" "$(date +%N)" | timeout 5 wl-copy
    timeout 5 wl-paste -n > /dev/null
    for panel in clipboard control-center launcher; do
      "$NOCTALIA" msg panel-toggle $panel > /dev/null; sleep 0.3
      "$NOCTALIA" msg panel-toggle $panel > /dev/null
    done
    notify-send -a disk-load "load $i $RANDOM" body
    sleep 0.3
  done
  echo "actions: $i"
'
wait "${loads[@]}"

stalls=$(grep -hE 'main loop blocked|stall [0-9]+ ms, report|took [0-9]{3,}(\.[0-9]+)? ?ms' \
  "$OUT/andromeda.log" "$OUT/noctalia.log" || true)
if [[ -n $stalls ]]; then
  echo "FAIL: main-loop stalls under disk load:"
  printf '%s\n' "$stalls" | cut -c1-400
  exit 1
fi
echo "PASS; artifacts: $OUT"
