#!/usr/bin/env bash
# harness: render_thread
# A GPU submit stuck for 6 s (another process holding the kernel buffer locks) stops only the render thread: the
# main loop answers IPC within 250 ms throughout (the stall-report threshold; most of it is the IPC client's spawn),
# a window opened meanwhile maps, and once the submit returns its buffer, uploaded late, is on screen.
set -euo pipefail
readonly CLIENT="${ANDROMEDA_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly TITLE=render-thread-stall

"$ANDROMEDA" stall-submit 6000 > /dev/null
FILL_COLOR=0xFFFF0000 "$CLIENT" "$TITLE" 400 240 > "$ANDROMEDA_RUNTIME_DIR/client.log" 2>&1 &
start=$(date +%s%N)
slowest=0
mapped=false
while (( $(date +%s%N) - start < 5000000000 )); do
  t0=$(date +%s%N)
  windows=$("$ANDROMEDA" windows --json)
  ms=$(( ($(date +%s%N) - t0) / 1000000 ))
  (( ms > slowest )) && slowest=$ms
  jq -e --arg t "$TITLE" 'any(.[]; .title == $t)' <<< "$windows" > /dev/null && mapped=true
  sleep 0.2
done
(( slowest < 250 )) || { echo "IPC took $slowest ms during the stalled submit, want < 250"; exit 1; }
$mapped || { echo "the window opened during the stalled submit did not map"; exit 1; }
grep -q 'GL call while the render thread holds the context' "$ANDROMEDA_LOG" \
  && { echo "a main-thread GL call ran while the context was lent: $(grep -m1 'GL call while' "$ANDROMEDA_LOG")"; exit 1; }

timeout 10 "$ANDROMEDA" settle > /dev/null || { echo "the output did not draw after the stalled submit returned"; exit 1; }
read -r x y w h < <("$ANDROMEDA" windows --json | jq -r --arg t "$TITLE" '.[] | select(.title == $t) | "\(.x) \(.y) \(.w) \(.h)"')
grim "$ANDROMEDA_RUNTIME_DIR/after.png"
rgb=$(magick "$ANDROMEDA_RUNTIME_DIR/after.png" -crop "1x1+$((x + w / 2))+$((y + h / 2))" -format '%[fx:int(255*r)] %[fx:int(255*g)] %[fx:int(255*b)]' info:)
read -r r g b <<< "$rgb"
(( r > 200 && g < 60 && b < 60 )) || { echo "window center is rgb $rgb after the stall, want its red buffer"; exit 1; }
echo "a 6 s stalled submit leaves IPC under 250 ms (slowest $slowest ms); a window opened meanwhile maps and shows its buffer"
