#!/usr/bin/env bash
# Overview focus ring model (rulebook §11): after a key the selected card carries the accent ring; after pointer motion
# no ring shows. Random key and motion sequences hold this, and the ring stays one card's outline, never two.
set -euo pipefail

readonly OUTPUT_W=1280
readonly OUTPUT_H=720
readonly POINTER="${ANDROMEDA_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
readonly CLIENT="${ANDROMEDA_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly SHOT="$ANDROMEDA_RUNTIME_DIR/focus-ring.png"
readonly K_TAB=15

cat >> "$ANDROMEDA_CONFIG" <<'EOF'

[colors]
backdrop = "#000000FF"
accent_primary = "#FF00FFFF"

[colors.overview]
background_tint = "#000000FF"
workspace_background = "#000000FF"

[appearance]
border_width = 0
outer_border_width = 0

[appearance.blur]
enabled = false

[overview]
style = "strip"
zoom = 0.5
EOF
"$ANDROMEDA" msg config-reload > /dev/null

pointer() { "$POINTER" "$OUTPUT_W" "$OUTPUT_H" "$@"; }
# Pixels of the accent colour on screen.
ring_pixels() {
  grim -o HEADLESS-1 "$SHOT"
  magick "$SHOT" -fx 'r > 0.8 && g < 0.25 && b > 0.8' -format '%[fx:round(mean*w*h)]' info:
}

for name in ring-a ring-b ring-c; do
  "$CLIENT" "$name" 400 300 > /dev/null 2>&1 &
done
for _ in $(seq 60); do
  [[ $("$ANDROMEDA" windows --json | jq 'length') -eq 3 ]] && break
  sleep 0.1
done
pointer move 10 10
"$ANDROMEDA" msg overview-open > /dev/null
"$ANDROMEDA" settle

if (($(ring_pixels) != 0)); then
  echo "the ring shows before any key"
  exit 1
fi
RANDOM=11
one_ring=0
for step in $(seq 16); do
  if ((RANDOM % 2)); then
    pointer tap $K_TAB
    want=shown
  else
    pointer move $((100 + RANDOM % 1000)) $((100 + RANDOM % 500))
    want=hidden
  fi
  "$ANDROMEDA" settle
  pixels=$(ring_pixels)
  if [[ $want == hidden ]] && ((pixels != 0)); then
    echo "step $step: $pixels ring pixels after pointer motion"
    exit 1
  fi
  if [[ $want == shown ]]; then
    if ((pixels < 200)); then
      echo "step $step: no ring after a key ($pixels pixels)"
      exit 1
    fi
    # Cards share one size, so every shown ring has the same outline length.
    ((one_ring == 0)) && one_ring=$pixels
    if ((pixels > one_ring * 3 / 2 || pixels < one_ring * 2 / 3)); then
      echo "step $step: $pixels ring pixels, one ring is $one_ring"
      exit 1
    fi
  fi
done
echo "the selection ring follows the last input: shown after Tab, hidden after pointer motion ($one_ring px)"
