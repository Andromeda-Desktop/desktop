#!/usr/bin/env bash
# A window on the active workspace is suspended and gets no frame callbacks while the session is locked and while its
# output is powered off, and resumes with its frames after each; a game keeps its slow tick behind the lock. Its
# dsk_surface_v1.presence goes shown (0) -> off (3) -> shown for each, one event per change. A layer surface (a bar)
# follows the same presence sequence: it is not a view, but it shows only while its output is on and the session open.
# The window's subsurface and popup follow the window's presence through their own dsk_surface_v1 objects.
set -euo pipefail
source "$ANDROMEDA_HARNESS_LIB"

readonly CLIENT="${ANDROMEDA_BACKGROUND_CLIENT:-./build-debug/tests/background-client}"
readonly LOCK_CLIENT="${ANDROMEDA_LOCK_CLIENT:-./build-debug/tests/lock-client}"
readonly NORMAL_LOG="$ANDROMEDA_RUNTIME_DIR/visible-normal.log"
readonly GAME_LOG="$ANDROMEDA_RUNTIME_DIR/visible-game.log"
readonly LOCK_LOG="$ANDROMEDA_RUNTIME_DIR/lock-client.log"
readonly LOCK_FIFO="$ANDROMEDA_RUNTIME_DIR/lock-control"
readonly LAYER_CLIENT="${ANDROMEDA_LAYER_CLIENT:-./build-debug/tests/layer-client}"
readonly LAYER_LOG="$ANDROMEDA_RUNTIME_DIR/layer.log"
readonly LAYER_FIFO="$ANDROMEDA_RUNTIME_DIR/layer-control"

CHILDREN=1 "$CLIENT" visible-normal > "$NORMAL_LOG" 2>&1 &
CONTENT_TYPE=game "$CLIENT" visible-game > "$GAME_LOG" 2>&1 &
mkfifo "$LAYER_FIFO"
exec {layer_fd}<> "$LAYER_FIFO"
LAYER_DSK_VERSION=20 LAYER_CONTROL=1 "$LAYER_CLIENT" HEADLESS-1 40 <&"$layer_fd" > "$LAYER_LOG" 2>&1 &
await_lines "$LAYER_LOG" ready 1
printf '%s\n' "dsk-secure bar 0" >&"$layer_fd"
await_lines "$LAYER_LOG" "dsk-presence 0" 1
await_events "$NORMAL_LOG" mapped 1
await_events "$GAME_LOG" mapped 1
"$ANDROMEDA" settle

frames() { events "$1" frame; }

# Frames a log gains over one second.
frames_in_a_second() {
  local before
  before=$(frames "$1")
  sleep 1 # real time: the background tick runs on a wall-clock timer
  echo $(($(frames "$1") - before))
}

expect_resumed_with_frames() {
  await_events "$NORMAL_LOG" resumed "$1" "the window after $2"
  local back
  back=$(frames "$NORMAL_LOG")
  for _ in $(seq 40); do
    (($(frames "$NORMAL_LOG") > back + 2)) && return 0
    sleep 0.05
  done
  echo "frames did not resume after $2"
  return 1
}

mkfifo "$LOCK_FIFO"
exec {lock_fd}<> "$LOCK_FIFO"
"$LOCK_CLIENT" <&"$lock_fd" > "$LOCK_LOG" 2>&1 &
await_events "$LOCK_LOG" locked 1 "the lock client"
await_events "$NORMAL_LOG" suspended 1 "the window behind the lock"
if grep -q '^suspended$' "$GAME_LOG"; then
  echo "the game was suspended behind the lock"
  exit 1
fi
locked_frames=$(frames_in_a_second "$NORMAL_LOG")
game_frames=$(frames_in_a_second "$GAME_LOG")
if ((locked_frames > 1)); then
  echo "the window kept receiving frames behind the lock: $locked_frames in 1 s"
  exit 1
fi
if ((game_frames < 5)); then
  echo "the game lost its tick behind the lock: $game_frames frames in 1 s"
  exit 1
fi

echo unlock >&"$lock_fd"
await_events "$LOCK_LOG" unlocked 1 "the lock client"
expect_resumed_with_frames 1 "unlocking"

"$ANDROMEDA" msg dpms-off > /dev/null
await_events "$NORMAL_LOG" suspended 2 "the window on a powered-off output"
off_frames=$(frames_in_a_second "$NORMAL_LOG")
if ((off_frames > 1)); then
  echo "the window kept receiving frames with its output off: $off_frames in 1 s"
  exit 1
fi

"$ANDROMEDA" msg dpms-on > /dev/null
expect_resumed_with_frames 2 "powering the output on"

presence=$(grep '^presence ' "$NORMAL_LOG" | cut -d' ' -f2 | tr '\n' ' ')
if [[ $presence != "0 3 0 3 0 " ]]; then
  echo "dsk presence went '$presence', not '0 3 0 3 0 ' (shown, off at lock, shown, off at dpms, shown)"
  exit 1
fi

for child in sub popup; do
  child_presence=$(grep "^$child-presence " "$NORMAL_LOG" | cut -d' ' -f2 | tr '\n' ' ')
  if [[ $child_presence != "0 3 0 3 0 " ]]; then
    echo "$child presence went '$child_presence', not '0 3 0 3 0 '"
    exit 1
  fi
done

await_lines "$LAYER_LOG" "dsk-presence 0" 3
layer_presence=$(grep '^dsk-presence ' "$LAYER_LOG" | cut -d' ' -f2 | tr '\n' ' ')
if [[ $layer_presence != "0 3 0 3 0 " ]]; then
  echo "layer presence went '$layer_presence', not '0 3 0 3 0 '"
  exit 1
fi

echo "presence $presence (layer $layer_presence); suspended behind the lock ($locked_frames frames/s, game kept $game_frames) and with the output off ($off_frames frames/s); resumed after each"
