#!/usr/bin/env bash
# Key-to-pty latency and parser backlog (DSR round trip), idle and during a flood of the same pty.
# One wtype per key; its spawn cost is in every number, so compare terminals and idle vs flood, not absolutes.
# Each wtype is a new virtual keyboard (a keymap change), so key loss is counted in a second pass that types all keys
# from one keyboard, as a user does.
# Gate (ours): flood p99 <= max(2 x idle p99, idle p99 + 20 ms) and <= 100 ms; DSR p99 during flood <= 1 s.
# Env: TERMS, KEYS (60), FLOODS (ascii dense_cells).
source "$(dirname "$0")/lib.sh"
TERMS=${TERMS:-ours foot kitty ghostty}
KEYS=${KEYS:-60}
FLOODS=${FLOODS:-ascii dense_cells}
load_boot
ALPHA=abcdefghijklmnopqrstuvwxyz
# measure LABEL FLOODFILE: send KEYS keys, print "p50 p99 lost dsr_p50 dsr_p99" in ms.
measure() {
  local res="$OUT/res/keys-$TID-$1" sent="$OUT/res/sent-$TID-$1"
  rm -f "$res" "$sent"
  term_type "$PROBE keys $res $2"
  sleep 1.5
  for i in $(seq 0 $((KEYS - 1))); do
    ch=${ALPHA:$((i % 26)):1}
    echo "$ch $(date +%s%N)" >> "$sent"
    run wtype -k Shift_L "$ch"
    sleep 0.05
  done
  run wtype -k Shift_L '~'
  "$ROOTDIR/tools/wait.sh" --file "$res" --timeout 60 > /dev/null || { echo "no result"; return; }
  python3 -I - "$sent" "$res" << 'EOF'
import sys
sent = [l.split() for l in open(sys.argv[1])]
got = [l.split() for l in open(sys.argv[2]) if l.strip()]
dsr = sorted(int(v) / 1e6 for k, v in got if k == "dsr")
keys = [(k, int(v)) for k, v in got if k != "dsr"]
lat, j = [], 0
for ch, t in keys:
    while j < len(sent) and sent[j][0] != ch:
        j += 1
    if j < len(sent):
        lat.append((t - int(sent[j][1])) / 1e6)
        j += 1
lat.sort()
p = lambda a, q: a[min(len(a) - 1, int(len(a) * q))] if a else -1
print(f"{p(lat, .5):.1f} {p(lat, .99):.1f} {len(sent) - len(lat)} {p(dsr, .5):.1f} {p(dsr, .99):.1f}")
EOF
}
# lost FLOODFILE: keys lost when one keyboard types 30 keys during the flood.
lost_one_keyboard() {
  local res="$OUT/res/loss-$TID" want=abcdefghijklmnopqrstuvwxyzabcd
  rm -f "$res"
  term_type "$PROBE keys $res $1"
  sleep 1.5
  run wtype -k Shift_L -d 50 "$want~"
  "$ROOTDIR/tools/wait.sh" --file "$res" --timeout 60 > /dev/null || { echo 30; return; }
  local got
  got=$(grep -v '^dsr' "$res" | awk '{printf "%s", $1}')
  echo $((${#want} - ${#got}))
}
declare -A IDLE99
printf '%-8s %-12s %8s %8s %5s %9s %9s\n' term load key_p50 key_p99 lost dsr_p50 dsr_p99
for t in $TERMS; do
  command -v "$t" > /dev/null || [[ $t == ours ]] || { echo "skip $t (not installed)"; continue; }
  term_start "$t" || continue
  read -r p50 p99 lost d50 d99 < <(measure idle "")
  IDLE99[$t]=$p99
  printf '%-8s %-12s %8s %8s %5s %9s %9s\n' "$t" idle "$p50" "$p99" "$lost" - -
  for f in $FLOODS; do
    read -r p50 p99 lost d50 d99 < <(measure "$f" "$CORPUS/$f")
    printf '%-8s %-12s %8s %8s %5s %9s %9s\n' "$t" "$f" "$p50" "$p99" "$lost" "$d50" "$d99"
    if [[ $t == ours ]]; then
      ok_keys() { awk -v f="$1" -v i="$2" 'BEGIN { lim = (2 * i > i + 20) ? 2 * i : i + 20; if (lim > 100) lim = 100; exit !(f >= 0 && f <= lim) }' || { echo "flood p99 $1 ms vs idle p99 $2 ms"; return 1; }; }
      ok_dsr() { awk -v d="$1" 'BEGIN { exit !(d >= 0 && d <= 1000) }' || { echo "DSR p99 $1 ms"; return 1; }; }
      gate "keys during $f" ok_keys "$p99" "${IDLE99[ours]}"
      gate "DSR backlog during $f" ok_dsr "$d99"
      one=$(lost_one_keyboard "$CORPUS/$f")
      echo "  keys lost from one keyboard during $f: $one (new-keyboard pass: $lost)"
      gate "no lost keys during $f" test "$one" -eq 0
    fi
  done
  term_stop
done
echo "FAILS $FAILS"
exit $((FAILS > 0))
