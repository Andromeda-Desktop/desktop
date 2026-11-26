#!/usr/bin/env bash
# No wrong password: the headless shell checks it with the real PAM login stack, and faillock counts it.
# The lock screen survives losing every output, as on a VT switch (wlroots destroys the outputs on pause and creates
# them again on resume): lock, destroy the only output, create it again, then the lock screen draws on it (not the
# compositor's black lock blank), typing redraws it,. Writes back.png and
# noctalia.log to $OUT (default ./artifacts/lock-output-replug).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/lock-output-replug}
source "$(dirname "$0")/lib.sh"
rm -rf "$OUT"
boot_headless 1
cat > "$RUNTIME/home/.config/andromeda/shell/config.toml" <<'TOML'
[shell]
setup_wizard_enabled = false
TOML

with_noctalia '
  fail() { echo "FAIL: $*" >&2; grim "$OUT/fail.png" || true; exit 1; }
  msg() { "$NOCTALIA" msg "$@" 2>&1 || true; }
  locked() { msg status | grep -q "\"locked\": true"; }
  # A capture is black while locked: the compositor log shows each lock surface commit instead.
  lock_frames() { grep -c "lock frame output=HEADLESS-1" "$OUT/andromeda.log" || true; }
  drew_since() { (( $(lock_frames) > $1 )); }

  wtype -s 3600000 &
  sleep 0.3
  [[ $(msg session lock) == ok ]] || fail "lock refused"
  wait_for "did not lock" locked
  wait_for "the lock screen did not draw" drew_since 0

  "$ANDROMEDA" output-destroy HEADLESS-1 > /dev/null || fail "output-destroy failed"
  sleep 1
  n=$(lock_frames)
  "$ANDROMEDA" output-create HEADLESS-1 > /dev/null || fail "output-create failed"
  locked || fail "losing the output unlocked the session"
  wait_for "the lock screen did not draw on the re-created output" drew_since "$n"
  sleep 1

  n=$(lock_frames)
  wtype abcdef
  wait_for "the lock screen did not redraw after typing" drew_since "$n"
  locked || fail "the session unlocked"
  echo "PASS"
'
