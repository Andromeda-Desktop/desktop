#!/usr/bin/env bash
# Adaptive window glass over a bottom-layer surface (desktop icons, widgets) reads the wallpaper cache: a window that
# repaints every frame adds no live blur pass. Needs ANDROMEDA_FRAME_LOG in the harness environment for the blur counts.
set -euo pipefail
[[ -n ${ANDROMEDA_FRAME_LOG:-} ]] || { echo "SKIP: needs ANDROMEDA_FRAME_LOG"; exit 0; }
source "$ANDROMEDA_HARNESS_LIB"
cat >> "$ANDROMEDA_CONFIG" <<'EOF'

[animation]
enabled = false
[appearance.blur]
enabled = true
optimized = true
[[window_rule]]
match.title = "^glass$"
blur = true
blur_optimized = false
default_floating = true
default_position = { x = 0, y = 0, anchor = "top_left" }
EOF
"$ANDROMEDA" msg config-reload > /dev/null
"$ANDROMEDA_LAYER_CLIENT" HEADLESS-1 0 > "$ANDROMEDA_RUNTIME_DIR/wall.log" 2>&1 &
await_lines "$ANDROMEDA_RUNTIME_DIR/wall.log" ready 1
"$ANDROMEDA_LAYER_CLIENT" HEADLESS-1 0 bottom-layer > "$ANDROMEDA_RUNTIME_DIR/square.log" 2>&1 &
await_lines "$ANDROMEDA_RUNTIME_DIR/square.log" ready 1
mkfifo "$ANDROMEDA_RUNTIME_DIR/glass.fifo"
exec {glass_fd}<> "$ANDROMEDA_RUNTIME_DIR/glass.fifo"
REPAINT_ON_STDIN=1 FILL_COLOR=0x40FFFFFF "$ANDROMEDA_UNMAP_CLIENT" glass 600 400 \
  < "$ANDROMEDA_RUNTIME_DIR/glass.fifo" > "$ANDROMEDA_RUNTIME_DIR/glass.log" 2>&1 &
await_lines "$ANDROMEDA_RUNTIME_DIR/glass.log" mapped 1
"$ANDROMEDA" settle > /dev/null

start=$(wc -l < "$ANDROMEDA_FRAME_LOG")
for i in $(seq 1 20); do
  echo R >&"$glass_fd"
  await_lines "$ANDROMEDA_RUNTIME_DIR/glass.log" repainted "$i"
  "$ANDROMEDA" settle > /dev/null
done
# Column 6 is the live blur pixels of a frame.
read -r frames blur < <(tail -n +"$((start + 1))" "$ANDROMEDA_FRAME_LOG" \
  | awk '$2 == "HEADLESS-1" { n++; px += $6 } END { print n + 0, px + 0 }')
((frames >= 10)) || { echo "only $frames frames drawn for 20 repaints"; exit 1; }
window=$((600 * 400))
if ((blur > frames * window / 10)); then
  echo "glass over the bottom layer blurred live: $blur px over $frames frames (window $window px)"
  exit 1
fi
echo "glass over the bottom layer read the cache: $blur live blur px over $frames frames"
