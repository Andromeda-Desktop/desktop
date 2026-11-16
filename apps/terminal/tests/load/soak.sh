#!/usr/bin/env bash
# Soak, ours only: MINUTES of mixed load (corpora in the shown session, 2 background sessions flooding, session churn,
# theme switches). Samples RSS, fds and threads per cycle to $OUT/soak.csv.
# Gates: every cycle's RSS <= cycle 2 RSS + 10 %, fds and threads <= cycle 2; alive, responsive, quiet at the end.
source "$(dirname "$0")/lib.sh"
MINUTES=${MINUTES:-30}
load_boot
key() { run wtype -k Shift_L "$@"; }
term_start ours
for _ in 1 2; do
  key -M ctrl -M shift t -m shift -m ctrl; sleep 0.5
  term_type 'while :; do yes "$(printf %0150d 0)" | head -c 100000000; sleep 5; done'
done
echo "cycle,secs,rss_kb,fds,threads" > "$OUT/soak.csv"
end=$(($(date +%s) + MINUTES * 60))
t0=$(date +%s)
cycle=0
while [[ $(date +%s) -lt $end ]]; do
  cycle=$((cycle + 1))
  for c in ascii sgr unicode cursor alt_screen kitty_images; do
    term_do "$PROBE run $CORPUS/$c $OUT/res/soak" "$OUT/res/soak" 300 || echo "cycle $cycle: $c timed out"
  done
  term_type "printf '\\e_Ga=d\\e\\\\\\e[3J'; clear"
  for _ in $(seq 10); do key -M ctrl -M shift t -m shift -m ctrl; sleep 0.15; key -M ctrl -M shift w -m shift -m ctrl; sleep 0.15; done
  printf '[appearance]\ntheme = "%s"\n[text]\nsize = 13\n' "$( ((cycle % 2)) && echo light || echo dark)" > "$CFG/terminal.toml"
  sleep 2
  alive > /dev/null || { echo "cycle $cycle: $(alive)"; break; }
  echo "$cycle,$(($(date +%s) - t0)),$(rss_kb "$TPID"),$(fds "$TPID"),$(threads "$TPID")" | tee -a "$OUT/soak.csv"
done
bounded() {
  awk -F, 'NR == 3 { r = $3; f = $4; t = $5 } NR > 3 && ($3 > r * 1.10 || $4 > f || $5 > t) {
    printf "cycle %s: rss %d kB fds %d threads %d vs cycle 2 %d/%d/%d\n", $1, $3, $4, $5, r, f, t; bad = 1 }
    END { if (NR < 4) { print "fewer than 3 cycles"; exit 1 } exit bad }' "$OUT/soak.csv"
}
gate "soak bounded RSS, fds, threads" bounded
gate "soak alive" alive
key -M ctrl c -m ctrl
gate "soak responsive" responsive
echo "FAILS $FAILS"
exit $((FAILS > 0))
