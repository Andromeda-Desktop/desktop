#!/usr/bin/env bash
# With a desktop shell bound, the shell owns the configuration notice and the session-quit confirmation: diagnostics
# reach it as a dsk_shell_v1 "config-diagnostics" action and no compositor banner draws; session-quit reaches it as
# "power-overlay end logout" and the compositor keeps running, also on a second press.
set -euo pipefail
source "$ANDROMEDA_HARNESS_LIB"

readonly SHELL_CLIENT="${ANDROMEDA_DESKTOP_CLIENT:-./build-debug/tests/desktop-client}"
readonly SHELL_LOG="$ANDROMEDA_RUNTIME_DIR/shell-dialogs.log"
readonly SHOT=$ANDROMEDA_RUNTIME_DIR/shell-dialogs.png
readonly WARNING='#F5C96B'

"$SHELL_CLIENT" shell-events > "$SHELL_LOG" 2>&1 &
await_events "$SHELL_LOG" ready 1 "the shell stand-in"
await_events "$SHELL_LOG" "action config-diagnostics$" 1 "the diagnostics sent on bind"

printf '\n[appearance]\nnot_a_real_key = true\n' >> "$ANDROMEDA_CONFIG"
"$ANDROMEDA" msg config-reload > /dev/null
await_events "$SHELL_LOG" "action config-diagnostics warning .*not_a_real_key" 1 "the warning sent to the shell"
"$ANDROMEDA" settle > /dev/null
grim -o HEADLESS-1 "$SHOT"
drawn=$(magick "$SHOT" -crop 1280x6+0+22 +repage txt: | grep -c "$WARNING" || true)
if (( drawn > 0 )); then
  echo "the compositor drew its banner while a shell is bound: $drawn warning pixels"
  exit 1
fi

"$ANDROMEDA" msg session-quit > /dev/null
"$ANDROMEDA" msg session-quit > /dev/null
await_events "$SHELL_LOG" "action power-overlay end logout$" 2 "session-quit sent to the shell"
"$ANDROMEDA" settle > /dev/null
if ! "$ANDROMEDA" windows > /dev/null 2>&1; then
  echo "the compositor quit although the shell owns the confirmation"
  exit 1
fi
echo "diagnostics and session-quit reached the shell; no compositor banner or quit"
