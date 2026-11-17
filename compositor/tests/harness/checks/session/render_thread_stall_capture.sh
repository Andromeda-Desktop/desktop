#!/usr/bin/env bash
# A screen capture whose own GPU copy is stuck (another process holding the kernel buffer locks) never blocks the
# main loop: IPC answers within 250 ms throughout, and the capture still ends with the screen's pixels.
# The first stall holds the frame the capture waits for; the second hits the job that frame's commit starts, which is
# the capture copy.
set -euo pipefail
source "$ANDROMEDA_HARNESS_LIB"
readonly CLIENT="${ANDROMEDA_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly TITLE=render-thread-stall-capture

FILL_COLOR=0xFFFF0000 "$CLIENT" "$TITLE" 400 240 > "$ANDROMEDA_RUNTIME_DIR/client.log" 2>&1 &
await_window "$TITLE"
timeout 10 "$ANDROMEDA" settle > /dev/null
read -r x y w h < <("$ANDROMEDA" windows --json | jq -r --arg t "$TITLE" '.[] | select(.title == $t) | "\(.x) \(.y) \(.w) \(.h)"')

"$ANDROMEDA" stall-submit 1500 > /dev/null
sleep 0.3 # real time: the render thread takes the first stall before grim asks for the frame
grim "$ANDROMEDA_RUNTIME_DIR/stalled.png" > "$ANDROMEDA_RUNTIME_DIR/grim-stalled.log" 2>&1 &
grim_pid=$!
# grim copies through shm; the capture client's GPU buffers make the compositor copy with a render pass.
"$ANDROMEDA_CAPTURE_CLIENT" --dmabuf > "$ANDROMEDA_RUNTIME_DIR/capture.log" 2>&1 &
capture_pid=$!
sleep 0.3 # real time: grim waits on the stalled frame before the second stall is armed
"$ANDROMEDA" stall-submit 3000 > /dev/null
start=$(date +%s%N)
slowest=0
while (( $(date +%s%N) - start < 6000000000 )); do
  t0=$(date +%s%N)
  "$ANDROMEDA" windows --json > /dev/null
  ms=$(( ($(date +%s%N) - t0) / 1000000 ))
  (( ms > slowest )) && slowest=$ms
  sleep 0.1
done
grep -q '^frame ' "$ANDROMEDA_RUNTIME_DIR/capture.log" || { echo "the dmabuf capture got no frame: $(tail -n 3 "$ANDROMEDA_RUNTIME_DIR/capture.log")"; exit 1; }
kill "$capture_pid"
(( slowest < 250 )) || { echo "IPC took $slowest ms while a capture copy was stalled, want < 250"; exit 1; }
timeout 5 tail --pid="$grim_pid" -f /dev/null || { echo "the capture never ended"; exit 1; }
wait "$grim_pid" || { echo "the capture failed: $(cat "$ANDROMEDA_RUNTIME_DIR/grim-stalled.log")"; exit 1; }
grep -q 'GL call while the render thread holds the context' "$ANDROMEDA_LOG" \
  && { echo "a main-thread GL call ran while the context was lent: $(grep -m1 'GL call while' "$ANDROMEDA_LOG")"; exit 1; }
rgb=$(magick "$ANDROMEDA_RUNTIME_DIR/stalled.png" -crop "1x1+$((x + w / 2))+$((y + h / 2))" -format '%[fx:int(255*r)] %[fx:int(255*g)] %[fx:int(255*b)]' info:)
read -r r g b <<< "$rgb"
(( r > 200 && g < 60 && b < 60 )) || { echo "the stalled capture shows rgb $rgb at the window center, want its red buffer"; exit 1; }
echo "a capture copy stalled 3 s leaves IPC under 250 ms (slowest $slowest ms) and still delivers the screen"
