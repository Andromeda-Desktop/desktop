#!/usr/bin/env bash
# An app window whose client has a dsk surface gets the density of its window glass over its backdrop: full for dark
# glass over white, none over black. The client draws its window fill from this value.
set -euo pipefail
source "$ANDROMEDA_HARNESS_LIB"

readonly LOG="$ANDROMEDA_RUNTIME_DIR/window-glass-density.log"

cat >> "$ANDROMEDA_CONFIG" <<'CONFIG'

[appearance]
scheme = "dark"
glass_style = "liquid"
edge_reflection = true
[animation]
enabled = false
[appearance.blur]
enabled = true
[colors]
backdrop = "#FFFFFFFF"
[[window_rule]]
match.app_id = "^window-glass-density$"
blur = true
CONFIG
"$ANDROMEDA" msg config-reload > /dev/null

env DSK_DENSITY=1 TRANSLUCENT_CONTENT=1 "$ANDROMEDA_SUBSURFACE_CLIENT" window-glass-density 400 300 > "$LOG" 2>&1 &
await_lines "$LOG" mapped 1
if ! await_lines "$LOG" "dsk-density 1.00" 1; then
  echo "dark window glass over white did not report full density: $(grep dsk-density "$LOG" | tr '\n' ' ')"
  exit 1
fi

sed -i 's/^backdrop = "#FFFFFFFF"$/backdrop = "#000000FF"/' "$ANDROMEDA_CONFIG"
"$ANDROMEDA" msg config-reload > /dev/null
if ! await_lines "$LOG" "dsk-density 0.00" 1; then
  echo "dark window glass over black did not report zero density: $(grep dsk-density "$LOG" | tr '\n' ' ')"
  exit 1
fi
