#!/usr/bin/env bash
# The Mirror page of the Link app (andromeda-link-app) mirrors inside the page: against a private andromeda-linkd and the
# headless phone, which streams an H.264 file of SMPTE bars as the app streams its encoder's output. Proves:
# 1. Opening the Mirror page shows the bars inside the page, with no other window.
# 2. A click on the picture reaches the phone as a tap at that point, a drag as a swipe, and typed text as text.
# 3. Stop tells the phone to stop.
# Writes screen-idle.png, screen-live.png, screen-stopped.png, steps.txt, hold.jsonl, linkd.log, app.log to $OUT
# (default ./artifacts/link-desk-mirror).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/link-desk-mirror}
source "$(dirname "$0")/lib.sh"
LINK_BIN=${LINK_BIN:-$ROOT/link/target/debug}
LINK_APP=${LINK_APP:-$ROOT/link/target/window/window/andromeda-link-app}
POINTER=$ROOT/compositor/build-debug/tests/pointer-client
boot_headless 1
rm -f "$OUT"/*.png "$OUT/steps.txt" "$OUT/hold.jsonl" "$OUT/app.log"

gst-launch-1.0 -q videotestsrc num-buffers=60 pattern=smpte \
  ! video/x-raw,width=720,height=1280,framerate=30/1 ! x264enc tune=zerolatency key-int-max=30 aud=true \
  ! video/x-h264,stream-format=byte-stream,profile=baseline ! filesink location="$RUNTIME/bars.h264"

with_noctalia '
  BIN='"$LINK_BIN"'
  LINK_APP='"$LINK_APP"'
  POINTER='"$POINTER"'
  fail() { grim "$OUT/fail.png" || true; echo "FAIL: $*" >&2; exit 1; }
  step() { printf "%s\n" "$*" | tee -a "$OUT/steps.txt"; }
  WAIT_S=20
  link() { gdbus call --session -d org.andromeda.Link1 -o /org/andromeda/Link1 -m "org.andromeda.Link1.$1" "${@:2}"; }
  shot() { sleep 1.5; grim "$OUT/$1.png"; } # real time: the window repaints
  click() { "$POINTER" 1280 720 move "$1" "$2" click 272 > /dev/null; }
  events() { grep -c "\"event\":\"$1\"" "$OUT/hold.jsonl" || true; }

  XDG_STATE_HOME=$RUNTIME/linkd "$BIN/andromeda-linkd" > "$OUT/linkd.log" 2>&1 &
  poll_for 5 link CancelPairing > /dev/null 2>&1 || true
  PORT=$(python3 -c "import json,sys; print(json.load(open(sys.argv[1]))[\"port\"])" "$RUNTIME/linkd/andromeda-link/devices.json")
  CODE=$(link StartPairing | sed -E "s/^\(.([0-9]+)., .*/\1/")
  "$BIN/andromeda-link-phone" --state "$RUNTIME/phone" --name "OnePlus 12" pair --code "$CODE" --addr "127.0.0.1:$PORT" \
    > /dev/null 2>> "$OUT/phone.log" || fail "pairing"
  ID=$(python3 -c "import json,sys; print(json.load(open(sys.argv[1]))[\"peers\"][0][\"id\"])" "$RUNTIME/linkd/andromeda-link/devices.json")
  link SetGrant "$ID" screen true > /dev/null
  python3 -c "import json,sys; s=json.load(open(sys.argv[1])); s[\"peers\"][0].setdefault(\"grants\", {})[\"screen\"]=True; json.dump(s, open(sys.argv[1], \"w\"))" "$RUNTIME/phone/devices.json"
  "$BIN/andromeda-link-phone" --state "$RUNTIME/phone" --name "OnePlus 12" --mirror-file "$RUNTIME/bars.h264" \
    --mirror-size 720x1280 hold > "$OUT/hold.jsonl" 2>> "$OUT/phone.log" &
  wait_for "the phone never connected" grep -q "\"event\":\"connected\"" "$OUT/hold.jsonl"

  "$LINK_APP" >> "$OUT/app.log" 2>&1 &
  APP=$!
  link_window_mapped() {
    w=$("$ANDROMEDA" windows --json | jq -c "[.. | objects | select(.app_id? == \"dev.andromeda.Link\")][0] // empty")
    [[ -n $w && $(jq .w <<< "$w") -gt 0 ]]
  }
  WAIT_S=10 wait_for "the Link window did not map" link_window_mapped
  [[ $(jq -r "[.w, .h] | join(\"x\")" <<< "$w") == 1200x592 ]] || fail "the points below assume a 1200x592 window: $w"
  WX=$(jq .x <<< "$w") WY=$(jq .y <<< "$w")
  # Window points: Mirror in the sidebar, the picture (x 596..834, y 88..513 for 720x1280), Stop in the caption.
  at() { click $((WX + $1)) $((WY + $2)); }
  shot screen-idle
  at 114 404
  wait_for "the mirror never started" grep -q "mirroring at" "$OUT/linkd.log"
  shot screen-live
  [[ -z $(pgrep -P "$APP" || true) ]] || fail "the app started a separate mirror: $(pgrep -a -P "$APP")"
  pixel() { magick "$OUT/screen-live.png" -format "%[fx:int(255*r)],%[fx:int(255*g)],%[fx:int(255*b)]" -crop "1x1+$1+$2" info:; }
  YELLOW=$(pixel $((WX + 648)) $((WY + 202))) BLUE=$(pixel $((WX + 820)) $((WY + 202)))
  python3 - "$YELLOW" "$BLUE" <<PY || fail "the page does not show the bars: yellow=$YELLOW blue=$BLUE"
