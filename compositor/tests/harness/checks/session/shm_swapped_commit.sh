#!/usr/bin/env bash
# A client swapped out while hidden commits its shm buffer. The commit waits while a worker thread pages the buffer
# in, so the compositor's main thread takes no major fault reading it, and the window then shows its pixels.
set -euo pipefail
readonly CLIENT="${ANDROMEDA_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly LOG="$ANDROMEDA_RUNTIME_DIR/client.log"
readonly TITLE=shm-swapped

PAGEOUT=1 FILL_COLOR=0xFFFF0000 OPAQUE=1 "$CLIENT" "$TITLE" 1200 900 > "$LOG" 2>&1 &
for _ in $(seq 100); do
  grep -q '^compositor-majflt ' "$LOG" && break
  sleep 0.05
done
grep -q '^compositor-majflt ' "$LOG" || { echo "the client printed no fault count: $(cat "$LOG")"; exit 1; }
read -r _ out total < <(grep -m1 '^paged-out ' "$LOG")
(( out > total / 2 )) || { echo "SKIP: MADV_PAGEOUT moved $out of $total pages out (no swap?)"; exit 0; }
pid=$(awk '/^compositor-pid / {print $2; exit}' "$LOG")
before=$(awk '/^compositor-majflt / {print $2; exit}' "$LOG")

timeout 10 "$ANDROMEDA" settle > /dev/null || { echo "the output did not settle after the swapped-out commit"; exit 1; }
after=$(sed 's/.*) //' "/proc/$pid/task/$pid/stat" | awk '{print $10}')
faults=$((after - before))
(( faults < 8 )) || { echo "the main thread took $faults major faults reading a buffer with $out of $total pages in swap"; exit 1; }

read -r x y w h < <("$ANDROMEDA" windows --json | jq -r --arg t "$TITLE" '.[] | select(.title == $t) | "\(.x) \(.y) \(.w) \(.h)"')
grim "$ANDROMEDA_RUNTIME_DIR/shot.png"
rgb=$(magick "$ANDROMEDA_RUNTIME_DIR/shot.png" -crop "1x1+$((x + w / 2))+$((y + h / 2))" -format '%[fx:int(255*r)] %[fx:int(255*g)] %[fx:int(255*b)]' info:)
read -r r g b <<< "$rgb"
(( r > 200 && g < 60 && b < 60 )) || { echo "window center is rgb $rgb, want its red buffer"; exit 1; }
echo "a commit with $out of $total buffer pages in swap cost the main thread $faults major faults and shows its pixels"
