#!/usr/bin/env bash
# dsk_settings_manager_v1 version 3: a saved value and a layer value wait for commit, then reach subscribers in one
# batch, and the committer hears applied with its generation after it.
set -euo pipefail
readonly D=${ANDROMEDA_DESKTOP_CLIENT:-./build-debug/tests/desktop-client}
readonly LOG=$(mktemp -d)
readonly WAIT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../../.." && pwd)/tools/wait.sh
COMMITTER=
WATCHER=
cleanup() {
  exec 3>&- 2> /dev/null || true
  [[ -n $COMMITTER ]] && kill "$COMMITTER" 2> /dev/null || true
  [[ -n $WATCHER ]] && kill "$WATCHER" 2> /dev/null || true
  rm -rf "$LOG"
}
trap cleanup EXIT

await_line() { # file line
  "$WAIT" --log "$1" --match "^$2\$" --timeout 4 > /dev/null || { echo "never saw '$2' in $1: $(< "$1")"; return 1; }
}
value_of() { "$D" settings-state | grep "^$1=" | cut -d= -f2 | cut -d' ' -f1; }

"$D" settings-watch > "$LOG/watch" &
WATCHER=$!
await_line "$LOG/watch" done

mkfifo "$LOG/in"
"$D" settings-commit 7 appearance.border_width=5 style/appearance.corner_radius=9 < "$LOG/in" > "$LOG/out" 2>&1 &
COMMITTER=$!
exec 3> "$LOG/in"
await_line "$LOG/out" pending

if [[ $(value_of appearance.border_width) == 5 || $(value_of appearance.corner_radius) == 9 ]]; then
  echo "requests applied before their commit: $("$D" settings-state | grep -E '^appearance\.(border_width|corner_radius)=')"
  exit 1
fi

echo commit >&3
await_line "$LOG/out" "applied 7"
[[ $(value_of appearance.border_width) == 5 ]] || { echo "the committed set is not in effect"; exit 1; }
[[ $(value_of appearance.corner_radius) == 9 ]] || { echo "the committed layer value is not in effect"; exit 1; }

# The first batch that carries the saved value carries the layer value too: one reload, not one per request kind.
# A batch ends at the first "done" after it, which no single-line wait can name: poll the watcher's log.
batch=
for _ in $(seq 40); do
  batch=$(awk '/^done$/ { if (hit) { print batch; exit } batch = ""; next } { batch = batch $0 "\n" } /^appearance\.border_width=5 / { hit = 1 }' "$LOG/watch")
  [[ -n $batch ]] && break
  sleep 0.1
done
if ! grep -q '^appearance\.corner_radius=9$' <<< "$batch"; then
  echo "the commit reached subscribers in more than one batch; the saved value's batch:"
  sed 's/^/  | /' <<< "$batch"
  exit 1
fi

echo "a set and a layer value applied together on commit, in one batch, then applied 7"
