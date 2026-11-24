#!/usr/bin/env bash
# harness: outputs=2
# Per-output scrolling defaults size new columns on their owning monitor. A
# reload updates the default for future columns without resizing existing ones.
set -euo pipefail

source "$ANDROMEDA_HARNESS_LIB"

readonly CLIENT="${ANDROMEDA_SUBSURFACE_CLIENT:-./build-debug/tests/subsurface-client}"

window_state() {
  local title=$1
  "$ANDROMEDA" windows --json \
    | jq -c --arg title "$title" '[.[] | select(.title == $title)]'
}

window_at() {
  window_state "$1" | jq -e --arg output "$3:" --argjson width "$2" \
    'length == 1 and .[0].w == $width and (.[0].workspace | startswith($output))' > /dev/null
}

expect_window_at() {
  await_until "$AWAIT_TRIES" "$AWAIT_STEP" window_at "$1" "$2" "$3" && return 0
  echo "expected '$1' at width $2 on $3, got: $(window_state "$1")"
  return 1
}

spawn_client() {
  local title=$1
  "$CLIENT" "$title" 640 480 > "$ANDROMEDA_RUNTIME_DIR/$title.log" 2>&1 &
}

cat >> "$ANDROMEDA_CONFIG" <<'EOF'

[animation]
enabled = false

[appearance]
border_width = 0
outer_border_width = 0

[layout]
mode = "scrolling"
gap = 0

[layout.scrolling]
default_extent_fraction = 0.5
center_underfull_strip = false

[output.HEADLESS-1]
position = [0, 0]
layout.scrolling.default_extent_fraction = 0.25

[output.HEADLESS-2]
position = [1280, 0]
layout.scrolling.default_extent_fraction = 0.75

[[window_rule]]
match.app_id = "^output-width-left$"
default_output = "HEADLESS-1"

[[window_rule]]
match.app_id = "^output-width-right-"
default_output = "HEADLESS-2"
EOF
"$ANDROMEDA" msg config-reload > /dev/null

# Both headless outputs are 1280 logical pixels wide. With zero gap and border,
# their 0.25 and 0.75 defaults produce exact 320 and 960 pixel columns.
spawn_client output-width-left
expect_window_at output-width-left 320 HEADLESS-1
spawn_client output-width-right-old
expect_window_at output-width-right-old 960 HEADLESS-2

# Updating HEADLESS-2 to 0.625 must refresh the existing workspace's resolved
# config. The old column retains its owned width, while the next one starts at
# exactly 800 pixels.
sed -i \
  's/^layout\.scrolling\.default_extent_fraction = 0\.75$/layout.scrolling.default_extent_fraction = 0.625/' \
  "$ANDROMEDA_CONFIG"
"$ANDROMEDA" msg config-reload > /dev/null

spawn_client output-width-right-new
expect_window_at output-width-right-new 800 HEADLESS-2
expect_window_at output-width-right-old 960 HEADLESS-2
expect_window_at output-width-left 320 HEADLESS-1

echo "per-output scrolling defaults sized new columns at 320, 960, then 800 after reload without resizing existing columns"
