#!/usr/bin/env bash
# Language & Region settings reach org.freedesktop.locale1: a mock locale1 on the test's private system bus records
# each call while locale_set (built against the shell's own LocaleService client) sets an X11 keyboard layout, and
# the running Noctalia fork's Settings > Language & Region page is screenshotted before and after. Writes artifacts
# to $OUT (default ./artifacts/language).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/language}
source "$(dirname "$0")/lib.sh"
boot_headless 1

SRC=$ROOT/shell
g++ -std=c++23 -I"$SRC/src" -I"$ROOT/libdesk-system/src" -I"$ROOT/libdesk-ui/src" -I"$ROOT/libdesk-ui" -o "$RUNTIME/locale-set" \
  "$(dirname "$0")/locale_set.cpp" "$ROOT/tests/e2e/log_platform_none.cpp" \
  -Wl,--start-group "$SRC/build-debug/subprojects/libdesk-system/libdesk_system.a" "$SRC/build-debug/subprojects/libdesk-ui/libdesk_ui.a" -Wl,--end-group -lsdbus-c++ -lsystemd

MOCK="$(cd "$(dirname "$0")" && pwd)/mock_locale1.py"
with_noctalia '
  python3 '"$MOCK"' "$OUT/calls.txt" > "$OUT/mock.log" 2>&1 &
  mock=$!
  poll_for 5 grep -q ready "$OUT/mock.log" || { echo "FAIL: mock locale1 did not start: $(cat "$OUT/mock.log")"; exit 1; }

  "$NOCTALIA" msg settings-open language > /dev/null
  sleep 2 # real time: the settings window maps and paints
  grim "$OUT/language-before.png"

  DBUS_SYSTEM_BUS_ADDRESS="$DBUS_SESSION_BUS_ADDRESS" "$RUNTIME/locale-set" | tee "$OUT/locale-set.txt"
  grep -q "^PASS" "$OUT/locale-set.txt" && ! grep -q "^FAIL" "$OUT/locale-set.txt" \
    || { echo "locale-set reported a failure"; exit 1; }
  grep -qx "SetX11Keyboard de  nodeadkeys  True True" "$OUT/calls.txt" \
    || { echo "FAIL: mock never received SetX11Keyboard de/nodeadkeys"; exit 1; }

  sleep 1 # real time: the shells own LocaleService rebuilds the page from PropertiesChanged
  grim "$OUT/language-after.png"
  kill "$mock"
'
[[ -s $OUT/language-before.png && -s $OUT/language-after.png ]] || { echo "no screenshot" >&2; exit 1; }
echo "PASS: locale1.SetX11Keyboard reached and reflected; artifacts: $OUT"
