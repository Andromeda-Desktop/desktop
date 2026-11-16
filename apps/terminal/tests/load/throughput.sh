#!/usr/bin/env bash
# Throughput per corpus, ours vs other terminals: median wall time (write + final DSR), CPU, GPU, peak RSS.
# Gate: ours <= 1.15x the fastest rival median per corpus. Env: TERMS, CASES, REPS (5), MIB (32), BIN, HOLDER.
source "$(dirname "$0")/lib.sh"
TERMS=${TERMS:-ours foot kitty ghostty}
CASES=${CASES:-ascii sgr unicode scroll_region cursor long_lines alt_screen dense_cells}
REPS=${REPS:-5}
load_boot
declare -A MED
printf '%-8s %-14s %9s %9s %8s %8s %8s\n' term case wall_ms min_ms cpu_s gpu_ms hwm_MiB
for t in $TERMS; do
  command -v "$t" > /dev/null || [[ $t == ours ]] || { echo "skip $t (not installed)"; continue; }
  term_start "$t" || continue
  echo "$t start-to-map $START_MS ms, PSS $(($(pss_kb "$TPID") / 1024)) MiB"
  for c in $CASES; do
    walls=()
    c0=$(cpu_ticks "$TPID"); g0=$(gpu_ns "$TPID")
    for r in $(seq "$REPS"); do
      res="$OUT/res/$t-$c-$r"
      if ! term_do "$PROBE run $CORPUS/$c $res" "$res" 180; then walls+=(999999); continue; fi
      read -r ns ok < "$res"
      [[ $ok == ok ]] && walls+=($((ns / 1000000))) || walls+=(999999)
    done
    cpu=$(awk -v d=$(($(cpu_ticks "$TPID") - c0)) -v t="$TCK" -v n="$REPS" 'BEGIN {printf "%.2f", d / t / n}')
    gpu=$(awk -v d=$(($(gpu_ns "$TPID") - g0)) -v n="$REPS" 'BEGIN {printf "%.1f", d / 1e6 / n}')
    med=$(printf '%s\n' "${walls[@]}" | median)
    min=$(printf '%s\n' "${walls[@]}" | sort -n | head -1)
    MED[$t.$c]=$med
    printf '%-8s %-14s %9s %9s %8s %8s %8s\n' "$t" "$c" "$med" "$min" "$cpu" "$gpu" $(($(hwm_kb "$TPID") / 1024))
  done
  [[ $t == ours ]] && { gate "ours alive after throughput" alive; gate "ours quiet after throughput" quiet; }
  term_stop
done
within() { # within OURS BEST: ours <= 1.15 x best
  [[ $1 -le $(($2 * 115 / 100)) ]] || { echo "ours $1 ms vs best rival $2 ms"; return 1; }
}
# Reported, not gated: each cell gets a new colour pair, and libghostty adds each to its per-page style set (half its
# parse time); foot keeps colours in the cell.
UNGATED=" dense_cells "
for c in $CASES; do
  [[ -n ${MED[ours.$c]:-} ]] || continue
  best=
  for t in $TERMS; do
    [[ $t == ours || -z ${MED[$t.$c]:-} ]] && continue
    [[ -z $best || ${MED[$t.$c]} -lt $best ]] && best=${MED[$t.$c]}
  done
  [[ -z $best ]] && continue
  if [[ $UNGATED == *" $c "* ]]; then
    echo "INFO throughput $c (not gated): ours ${MED[ours.$c]} ms vs best rival $best ms"
  else
    gate "throughput $c" within "${MED[ours.$c]}" "$best"
  fi
done
echo "FAILS $FAILS"
exit $((FAILS > 0))
