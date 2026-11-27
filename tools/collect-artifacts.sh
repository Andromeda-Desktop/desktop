#!/usr/bin/env bash
# Usage: tools/collect-artifacts.sh DEST FILE...
# Copies release artifacts into DEST. Two artifacts may carry the same package (apps and core both carry Link): an
# identical copy is skipped, a different file under the same name fails, so a gate never checks one copy and
# publishes another.
set -euo pipefail
dest=$1
shift
mkdir -p "$dest"
for f in "$@"; do
  t=$dest/${f##*/}
  if [[ -e $t ]]; then
    cmp -s "$f" "$t" || { echo "::error::${f##*/} differs between artifacts ($f)" >&2; exit 1; }
    continue
  fi
  cp "$f" "$t"
done
