#!/usr/bin/env bash
# Hostile input and churn, ours only. After every case: the app runs, its log has no crash line, a DSR round trip
# answers within 2 s. At the end: 0 idle CPU. Env: CASES (all below), BIN (build-asan binary works), HOLDER=1 adds
# the holder-kill case.
source "$(dirname "$0")/lib.sh"
CASES=${CASES:-vtfuzz urandom osc52 titles links kitty_images alt_screen single_line resize_storm session_churn many_floods paste holder_kill}
load_boot
key() { run wtype -k Shift_L "$@"; }
term_start ours
run_corpus() { term_do "$PROBE run $CORPUS/$1 $OUT/res/chaos-$1" "$OUT/res/chaos-$1" "${2:-300}"; }
# during CMD -- ACTION...: type CMD (a probe run writing $OUT/res/during), repeat ACTION while it runs.
during() {
  local cmd=$1 res="$OUT/res/during"
  shift
  rm -f "$res"
  term_type "$cmd $res"
  for _ in $(seq 600); do
    [[ -s $res ]] && return 0
    kill -0 "$TPID" 2> /dev/null || { echo "app died"; return 1; }
    "$@"
  done
  "$ROOTDIR/tools/wait.sh" --file "$res" --timeout 120 > /dev/null
}
storm_step() {
  run "$ANDROMEDA" msg window-toggle-maximize > /dev/null
  run "$ANDROMEDA" msg window-modify-width-right:0.07 > /dev/null || true
  sleep 0.05
}
# Close, then open while the closed row still animates out of the sidebar.
churn_step() {
  key -M ctrl -M shift t -m shift -m ctrl
  sleep 0.3
  key -M ctrl -M shift w -m shift -m ctrl -M ctrl -M shift t -m shift -m ctrl
  sleep 0.05
  key -M ctrl -M shift w -m shift -m ctrl
  sleep 0.2
}
for c in $CASES; do
  case $c in
    vtfuzz | urandom | osc52 | titles | links | kitty_images | alt_screen)
      gate "$c finishes" run_corpus "$c" ;;
    single_line)
      gate "100 MB single line" term_do "$PROBE stream single_line 100; $PROBE dsr $OUT/res/sl" "$OUT/res/sl" 300 ;;
    resize_storm)
      gate "resize storm during flood" during "$PROBE run $CORPUS/long_lines" storm_step ;;
    session_churn)
      gate "session churn during flood" during "$PROBE run $CORPUS/sgr" churn_step ;;
    many_floods)
      for _ in 1 2 3; do key -M ctrl -M shift t -m shift -m ctrl; sleep 0.5; term_type 'yes "$(printf %0200d 0)"'; done
      sleep 10
      for _ in 1 2 3; do key -M ctrl -M shift w -m shift -m ctrl; sleep 0.5; key -k Tab -k Tab -k Return; sleep 0.5; done
      gate "4 sessions flooding for 10 s" responsive ;;
    paste)
      head -c 50000000 "$CORPUS/ascii" > "$OUT/res/paste.src"
      want=$(stat -c %s "$OUT/res/paste.src")
      run wl-copy < "$OUT/res/paste.src" &
      sleep 1
      rm -f "$OUT/res/paste.out"
      term_type "stty -icanon; cat > $OUT/res/paste.out"
      sleep 0.5
      t0=$(date +%s)
      key -M ctrl -M shift v -m shift -m ctrl
      pasted() {
        for _ in $(seq 1200); do
          [[ $(stat -c %s "$OUT/res/paste.out" 2> /dev/null || echo 0) -ge $want ]] && return 0
          sleep 0.1
        done
        echo "pasted $(stat -c %s "$OUT/res/paste.out" 2> /dev/null || echo 0) of $want bytes"
        return 1
      }
      gate "$((want / 1000000)) MB paste arrives within 120 s" pasted
      echo "  paste took $(($(date +%s) - t0)) s"
      key -M ctrl c -m ctrl; sleep 0.5; term_type 'stty sane' ;;
    holder_kill)
      [[ ${HOLDER:-0} == 1 ]] || { echo "skip holder_kill (HOLDER=0)"; continue; }
      term_type "yes"
      sleep 2
      kill -KILL "$HOLDER_PID"
      sleep 2
      key -M ctrl -M shift t -m shift -m ctrl
      sleep 1 ;;
  esac
  gate "$c: alive" alive
  gate "$c: responsive" responsive
done
gate "quiet after chaos" quiet
echo "FAILS $FAILS"
exit $((FAILS > 0))
