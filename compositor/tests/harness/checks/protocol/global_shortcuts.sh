#!/usr/bin/env bash
# A global shortcut bound from [global_shortcuts] goes live without a dialog. Its press sends activated and its release
# deactivated, and neither key event reaches the focused window. A config reload that disables it while the key is held
# sends deactivated at once, and the later release still stays away from the window. `global-shortcuts --json` shows
# the disabled shortcut, `global-shortcuts-assign` rebinds it, refuses another app's chord and turns it off, and
# `global-shortcuts-revoke` sends revoked to the bound session.
set -euo pipefail
source "$ANDROMEDA_HARNESS_LIB"

readonly OBSERVER="${ANDROMEDA_SEAT_LOG_CLIENT:-./build-debug/tests/seat-log-client}"
readonly SHELL_CLIENT="${ANDROMEDA_DESKTOP_CLIENT:-./build-debug/tests/desktop-client}"
readonly POINTER="${ANDROMEDA_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
readonly SHELL_LOG="$ANDROMEDA_RUNTIME_DIR/shell-events.log"
readonly WINDOW_LOG="$ANDROMEDA_RUNTIME_DIR/window.log"
readonly SHELL_FIFO="$ANDROMEDA_RUNTIME_DIR/shell-control"
readonly APP="org.test.GlobalShortcuts$$"
readonly KEY_F9=67
readonly BASE_CONFIG="$ANDROMEDA_RUNTIME_DIR/base-config.toml"
cp "$ANDROMEDA_CONFIG" "$BASE_CONFIG"

set_chord() {
  cp "$BASE_CONFIG" "$ANDROMEDA_CONFIG"
  printf '\n[global_shortcuts."%s"]\ntalk = "%s"\n' "$APP" "$1" >> "$ANDROMEDA_CONFIG"
  "$ANDROMEDA" msg config-reload > /dev/null
}

set_chord "Ctrl+F9"
mkfifo "$SHELL_FIFO"
exec {shell_fd}<> "$SHELL_FIFO"
"$OBSERVER" key-window > "$WINDOW_LOG" 2>&1 &
await_events "$WINDOW_LOG" keyboard-enter 1 "the window's keyboard focus"
DESKTOP_CLIENT_SHELL_VERSION=5 "$SHELL_CLIENT" shell-events <&"$shell_fd" > "$SHELL_LOG" 2>&1 &
await_events "$SHELL_LOG" ready 1 "the shell stand-in"

printf 'shortcuts-open %s\nshortcut-add talk Talk -\nshortcuts-bind\n' "$APP" >&"$shell_fd"
await_events "$SHELL_LOG" shortcuts-done 1 "the bind without a dialog"
if ! grep -qx 'shortcut talk Ctrl+F9 0' "$SHELL_LOG"; then
  echo "the shortcut is not live on Ctrl+F9: $(tr '\n' '|' < "$SHELL_LOG")"
  exit 1
fi

"$POINTER" 1280 720 mod ctrl pause 50 key-press "$KEY_F9" pause 300 key-release "$KEY_F9" mod none pause 50 \
  > "$ANDROMEDA_RUNTIME_DIR/pointer-1.log" 2>&1
await_events "$SHELL_LOG" "deactivated talk" 1 "the release"
if [[ $(grep -E '^(activated|deactivated) ' "$SHELL_LOG" | tr '\n' '|') != "activated talk|deactivated talk|" ]]; then
  echo "expected one activated then one deactivated: $(tr '\n' '|' < "$SHELL_LOG")"
  exit 1
fi

"$POINTER" 1280 720 mod ctrl pause 50 key-press "$KEY_F9" pause 2000 key-release "$KEY_F9" mod none pause 50 \
  > "$ANDROMEDA_RUNTIME_DIR/pointer-2.log" 2>&1 &
pointer=$!
await_events "$SHELL_LOG" "activated talk" 2 "the second press"
set_chord ""
await_events "$SHELL_LOG" "deactivated talk" 2 "deactivated when the reload disabled the shortcut mid-press"
if ! kill -0 "$pointer" 2> /dev/null; then
  echo "deactivated came only with the release, not with the reload"
  exit 1
fi
await_events "$SHELL_LOG" "shortcut talk - 2" 1 "the disabled state"
wait "$pointer"

# Settings changes keys through global-shortcuts-assign: it writes settings.toml over config.toml and rebinds live.
"$ANDROMEDA" global-shortcuts-assign "$APP talk Ctrl+F10" > /dev/null
await_events "$SHELL_LOG" "shortcut talk Ctrl+F10 0" 1 "the assigned chord, live without a reload by hand"
"$ANDROMEDA" global-shortcuts-assign "org.test.Other$$ talk Ctrl+F11" > /dev/null
refused=$("$ANDROMEDA" global-shortcuts-assign "$APP talk Ctrl+F11" --json)
if ! jq -e --arg other "org.test.Other$$" '.assigned == false and .reason == "in_use" and .holder == $other' \
    <<< "$refused" > /dev/null; then
  echo "assigning another app's chord was not refused with its holder: $refused"
  exit 1
fi
"$ANDROMEDA" global-shortcuts-assign "$APP talk off" > /dev/null
await_events "$SHELL_LOG" "shortcut talk - 2" 2 "the shortcut turned off by assign"

"$ANDROMEDA" global-shortcuts --json > "$ANDROMEDA_RUNTIME_DIR/inspect.json"
if ! jq -e --arg app "$APP" '[.sessions[] | select(.app == $app and .phase == "bound") | .shortcuts[]
    | select(.id == "talk" and .conflict == "disabled" and .trigger == null)] | length == 1' \
    "$ANDROMEDA_RUNTIME_DIR/inspect.json" > /dev/null; then
  echo "global-shortcuts does not show the disabled shortcut: $(cat "$ANDROMEDA_RUNTIME_DIR/inspect.json")"
  exit 1
fi
"$ANDROMEDA" global-shortcuts-revoke "$APP" > /dev/null
await_events "$SHELL_LOG" shortcuts-revoked 1 "revoked after global-shortcuts-revoke"

printf 'mark done\n' >&"$shell_fd"
await_events "$SHELL_LOG" "mark done" 1 "the shell's round trip"
if [[ $(events "$SHELL_LOG" "deactivated talk") != 2 ]] || grep -q "keyboard-key code=$KEY_F9" "$WINDOW_LOG"; then
  echo "a shortcut key leaked or ended twice: shell=$(tr '\n' '|' < "$SHELL_LOG") window=$(tr '\n' '|' < "$WINDOW_LOG")"
  exit 1
fi
echo "a config shortcut fired activated/deactivated without reaching the window, a reload mid-press ended it once, and revoke unbound it"