import sys
yellow, blue = ([int(c) for c in arg.split(",")] for arg in sys.argv[1:])
assert yellow[0] > 150 and yellow[1] > 150 and yellow[2] < 60, yellow
assert blue[2] > 150 and blue[0] < 60 and blue[1] < 60, blue
PY
  step "live: the bars show inside the page (yellow $YELLOW, blue $BLUE)"

  at 715 300
  X=$((WX + 715))
  "$POINTER" 1280 720 move $X $((WY + 452)) press 272 move $X $((WY + 372)) move $X $((WY + 252)) move $X $((WY + 152)) \
    release 272 > /dev/null
  "$POINTER" 1280 720 move $X $((WY + 300)) notch 1 > /dev/null
  wtype -s 300 hi
  wait_for "the input did not all reach the phone" eval "(( \$(events mirror-input) >= 5 ))"
  grep "\"event\":\"mirror-input\"" "$OUT/hold.jsonl" > "$OUT/input.jsonl"
  python3 - "$OUT/input.jsonl" <<PY || fail "the phone got other input: $(cat "$OUT/input.jsonl")"
import json, sys
got = [json.loads(line) for line in open(sys.argv[1])]
# Raw touch since 68b5148: a click is down + up in place, a drag is down, moves, up; a wheel notch is one wheel event.
downs = [e for e in got if e["action"] == "touch-down"]
ups = [e for e in got if e["action"] == "touch-up"]
wheel = next(e for e in got if e["action"] == "wheel")
tap, swipe_down, swipe_up = downs[0], downs[1], ups[1]
assert abs(tap["x"] - 5000) < 150 and abs(tap["y"] - 5000) < 150, tap
assert abs(ups[0]["y"] - tap["y"]) < 50, ups[0]
assert abs(swipe_down["x"] - 5000) < 150 and swipe_down["y"] > 7500 and swipe_up["y"] < 2000, (swipe_down, swipe_up)
assert wheel["y2"] > 0, wheel
assert "".join(e["text"] for e in got if e["action"] == "text") == "hi", got
PY
  step "input: a tap, a drag, a wheel notch, and typed text reach the phone: $(tr -d "\n" < "$OUT/input.jsonl" | head -c 400)"

  at 1053 28
  wait_for "Stop did not stop the phone" grep -q "\"event\":\"mirror-stop\".*viewer closed" "$OUT/hold.jsonl"
  shot screen-stopped
  step "stopped: Stop ended the phone stream"
'
