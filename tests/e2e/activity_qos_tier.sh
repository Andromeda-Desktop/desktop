#!/usr/bin/env bash
# P8 (docs/apps.md §8): Settings › Activity shows the compositor's QoS tier of each app. A foot window runs in a real
# transient app scope; the compositor, the mock systemd and the shell share boot_headless's private bus, so the
# shell reads org.andromeda.Compositor1.Apps of this headless compositor, not the session's one.
# Fails when:
#   - the tier-test row has no tier while its window shows,
#   - with Efficiency Mode on, the row hides to a tier other than `hidden-long`,
#   - with Efficiency Mode off again, the row leaves `visible` after its workspace is switched away: Balanced on mains
#     never throttles (docs/qos.md; test hidden delay 1 s).
# Writes the dumps and logs to $OUT (default ./artifacts/activity-qos-tier).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/activity-qos-tier}
source "$(dirname "$0")/lib.sh"
REAL_RUNTIME=/run/user/$(id -u)
SCOPE="app-tier\\x2dtest-$$.scope"
MOCK="$(cd "$(dirname "$0")" && pwd)/mock_systemd1.py"
fail() { echo "FAIL: $*"; exit 1; }

trap 'XDG_RUNTIME_DIR=$REAL_RUNTIME systemctl --user stop "$SCOPE" > /dev/null 2>&1 || true; kill $(jobs -p) 2> /dev/null || true; wait 2> /dev/null; rm_runtime' EXIT
export ANDROMEDA_QOS_TEST_DELAYS_MS=1000,1500
boot_headless 1
BUS=$BUS_ADDRESS

onbus() { run env DBUS_SESSION_BUS_ADDRESS="$BUS" DBUS_SYSTEM_BUS_ADDRESS="$BUS" "$@"; }
onbus env REAL_RUNTIME="$REAL_RUNTIME" python3 "$MOCK" "$OUT/calls.txt" > "$OUT/mock.log" 2>&1 &
WAIT_S=5 wait_for "the mock systemd did not start" grep -q ready "$OUT/mock.log"
onbus "$NOCTALIA" > "$OUT/noctalia.log" 2>&1 &
poll_for 10 onbus "$NOCTALIA" msg settings-close > /dev/null 2>&1 || true

XDG_RUNTIME_DIR=$REAL_RUNTIME systemd-run --user --quiet --scope --unit="$SCOPE" \
  env XDG_RUNTIME_DIR="$RUNTIME" WAYLAND_DISPLAY=wayland-0 foot --config=/dev/null --app-id=tier-test --title=tier-test sleep 600 \
  > /dev/null 2>&1 &
cgroup=""
scope_up() { cgroup=$(XDG_RUNTIME_DIR=$REAL_RUNTIME systemctl --user show -P ControlGroup "$SCOPE" 2> /dev/null || true); [[ -n $cgroup ]]; }
WAIT_S=5 wait_for "the foot scope did not start" scope_up
onbus dbus-send --session --print-reply --dest=org.freedesktop.systemd1 /org/freedesktop/systemd1 \
  org.andromeda.MockSystemd.StartWithCgroup string:"$SCOPE" string:app.slice string:"$cgroup" > /dev/null

onbus "$NOCTALIA" msg activity-open tier-test > /dev/null
tier() {
  onbus "$NOCTALIA" msg activity-dump > "$OUT/dump.json"
  jq -r '[.rows[] | select(.app_id == "tier-test") | .tier][0] // ""' "$OUT/dump.json"
}
shown=""
tier_shown() { shown=$(tier); [[ $shown == focused || $shown == visible ]]; }
poll_for 6 tier_shown || true
[[ $shown == focused || $shown == visible ]] || fail "tier-test shows tier '$shown' while its window shows"
echo "PASS shown window: tier $shown"


# Efficiency Mode: the hold takes the app to T3 (hidden-long) at once when it hides, not `hidden` first. The focused
# app is exempt, and a shell window does not take that role, so the check starts when the window hides.
export E2E="$(cd "$(dirname "$0")" && pwd)" POINTER=$ROOT/compositor/build-debug/tests/pointer-client
click() { run env ANDROMEDA="$ANDROMEDA" NOCTALIA="$NOCTALIA" bash "$E2E/activity_click.sh" "$1"; }
click "Efficiency Mode"
run "$ANDROMEDA" msg workspace-switch:2 > /dev/null
tier_hidden() { shown=$(tier); [[ $shown == hidden* ]]; }
poll_for 6 tier_hidden || true
[[ $shown == hidden-long ]] || fail "tier-test shows tier '$shown' when hidden with Efficiency Mode on"
echo "PASS Efficiency Mode on: tier hidden-long"
run "$ANDROMEDA" msg workspace-switch:1 > /dev/null
poll_for 6 tier_shown || true
click "Efficiency Mode"

run "$ANDROMEDA" msg workspace-switch:2 > /dev/null
# Balanced on mains (no System1 on the test bus) never throttles: past the 1 s hidden delay the row stays visible.
end=$((SECONDS + 3))
while ((SECONDS < end)); do
  shown=$(tier)
  [[ $shown == hidden* ]] && fail "tier-test shows tier '$shown' on Balanced mains after its workspace was switched away"
  sleep 0.1
done
[[ $shown == visible ]] || fail "tier-test shows tier '$shown' after its workspace was switched away"
echo "PASS hidden window on Balanced mains: tier visible"
