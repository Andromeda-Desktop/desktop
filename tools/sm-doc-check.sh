#!/usr/bin/env bash
# Checks every `path:line[-line] (symbol)` reference in docs/state-machines.md:
# the file exists, the lines exist, and the symbol appears within 20 lines.
set -euo pipefail
root=$(git -C "$(dirname "$0")" rev-parse --show-toplevel)
doc=${1:-$root/docs/state-machines.md}
fail=0 n=0
while IFS='|' read -r path a b sym; do
  n=$((n + 1))
  f=$root/$path
  [[ -f $f ]] || f=$root/link/$path
  if [[ ! -f $f ]]; then echo "missing file: $path:$a"; fail=1; continue; fi
  len=$(wc -l < "$f")
  if (( ${b:-$a} > len )); then echo "past end ($len): $path:$a${b:+-$b}"; fail=1; continue; fi
  if [[ -n $sym ]]; then
    sym=${sym##*::} sym=${sym##*.}
    lo=$(( a > 20 ? a - 20 : 1 )) hi=$(( ${b:-$a} + 20 ))
    sed -n "${lo},${hi}p" "$f" | grep -qF -- "$sym" || { echo "symbol '$sym' not near $path:$a"; fail=1; }
  fi
done < <(grep -oP '`?\b[\w./-]+\.(?:cpp|h|hpp|c|rs|kt|py|sh|toml|xml|service):\d+(?:-\d+)?`?(?: \(`?[\w:~<>.]+`?\))?' "$doc" \
  | sed -E 's/`//g; s/^([^:]+):([0-9]+)(-([0-9]+))?( \(([^)]*)\))?$/\1|\2|\4|\6/')
echo "$n refs checked"
exit $fail
