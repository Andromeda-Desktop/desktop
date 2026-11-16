#!/usr/bin/env bash
# A window drawn scaled by the open popin maps a click back to the surface point drawn under the cursor: a press
# 10 px inside the drawn edge of a half-size window lands 20 px inside the surface edge, not at the 1:1 point.
set -euo pipefail

readonly BTN_LEFT=272
readonly POINTER="${ANDROMEDA_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
readonly OBSERVER="${ANDROMEDA_SEAT_LOG_CLIENT:-./build-debug/tests/seat-log-client}"
readonly LOG="$ANDROMEDA_RUNTIME_DIR/popin-hit.log"

cat >> "$ANDROMEDA_CONFIG" <<'EOF'

[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0

[appearance.shadow]
enabled = false

[animation]
duration_ms = 5000
curve = "linear"

[animation.windows_in]
style = "popin"
scale = 0.5

[animation.windows_move]
enabled = false

[animation.dim_unfocused]
enabled = false

[[window_rule]]
match.title = "^popin-hit$"
default_floating = true
default_floating_size_px = { width = 400, height = 240 }
default_position = { x = 100, y = 180, anchor = "top_left" }
EOF
"$ANDROMEDA" msg config-reload > /dev/null

"$ANDROMEDA" clock-freeze
LOG_POINTER_INPUT=1 "$OBSERVER" popin-hit > "$LOG" 2>&1 &
for _ in $(seq 200); do
  [[ $("$ANDROMEDA" windows --json | jq '[.[] | select(.title == "popin-hit")] | length') -eq 1 ]] && break
  sleep 0.025
done
"$ANDROMEDA" clock-advance 1

# At progress 0 the 400x240 window at (100,180) is drawn 200x120 around its centre: (200,240)-(400,360).
"$POINTER" 1280 720 move 390 350 click "$BTN_LEFT"
for _ in $(seq 100); do
  grep -q '^press-position' "$LOG" && break
  sleep 0.025
done
"$ANDROMEDA" clock-advance 6000

read -r x y < <(sed -n 's/^press-position x=\([0-9-]*\) y=\([0-9-]*\)$/\1 \2/p' "$LOG" | head -n 1)
if [[ -z ${x:-} ]]; then
  echo "the click never reached the opening window"
  cat "$LOG"
  exit 1
fi
if (( x < 376 || x > 384 || y < 216 || y > 224 )); then
  echo "click 10 px inside the drawn corner reached surface ($x,$y), expected about (380,220)"
  exit 1
fi
