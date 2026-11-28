#!/usr/bin/env bash
# A full-width shape whose bottom corners flare across (a bar's inner edge) blurs its body and the two flare squares,
# not the radius-high band along its whole bottom edge. Its blur area per frame stays well below that of a plain shape
# of the same flared box. Needs ANDROMEDA_FRAME_LOG in the harness environment for the blur counts.
set -euo pipefail
source "$ANDROMEDA_HARNESS_LIB"
[[ -n ${ANDROMEDA_FRAME_LOG:-} ]] || { echo "SKIP: needs ANDROMEDA_FRAME_LOG"; exit 0; }

cat >> "$ANDROMEDA_CONFIG" <<'CONFIG'

[animation]
enabled = false
[appearance.blur]
enabled = true
optimized = false
[[window_rule]]
match.title = "^under$"
default_floating = true
default_position = { x = 0, y = 0, anchor = "top_left" }
CONFIG
"$ANDROMEDA" msg config-reload > /dev/null

mkfifo "$ANDROMEDA_RUNTIME_DIR/under.fifo"
exec {under_fd}<> "$ANDROMEDA_RUNTIME_DIR/under.fifo"
REPAINT_ON_STDIN=1 FILL_COLOR=0xFF4080C0 "$ANDROMEDA_UNMAP_CLIENT" under 1280 300 \
  < "$ANDROMEDA_RUNTIME_DIR/under.fifo" > "$ANDROMEDA_RUNTIME_DIR/under.log" 2>&1 &
await_lines "$ANDROMEDA_RUNTIME_DIR/under.log" mapped 1

readonly LAYER_LOG="$ANDROMEDA_RUNTIME_DIR/glass-shape-flare-area-layer.log"
mkfifo "$ANDROMEDA_RUNTIME_DIR/layer.fifo"
env LAYER_CONTROL=1 LAYER_DSK_VERSION=23 LAYER_EXCLUSIVE=-1 "$ANDROMEDA_LAYER_CLIENT" HEADLESS-1 120 \
  < "$ANDROMEDA_RUNTIME_DIR/layer.fifo" > "$LAYER_LOG" 2>&1 &
exec {layer_fd}> "$ANDROMEDA_RUNTIME_DIR/layer.fifo"
await_lines "$LAYER_LOG" ready 1

layer_command() {
  local action=$1 label=$2
  shift 2
  printf '%s %s %s\n' "$action" "$label" "$*" >&"$layer_fd"
  await_lines "$LAYER_LOG" "done $label" 1
}
repaints=0
# blur_per_frame: the mean live blur pixels of the frames that 10 repaints of the window under the shape draw.
blur_per_frame() {
  "$ANDROMEDA" settle > /dev/null
  local start i frames blur
  start=$(wc -l < "$ANDROMEDA_FRAME_LOG")
  for i in $(seq 1 10); do
    echo R >&"$under_fd"
    repaints=$((repaints + 1))
    await_lines "$ANDROMEDA_RUNTIME_DIR/under.log" repainted "$repaints"
    "$ANDROMEDA" settle > /dev/null
  done
  # Column 6 is the live blur pixels of a frame.
  read -r frames blur < <(tail -n +"$((start + 1))" "$ANDROMEDA_FRAME_LOG" \
    | awk '$2 == "HEADLESS-1" && $6 > 0 { n++; px += $6 } END { print n + 0, px + 0 }')
  ((frames >= 5)) || { echo "only $frames blurred frames for 10 repaints" >&2; exit 1; }
  echo $((blur / frames))
}

layer_command color clear 00000000
layer_command dsk-shape tint 8 1 1 1 0.2
# Body 0 40 1280 40; the bottom corners flare 20 px down past the body (join c = 1: across their edge).
layer_command dsk-shape clip 3 0 40 1280 40
layer_command dsk-shape corners 15 0 0 -20 -20
layer_command dsk-shape join 9 0 0 1 0
flared=$(blur_per_frame)

# The same flared box as a plain shape: the band blurs.
layer_command dsk-shape plain-corners 15 0 0 0 0
layer_command dsk-shape plain-join 9 0 0 0 0
layer_command dsk-shape plain-clip 3 0 40 1280 60
plain=$(blur_per_frame)

if ((flared > plain - 1280 * 10)); then
  echo "the flared shape blurs $flared px per frame, the plain box $plain: its flare band still blurs"
  exit 1
fi
echo "flared shape $flared px per frame, plain box $plain"
