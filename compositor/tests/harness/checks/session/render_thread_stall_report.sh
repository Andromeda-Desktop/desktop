#!/usr/bin/env bash
# harness: render_thread
# A GPU submit stuck 250 ms or more on the render thread writes a stall report of its own with developer diagnostics
# on: component andromeda-render, the render thread's samples and user stack. The main loop does not stall, so it
# writes none.
set -euo pipefail
readonly STALLS="$ANDROMEDA_RUNTIME_DIR/xdg-state/andromeda/stalls"

sed -i 's/^\[general\]$/[general]\ndeveloper_diagnostics = true/' "$ANDROMEDA_CONFIG"
"$ANDROMEDA" msg config-reload > /dev/null
"$ANDROMEDA" stall-submit 400 > /dev/null
for _ in $(seq 40); do
  compgen -G "$STALLS/*.txt" > /dev/null && break
  sleep 0.1
done
report=$(compgen -G "$STALLS/*.txt" | head -n 1) || { echo "no stall report for a 400 ms render-thread job"; exit 1; }
sleep 0.3 # real time: a second report, if any, is written on its watch thread right after its pass ends
count=$(compgen -G "$STALLS/*.txt" | wc -l)
(( count == 1 )) || { echo "$count stall reports, want one: $(grep -h '^component' "$STALLS"/*.txt | tr '\n' ' ')"; exit 1; }
grep -q '^component: andromeda-render$' "$report" || { echo "report lacks the render component: $(head -n 3 "$report")"; exit 1; }
duration=$(sed -n 's/^duration_ms: //p' "$report")
(( duration >= 400 && duration < 1000 )) || { echo "report duration $duration ms, want 400-999"; exit 1; }
grep -q 'wchan=hrtimer_nanosleep\|wchan=do_nanosleep' "$report" || { echo "report lacks the sleep sample: $(grep samples "$report")"; exit 1; }
sed -n '/^user_stack:/,/^kernel_stacks:/p' "$report" | grep -q 'nanosleep' \
  || { echo "user stack lacks the stalled job: $(sed -n '/^user_stack:/,/^kernel_stacks:/p' "$report" | head -n 12)"; exit 1; }
echo "a 400 ms render-thread job writes one andromeda-render report with its samples and stack"
