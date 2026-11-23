#!/usr/bin/env bash
# A window under an opaque fullscreen window is occluded: its dsk_surface_v1.presence goes shown (0) -> occluded (1)
# -> shown, it is suspended while covered and resumes when the fullscreen window leaves fullscreen.
set -euo pipefail
source "$ANDROMEDA_HARNESS_LIB"

readonly CLIENT="${ANDROMEDA_BACKGROUND_CLIENT:-./build-debug/tests/background-client}"
readonly UNDER_LOG="$ANDROMEDA_RUNTIME_DIR/occluded-under.log"
readonly OVER_LOG="$ANDROMEDA_RUNTIME_DIR/occluded-over.log"

cat >> "$ANDROMEDA_CONFIG" <<'CONF'

[animation]
enabled = false
CONF
"$ANDROMEDA" msg config-reload > /dev/null

"$CLIENT" occluded-under > "$UNDER_LOG" 2>&1 &
await_lines "$UNDER_LOG" "presence 0" 1
"$CLIENT" occluded-over > "$OVER_LOG" 2>&1 &
await_events "$OVER_LOG" mapped 1
"$ANDROMEDA" settle

"$ANDROMEDA" msg window-toggle-fullscreen > /dev/null
await_lines "$UNDER_LOG" "presence 1" 1
await_events "$UNDER_LOG" suspended 1 "the covered window"

"$ANDROMEDA" msg window-toggle-fullscreen > /dev/null
await_lines "$UNDER_LOG" "presence 0" 2
await_events "$UNDER_LOG" resumed 1 "the uncovered window"

presence=$(grep '^presence ' "$UNDER_LOG" | cut -d' ' -f2 | tr '\n' ' ')
if [[ $presence != "0 1 0 " ]]; then
  echo "presence went '$presence', not '0 1 0 ' (shown, occluded under fullscreen, shown)"
  exit 1
fi
over=$(grep '^presence ' "$OVER_LOG" | cut -d' ' -f2 | sort -u | tr '\n' ' ')
if [[ $over != "0 " ]]; then
  echo "the fullscreen window's presence went '$over', not only shown"
  exit 1
fi
echo "presence $presence; suspended while covered, resumed after"
