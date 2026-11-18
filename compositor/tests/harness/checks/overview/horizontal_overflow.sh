#!/usr/bin/env bash
# A Strip row shows its workspace as the screen does: a column that runs past the output edge is cut at the row
# frame, at full size, never shrunk to fit and never drawn outside the frame. This holds along whichever axis the
# columns scroll, while the neighbouring workspace's row steps along the other axis.
set -euo pipefail

readonly CLIENT="${ANDROMEDA_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly SCREENSHOT="$ANDROMEDA_RUNTIME_DIR/horizontal-overview-overflow.png"
readonly VERTICAL_SHOT="$ANDROMEDA_RUNTIME_DIR/vertical-overview-overflow.png"
# At zoom 0.5 the row frame is 640x360 at x 320, y 194 (centred, 14 px below the middle); the second workspace's row
# starts one frame width plus the 64 px row gap further along X.
readonly FRAME_X=320
readonly FRAME_Y=194
readonly FRAME_W=640
readonly FRAME_H=360
readonly NEIGHBOUR_X=1024
# The unmap client's default fill.
readonly FILL='r > 0.3 && r < 0.37 && g > 0.43 && g < 0.5 && b > 0.63 && b < 0.7'

spawn_client() {
  "$CLIENT" "$1" 1200 700 > /dev/null 2>&1 &
}

sample_blue() {
  local image=$1 x=$2 y=$3
  magick "$image" -crop "20x20+$x+$y" -format '%[fx:round(255*mean.b)]' info:
}

wait_for_count() {
  local want=$1
  for _ in $(seq 60); do
    [[ $("$ANDROMEDA" windows --json | jq 'length') -eq $want ]] && return 0
    sleep 0.1
  done
  echo "expected $want windows, got: $("$ANDROMEDA" windows --json)"
  return 1
}

cat >> "$ANDROMEDA_CONFIG" <<'EOF'

[animation.overview]
duration_ms = 1000
curve = "linear"

[colors]
backdrop = "#000000FF"

[colors.overview]
background_tint = "#000000FF"
workspace_background = "#000000FF"

[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0

[overview]
style = "strip"
zoom = 0.5
EOF
"$ANDROMEDA" msg config-reload > /dev/null

spawn_client horizontal-overflow-first
wait_for_count 1
spawn_client horizontal-overflow-second
wait_for_count 2
spawn_client horizontal-overflow-third
wait_for_count 3

first_id=$("$ANDROMEDA" windows --json | jq -r '.[] | select(.title == "horizontal-overflow-first") | .id')
if [[ -z $first_id ]]; then
  echo "could not resolve the first horizontal column: $("$ANDROMEDA" windows --json)"
  exit 1
fi
"$ANDROMEDA" msg "window-focus:$first_id" > /dev/null
"$ANDROMEDA" settle
"$ANDROMEDA" msg overview-open > /dev/null
"$ANDROMEDA" settle
grim "$SCREENSHOT"

# fill_outside <image>: client pixels above, below, left and right of the current row frame.
fill_outside() {
  local region n=0
  for region in "1280x${FRAME_Y}+0+0" "1280x$((720 - FRAME_Y - FRAME_H))+0+$((FRAME_Y + FRAME_H))" \
    "${FRAME_X}x${FRAME_H}+0+$FRAME_Y" "$((NEIGHBOUR_X - FRAME_X - FRAME_W))x${FRAME_H}+$((FRAME_X + FRAME_W))+$FRAME_Y"; do
    n=$((n + $("$ANDROMEDA_PIXEL_PROBE" "$1" count "$FILL" "$region")))
  done
  echo "$n"
}

outside=$(fill_outside "$SCREENSHOT")
((outside == 0)) || { echo "horizontal strip: $outside client pixels drawn outside the row frame"; exit 1; }
# The second column starts 1200 px into the desk, so its first 80 px are on screen: the row shows them as a full-height
# slice at the frame's right edge, right of the first column (which ends at x 924).
read -r _ _ sw sh < <("$ANDROMEDA_PIXEL_PROBE" "$SCREENSHOT" bbox "$FILL" "30x${FRAME_H}+$((FRAME_X + FRAME_W - 30))+$FRAME_Y")
if ((sw == 0 || sh < 300)); then
  echo "horizontal strip: the column past the screen edge is not a full-height slice: ${sw}x$sh"
  exit 1
fi

"$ANDROMEDA" msg overview-close > /dev/null
"$ANDROMEDA" settle

# Arrange the workspaces horizontally: the strip turns vertical, so the same three columns now stack downwards past the
# output's bottom edge, and the neighbouring workspace's row steps along X.
cat >> "$ANDROMEDA_CONFIG" <<'EOF'

[output."HEADLESS-1"]
workspaces = 3
workspace_axis = "horizontal"
EOF
"$ANDROMEDA" msg config-reload > /dev/null

# A client on the neighbouring workspace makes its row identifiable by colour: the shared black workspace background
# cannot be told apart from the backdrop.
"$ANDROMEDA" msg workspace-switch:2 > /dev/null
spawn_client horizontal-overflow-neighbour
wait_for_count 4
"$ANDROMEDA" settle
"$ANDROMEDA" msg workspace-switch:1 > /dev/null
"$ANDROMEDA" msg column-focus-first > /dev/null
"$ANDROMEDA" settle

vertical_rows=0
for _ in $(seq 30); do
  vertical_rows=$("$ANDROMEDA" windows --json |
    jq '[.[] | select(.title | startswith("horizontal-overflow-")) | select(.title != "horizontal-overflow-neighbour") | .y] | unique | length')
  [[ $vertical_rows -eq 3 ]] && break
  sleep 0.1
done
if ((vertical_rows != 3)); then
  echo "the scrolling strip never stacked its columns vertically: $("$ANDROMEDA" windows --json)"
  exit 1
fi

"$ANDROMEDA" msg overview-open > /dev/null
"$ANDROMEDA" settle
grim "$VERTICAL_SHOT"

outside=$(fill_outside "$VERTICAL_SHOT")
((outside == 0)) || { echo "vertical strip: $outside client pixels drawn outside the row frame"; exit 1; }
neighbour_blue=$(sample_blue "$VERTICAL_SHOT" $((NEIGHBOUR_X + 60)) $((FRAME_Y + 60)))
if ((neighbour_blue < 80)); then
  echo "the neighbouring workspace's row did not step along X: blue=$neighbour_blue at $((NEIGHBOUR_X + 60)),$((FRAME_Y + 60))"
  exit 1
fi

echo "Strip rows cut columns past the screen edge at the frame, along either axis, and step workspaces along the other"
