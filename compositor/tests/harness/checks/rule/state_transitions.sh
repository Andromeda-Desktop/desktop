#!/usr/bin/env bash
# A float or pin transition re-selects window rules, so `match.is_floating` and
# `match.is_pinned` effects must land on the toggle itself. Nothing here moves
# focus or identity, so no other rule refresh can produce the effect.
set -euo pipefail

source "$ANDROMEDA_HARNESS_LIB"

readonly CLIENT="${ANDROMEDA_SUBSURFACE_CLIENT:-./build-debug/tests/subsurface-client}"
readonly SCREENSHOT="$ANDROMEDA_RUNTIME_DIR/rule-state-transitions.png"

if [[ ! -x $CLIENT ]]; then
  echo "subsurface client not built at $CLIENT"
  exit 1
fi

cat >> "$ANDROMEDA_CONFIG" <<'EOF'

[animation]
enabled = false

[colors]
backdrop = "#00FF00FF"

[appearance]
border_width = 0
corner_radius = 0

[[window_rule]]
match.title = "^state-rules$"
match.is_floating = true
opacity = 0.5

[[window_rule]]
match.title = "^state-rules$"
match.is_pinned = true
opacity = 0.5
EOF
"$ANDROMEDA" msg config-reload > /dev/null

field_of() {
  "$ANDROMEDA" windows --json | jq -r --arg field "$1" '.[] | select(.title == "state-rules") | .[$field]'
}


# The client's blue child surface covers the window centre. Opaque it encodes as
# blue 255; at rule opacity 0.5 the green backdrop shows through and drops it to
# roughly 128.
sample_blue() {
  local x y w h
  read -r x y w h <<< "$(
    "$ANDROMEDA" windows --json | jq -r '.[] | select(.title == "state-rules") | "\(.x) \(.y) \(.w) \(.h)"'
  )"
  grim "$SCREENSHOT"
  magick "$SCREENSHOT" -crop "20x20+$((x + w / 2 - 10))+$((y + h / 2 - 10))" \
    -format '%[fx:round(255*mean.b)]' info:
}

# Failures go to stderr: the caller captures stdout to report the samples it saw.
expect_state() {
  local label=$1 want=$2 blue=
  "$ANDROMEDA" settle
  blue=$(sample_blue)
  if [[ $want == applied ]] && ((blue > 200)); then
    echo "$label: state rule was not applied (blue=$blue)" >&2
    exit 1
  fi
  if [[ $want == absent ]] && ((blue < 240)); then
    echo "$label: state rule stayed applied (blue=$blue)" >&2
    exit 1
  fi
  echo "$label blue=$blue"
}

"$CLIENT" state-rules 640 480 > "$ANDROMEDA_RUNTIME_DIR/state-rules.log" 2>&1 &
await_field state-rules floating false
tiled=$(expect_state "tiled" absent)

"$ANDROMEDA" msg window-toggle-floating > /dev/null
await_field state-rules floating true
floated=$(expect_state "floating" applied)

"$ANDROMEDA" msg window-toggle-floating > /dev/null
await_field state-rules floating false
retiled=$(expect_state "re-tiled" absent)

"$ANDROMEDA" msg window-toggle-pinned > /dev/null
await_field state-rules floating true
pinned=$(expect_state "pinned" applied)

"$ANDROMEDA" msg window-toggle-pinned > /dev/null
await_field state-rules floating false
unpinned=$(expect_state "unpinned" absent)

echo "float and pin transitions re-apply state rules: $tiled, $floated, $retiled, $pinned, $unpinned"
