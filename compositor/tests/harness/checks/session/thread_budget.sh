#!/usr/bin/env bash
# The compositor's own threads stay inside the budget: each named owner runs at most its listed count, and no thread is
# unnamed (an unnamed thread keeps the process name). Library threads (Mesa "<proc>:<name>", jemalloc, pango) are
# counted only in the total.
set -euo pipefail
declare -A BUDGET=(
  [andro-log]=1 [andro-render]=1 [andro-residency]=1 [andro-icons]=1 [andro-keymap]=1 [andro-activity]=1
  [andro-filewrite]=1 [andro-scanout]=1 [andro-slack]=1 [andromeda-stall]=1 [andromeda-text]=1
  [andro-filedone]=1 [andro-shortcuts]=1 [andro-layerperm]=1 [andro-inputperm]=1
)
readonly TOTAL=32

"$ANDROMEDA_UNMAP_CLIENT" budget 400 300 > "$ANDROMEDA_RUNTIME_DIR/client.log" 2>&1 &
timeout 10 "$ANDROMEDA" settle > /dev/null
"$ANDROMEDA" msg overview-open > /dev/null
timeout 10 "$ANDROMEDA" settle > /dev/null

pid=$ANDROMEDA_SERVER_PID
declare -A seen=()
total=0
for task in /proc/"$pid"/task/*; do
  tid=${task##*/}
  name=$(< "$task/comm")
  total=$((total + 1))
  [[ $tid == "$pid" || $name == *:* || $name != andro* ]] && continue
  seen[$name]=$((${seen[$name]:-0} + 1))
done
for name in "${!seen[@]}"; do
  [[ -n ${BUDGET[$name]:-} ]] || { echo "thread '$name' (x${seen[$name]}) has no budget: name it and list it here"; exit 1; }
  ((seen[$name] <= BUDGET[$name])) || { echo "thread '$name' runs ${seen[$name]} times, budget ${BUDGET[$name]}"; exit 1; }
done
((total <= TOTAL)) || { echo "the compositor runs $total threads, budget $TOTAL"; exit 1; }
echo "$total threads; own: $(for n in "${!seen[@]}"; do printf '%s ' "$n"; done)"
