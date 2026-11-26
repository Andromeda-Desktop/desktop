#!/usr/bin/env bash
# The Link app (andromeda-link-app) against a private andromeda-linkd and the headless phone: before the service runs,
# the page says Link is not running; with the service and no phone, the app opens Pair a Phone with a QR code, and
# pairing the phone with that code lists it. Once the phone holds with a status and a player, the daemon exports its
# MPRIS player; Waiting shows a notification with Reply, a missed call, and the phone's file offer. A reply typed on
# the notification and the phone card's Send clipboard both reach the phone. The dark shots follow the shell's theme.
# Writes app-down.png, app-empty.png, app-pairing.png, app-away.png, app-player.png, app-waiting.png, app-light.png,
# app-dark.png, app-settings.png, app-settings-away.png, app-<view>[-away|-dark].png, steps.txt,
# hold.jsonl, linkd.log, session.log, app.log, and phone.log to $OUT (default ./artifacts/link-app).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/link-app}
source "$(dirname "$0")/lib.sh"
LINK_BIN=${LINK_BIN:-$ROOT/link/target/debug}
LINK_APP=${LINK_APP:-$ROOT/apps/link-app/build/andromeda-link-app}
boot_headless 1
rm -f "$OUT"/*.png "$OUT/steps.txt" "$OUT/phone.log" "$OUT/hold.jsonl" "$OUT/app.log"

# The phone's storage, photo albums, contacts, and texts for the views.
mkdir -p "$RUNTIME/storage/Download/Tickets" "$RUNTIME/photos/Camera" "$RUNTIME/photos/Screenshots"
head -c 2400000 /dev/urandom > "$RUNTIME/storage/Download/Lease agreement 2026.pdf"
head -c 380000 /dev/urandom > "$RUNTIME/storage/Download/Boarding pass KTM-PKR.png"
head -c 9100000 /dev/urandom > "$RUNTIME/storage/Download/Rithu (live).mp3"
head -c 52000 /dev/urandom > "$RUNTIME/storage/Download/Tickets/Concert.pdf"
for i in 1 2 3 4 5 6 7; do
  magick -size 600x800 "gradient:#$((30 + i * 9))566a-#c9a4$((40 + i * 7))" -swirl $((i * 40)) "$RUNTIME/photos/Camera/IMG_220$i.png"
done
magick -size 400x860 gradient:#dde1dc-#1e6b66 "$RUNTIME/photos/Screenshots/Screenshot_1.png"
NOW_MS=$(($(date +%s) * 1000))
cat > "$RUNTIME/contacts.json" << EOF
[{"name": "Ada", "numbers": ["+977 980-1112223"]}, {"name": "Bishal Rai", "numbers": ["+977 980-1234567"]}]
EOF
cat > "$RUNTIME/texts.json" << EOF
[{"id": 6, "thread": 1, "address": "+9779801112223", "body": "Perfect, see you there", "date": $((NOW_MS - 60000)), "sent": false},
 {"id": 5, "thread": 1, "address": "+9779801112223", "body": "Yes, 6 at the cafe. Bring the projector cable too", "date": $((NOW_MS - 240000)), "sent": true},
 {"id": 4, "thread": 1, "address": "+9779801112223", "body": "Are we still on for 6?", "date": $((NOW_MS - 600000)), "sent": false},
 {"id": 3, "thread": 2, "address": "+9779801234567", "body": "Call me when you are free", "date": $((NOW_MS - 7200000)), "sent": false},
 {"id": 2, "thread": 3, "address": "NTC", "body": "Your data pack of 10 GB is active until 30 Nov", "date": $((NOW_MS - 90000000)), "sent": false}]
EOF

with_noctalia '
  BIN='"$LINK_BIN"'
  LINK_APP='"$LINK_APP"'
  fail() { grim "$OUT/app-fail.png" || true; echo "FAIL: $*" >&2; exit 1; }
  msg() { "$NOCTALIA" msg "$@" 2>&1 || true; }
  step() { printf "%s\n" "$*" >> "$OUT/steps.txt"; }
  WAIT_S=20
  say() { printf "%s\n" "$*" >&3; }
  shot() { sleep 1.5; grim "$OUT/app-$1.png"; } # real time: the window repaints
  # -s: wtype waits after its keymap upload, else the compositor drops the first key.
  key() { wtype -s 300 "$@"; }
  click() { "'"$ROOT"'/compositor/build-debug/tests/pointer-client" 1280 720 move "$1" "$2" click 272 > /dev/null; }
  app() {
    "$LINK_APP" >> "$OUT/app.log" 2>&1 &
    app_pid=$!
    w=
    link_window_mapped() {
      w=$("$ANDROMEDA" windows --json | jq -c "[.. | objects | select(.app_id? == \"dev.andromeda.Link\")][0] // empty")
      [[ -n $w && $(jq .w <<< "$w") -gt 0 ]]
    }
    WAIT_S=10 wait_for "the Link window did not map" link_window_mapped
    [[ $(jq -r "[.w, .h] | join(\"x\")" <<< "$w") == 1200x592 ]] || fail "the points below assume a 1200x592 window: $w"
    WX=$(jq .x <<< "$w") WY=$(jq .y <<< "$w")
  }
  # Window points of the sidebar rows and the phone card.
  at() { click $((WX + $1)) $((WY + $2)); }
  view() {
    case $1 in
      waiting) at 68 88 ;; messages) at 75 140 ;; calls) at 59 193 ;; photos) at 65 244 ;; files) at 59 290 ;;
      activity) at 68 351 ;; settings) at 62 435 ;; *) fail "no view $1" ;;
    esac
  }
  clips() { grep -c "\"event\":\"clip-offered\"" "$OUT/hold.jsonl" || true; }

  msg theme-mode-set light > /dev/null
  app
  shot down
  step "down: the page before andromeda-linkd runs"

  XDG_STATE_HOME=$RUNTIME/linkd "$BIN/andromeda-linkd" > "$OUT/linkd.log" 2>&1 &
  wait_for "shell never saw andromeda-linkd" eval "[[ \$(msg link-devices) != *error* ]]"
  "$BIN/andromeda-link-session" > "$OUT/session.log" 2>&1 &
  wait_for "andromeda-link-session did not start" grep -q running "$OUT/session.log"
  PORT=$(python3 -c "import json,sys; print(json.load(open(sys.argv[1]))[\"port\"])" "$RUNTIME/linkd/andromeda-link/devices.json")
  shot empty
  step "empty: the service runs and no phone is paired"

  prop() { gdbus call --session -d org.andromeda.Link1 -o /org/andromeda/Link1 -m org.freedesktop.DBus.Properties.Get org.andromeda.Link1 "$1"; }
  # With no phone the app opens Pair a Phone by itself.
  wait_for "the app did not start pairing" eval "[[ \$(prop Pairing) == *true* ]]"
  shot pairing
  # The phone scans the QR code on the screen, as the camera would.
  URI=$(zbarimg -q --raw "$OUT/app-pairing.png") || fail "no QR code on the screen"
  step "pairing: the dialog shows a QR code for ${URI%%&*}"
  "$BIN/andromeda-link-phone" --state "$RUNTIME/phone" --name "OnePlus 12" pair --uri "$URI" --addr "127.0.0.1:$PORT" \
    >> "$OUT/phone.log" 2>&1 || fail "pairing with the QR code the app showed"
  wait_for "phone not listed" eval "[[ \$(msg link-devices) == *\"OnePlus 12\" ]]"
  ID=$(msg link-devices | cut -d" " -f1)
  wait_for "the pairing session never ended" eval "[[ \$(msg link-devices) == \"$ID disconnected OnePlus 12\" ]]"
  shot away
  step "away: paired from the app; the phone is not connected"
  view settings
  shot settings-away
  step "settings-away: grants of a phone that is not connected"
  view photos
  shot photos-off
  step "photos-off: the browse switch is off; the view offers Turn on"
  grant() { gdbus call --session -d org.andromeda.Link1 -o /org/andromeda/Link1 -m org.andromeda.Link1.SetGrant "$ID" "$1" true > /dev/null; }
  grant browse; grant messages; grant screen
  # The phone keeps its own switches for this desk; turn the same ones on, as its user would.
  python3 -c "import json,sys; s=json.load(open(sys.argv[1])); s[\"peers\"][0].setdefault(\"grants\", {}).update(browse=True, messages=True, mic=True, screen=True); json.dump(s, open(sys.argv[1], \"w\"))" "$RUNTIME/phone/devices.json"
  for v in messages calls photos files activity; do view $v; shot $v-away; done
  step "away: every view with its switch on and the phone not connected"
  view waiting

  mkfifo "$RUNTIME/phone.in"
  exec 3<> "$RUNTIME/phone.in"
  "$BIN/andromeda-link-phone" --state "$RUNTIME/phone" --name "OnePlus 12" --browse-root "Internal storage=$RUNTIME/storage" \
    --photos "$RUNTIME/photos" --contacts "$RUNTIME/contacts.json" --texts "$RUNTIME/texts.json" hold --status 82,1,wifi,3 \
    < "$RUNTIME/phone.in" > "$OUT/hold.jsonl" 2>> "$OUT/phone.log" &
  wait_for "phone never shown connected" eval "[[ \$(msg link-devices) == \"$ID connected OnePlus 12\" ]]"
  magick -size 96x96 gradient:"#40566a-#c9a46a" "$RUNTIME/art.png"
  say "media {\"player\":\"spotify\",\"name\":\"Spotify\",\"state\":\"playing\",\"title\":\"Kathmandu Rain\",\"artist\":\"Bipul Chettri\",\"album\":\"Rithu\",\"length_ms\":220000,\"position_ms\":72000,\"volume\":70,\"can\":[\"play\",\"pause\",\"play-pause\"],\"artwork_file\":\"$RUNTIME/art.png\"}"
  wait_for "no MPRIS player for the phone" eval "playerctl -l 2> /dev/null | grep -qx andromeda_link_$ID"
  shot player
  step "player: the phone plays; shot before any notification covers the header"
  say "notify {\"id\":\"mail-1\",\"app\":\"Gmail\",\"title\":\"Invoice INV-2026-1043\",\"text\":\"Your payment of Rs 13,000 is due on Friday\"}"
  say "call {\"state\":\"ringing\",\"number\":\"+977 980-1234567\",\"name\":\"Bishal Rai\"}"
  sleep 0.5 # real time: the ringing row shows before the call ends unanswered
  say "call {\"state\":\"idle\"}"
  say "notify {\"id\":\"chat-1\",\"app\":\"WhatsApp\",\"title\":\"Ada\",\"text\":\"Are we still on for 6? I can bring the projector\",\"actions\":[{\"id\":\"0\",\"label\":\"Reply\",\"reply\":true}]}"
  mkdir -p "$RUNTIME/src"
  head -c 4200000 /dev/urandom > "$RUNTIME/src/IMG_2201.jpg"
  say "send $RUNTIME/src/IMG_2201.jpg"

  # Reply on the WhatsApp notification in Waiting opens its inline field with the focus.
  shot waiting
  at 600 260
  sleep 0.5 # real time: the field takes the focus
  key "See you at 6"
  key -k Return
  wait_for "the phone never got the reply" grep -q "\"event\":\"notification-action\".*\"reply_text\":\"See you at 6\"" "$OUT/hold.jsonl"
  step "reply: $(grep "\"event\":\"notification-action\"" "$OUT/hold.jsonl" | tail -1)"

  printf "INV-2026-1043, Rs 13,000" | wl-copy
  wait_for "link-session never synced the copy" eval "(( \$(clips) >= 1 ))"
  # Send clipboard on the phone card shares the text with the phone.
  at 120 507
  wait_for "Send clipboard did not reach the phone" grep -q "\"event\":\"received\",\"kind\":\"text\",\"text\":\"INV-2026-1043" "$OUT/hold.jsonl"
  step "send clipboard: the phone received the copied text"
  shot light
  view settings
  shot settings
  step "settings: grants of the connected phone"
  for v in messages calls photos files activity; do view $v; shot $v; done
  step "views: messages, calls (the missed call), photos, files, activity with the phone connected"
  folder() { gdbus call --session -d org.andromeda.Link1 -o /org/andromeda/Link1 -m org.andromeda.Link1.ListFiles "$ID" "$1"; }
  folder "/Internal storage/Download" | grep -q "Lease agreement" || fail "ListFiles did not reach the phone storage"

  msg theme-mode-set dark > /dev/null
  view waiting
  shot dark
  for v in messages calls photos files activity; do view $v; shot $v-dark; done
  step "dark: the same phone after the shell switched to dark"
'
echo "PASS; artifacts: $OUT"
