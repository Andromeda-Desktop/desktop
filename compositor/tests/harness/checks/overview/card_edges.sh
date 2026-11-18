#!/usr/bin/env bash
# While the overview opens, a window card moves smoothly: within each phase of the open (zoom, then spread) each of its
# four edges moves one way only, frame by frame, never stepping back by a pixel (DESIGN.md "Overview v3": card edges are rounded once from one float rect).
set -euo pipefail

readonly OUTPUT_W=1280
readonly OUTPUT_H=720
IMAGE="$ANDROMEDA_RUNTIME_DIR/edges.png"

cat >> "$ANDROMEDA_CONFIG" <<'EOF'

[layout]
mode = "floating"

[overview]
style = "spaces"
background_blur = false

[animation.overview]
duration_ms = 1000
curve = "linear"
workspace_curve = "linear"
EOF
"$ANDROMEDA" msg config-reload > /dev/null

FILL_COLOR=0xFFFF0000 "$ANDROMEDA_UNMAP_CLIENT" edges-card 701 433 > /dev/null 2>&1 &
for _ in $(seq 60); do
  [[ $("$ANDROMEDA" windows --json | jq 'length') -eq 1 ]] && break
  sleep 0.05
done
"$ANDROMEDA_POINTER_CLIENT" "$OUTPUT_W" "$OUTPUT_H" move 5 5
"$ANDROMEDA" settle

edges() {
  grim -o HEADLESS-1 "$IMAGE"
  read -r x y w h < <("$ANDROMEDA_PIXEL_PROBE" "$IMAGE" bbox 'r > 0.9 && g < 0.1 && b < 0.1')
  echo "$x $y $((x + w)) $((y + h))"
}

"$ANDROMEDA" clock-freeze > /dev/null
"$ANDROMEDA" msg overview-open > /dev/null
frames=()
for _ in $(seq 30); do
  "$ANDROMEDA" clock-advance 33 > /dev/null
  frames+=("$(edges)")
done
"$ANDROMEDA" clock-resume > /dev/null

# 33 ms frames of the 1000 ms linear open: zoom alone runs to p .45 (frame 13), spread alone from p .55 (frame 17)
# (motion.overview_spread_start, overview_zoom_end). Each phase sets an edge's direction from its first and last frame;
# any frame-to-frame step against it is jitter.
printf '%s\n' "${frames[@]}" | awk '
  { for (i = 1; i <= 4; i++) v[NR, i] = $i }
  END {
    split("left top right bottom", name, " ")
    split("1 13 17 30", seg, " ")
    for (s = 1; s <= 3; s += 2) for (i = 1; i <= 4; i++) {
      dir = v[seg[s + 1], i] - v[seg[s], i]
      for (n = seg[s] + 1; n <= seg[s + 1]; n++) {
        step = v[n, i] - v[n - 1, i]
        if (dir * step < 0) { printf "the %s edge stepped back at frame %d: %d -> %d\n", name[i], n, v[n - 1, i], v[n, i]; bad = 1 }
      }
    }
    exit bad
  }' || exit 1
echo "every card edge moves one way within each phase of the open"
