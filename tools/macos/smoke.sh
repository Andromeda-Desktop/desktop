#!/usr/bin/env bash
# Runs on a Mac: checks the bundle signature, starts the app, runs a command in its shell through `ctl`,
# restarts the app and checks that ptyd kept the session. Logs and screenshots go to OUT.
# Usage: tools/macos/smoke.sh [APP] [OUT]
set -euo pipefail
app=${1:-/var/tmp/andromeda-macos/Andromeda Terminal.app}
out=${2:-/var/tmp/andromeda-macos/smoke}
exe=$app/Contents/MacOS/andromeda-terminal
mkdir -p "$out"
codesign --verify --deep --strict "$app"

# Runs the command every 0.2 s until it succeeds or SECONDS pass.
wait_for() {
  local tries=$(($1 * 5))
  shift
  while ! "$@" > /dev/null 2>&1; do
    tries=$((tries - 1))
    [ $tries -gt 0 ] || return 1
    sleep 0.2
  done
}
fail() {
  echo "smoke: $1" >&2
  tail -n 40 "$out"/terminal-*.log >&2 || true
  screencapture -x "$out/failure.png" || true
  exit 1
}
first_session() { "$exe" ctl ls | jq -er '.[0].id'; }
# The typed line holds $((6*7)); only a shell that ran it prints 42.
has_output() { "$exe" ctl get-text --extent all "$1" | grep -q 'andromeda-42'; }

ANDROMEDA_PROFILE=1 "$exe" > "$out/terminal-1.log" 2>&1 &
pid=$!
wait_for 30 first_session || fail "no session over the control socket"
id=$(first_session)
"$exe" ctl send-text "$id" $'echo andromeda-$((6*7))\n'
wait_for 10 has_output "$id" || fail "the shell did not run the typed command"
screencapture -x "$out/window.png" || echo "smoke: no screenshot (screen recording permission)"
pgrep -x andromeda-ptyd > /dev/null || fail "no ptyd"

kill "$pid"
wait "$pid" || true
pgrep -x andromeda-ptyd > /dev/null || fail "ptyd exited with the app"

ANDROMEDA_PROFILE=1 "$exe" > "$out/terminal-2.log" 2>&1 &
pid=$!
wait_for 30 first_session || fail "no session after the restart"
wait_for 10 has_output "$(first_session)" || fail "the restarted app lost the session output"
screencapture -x "$out/restarted.png" || true
kill "$pid"
wait "$pid" || true
echo "smoke: ok"
