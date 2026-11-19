#!/usr/bin/env bash
# Spaces overview: the shown desktop's thumb draws a copy of its window. While the overview opens, that copy stays
# inside its thumb. With THUMB_SHOTS set to a directory, the frames are copied there.
set -euo pipefail
source "$ANDROMEDA_HARNESS_LIB"

readonly FRAMES=30
IMAGE="$ANDROMEDA_RUNTIME_DIR/thumb-copy.png"

cat >> "$ANDROMEDA_CONFIG" <<CONFIG
[overview]
style = "spaces"
background_blur = true
CONFIG
"$ANDROMEDA" msg config-reload > /dev/null

FILL_COLOR=0xFFFF0000 "$ANDROMEDA_UNMAP_CLIENT" red 1280 690 > /dev/null 2>&1 &
for _ in $(seq 80); do
  [[ -n $("$ANDROMEDA" windows --json | jq -r '.[] | select(.title == "red") | .id') ]] && break
  sleep 0.025
done
"$ANDROMEDA" settle > /dev/null

"$ANDROMEDA" clock-freeze > /dev/null
"$ANDROMEDA" msg overview-open > /dev/null
for frame in $(seq "$FRAMES"); do
  "$ANDROMEDA" clock-advance 16 > /dev/null
  grim -o HEADLESS-1 "$IMAGE"
  [[ -n ${THUMB_SHOTS:-} ]] && cp "$IMAGE" "$THUMB_SHOTS/open-$((frame * 16))ms.png"
done
"$ANDROMEDA" clock-resume > /dev/null
"$ANDROMEDA" settle > /dev/null
"$ANDROMEDA" clock-freeze > /dev/null
"$ANDROMEDA" msg overview-close > /dev/null
for frame in $(seq "$FRAMES"); do
  "$ANDROMEDA" clock-advance 16 > /dev/null
  grim -o HEADLESS-1 "$IMAGE"
  [[ -n ${THUMB_SHOTS:-} ]] && cp "$IMAGE" "$THUMB_SHOTS/close-$((frame * 16))ms.png"
done
"$ANDROMEDA" clock-resume > /dev/null
