#!/usr/bin/env bash
# dsk_shell_v1 freeze_frame holds each output's frame over a look change until thaw_frame (or 1 s), then fades it out.
set -euo pipefail
source "$ANDROMEDA_HARNESS_LIB"
fail() { echo "$1"; exit 1; }

readonly SHELL_LOG="$ANDROMEDA_RUNTIME_DIR/theme-freeze-shell.log"
readonly SCREENSHOT="$ANDROMEDA_RUNTIME_DIR/theme-freeze.png"

cat >> "$ANDROMEDA_CONFIG" <<'CONFIG'

[colors]
backdrop = "#00FF00FF"
CONFIG
"$ANDROMEDA" msg config-reload > /dev/null
"$ANDROMEDA" settle > /dev/null

backdrop() {
  sed -i "s/^backdrop = .*/backdrop = \"#$1\"/" "$ANDROMEDA_CONFIG"
  "$ANDROMEDA" msg config-reload > /dev/null
  "$ANDROMEDA" settle > /dev/null
}
red_green() {
  grim -o HEADLESS-1 "$SCREENSHOT"
  magick "$SCREENSHOT" -crop 1x1+40+40 +repage -format "%[fx:int(255*u.r)] %[fx:int(255*u.g)]\n" info:
}
expect() {
  local want=$1 what=$2 red green
  read -r red green < <(red_green)
  case $want in
    green) ((green > 240 && red < 15)) || fail "$what: want the green frame, got red $red green $green" ;;
    red) ((red > 240 && green < 15)) || fail "$what: want the red backdrop, got red $red green $green" ;;
  esac
}

mkfifo "$ANDROMEDA_RUNTIME_DIR/theme-freeze.fifo"
exec {shell_fd}<> "$ANDROMEDA_RUNTIME_DIR/theme-freeze.fifo"
DESKTOP_CLIENT_SHELL_VERSION=7 "$ANDROMEDA_DESKTOP_CLIENT" shell-events <&"$shell_fd" > "$SHELL_LOG" 2>&1 &
await_lines "$SHELL_LOG" ready 1

expect green "before the freeze"
echo "freeze 1" >&"$shell_fd"
await_lines "$SHELL_LOG" 'frozen 1' 1
backdrop FF0000FF
expect green "held over the switch"
echo "thaw 7" >&"$shell_fd"
"$ANDROMEDA" settle > /dev/null
expect green "after a thaw with another serial"
echo "thaw 1" >&"$shell_fd"
shows_red() { local r g; read -r r g < <(red_green); ((r > 240 && g < 15)); }
await_until 120 0.025 shows_red || fail "thaw 1 did not fade the held frame out"

backdrop 00FF00FF
echo "freeze 2" >&"$shell_fd"
await_lines "$SHELL_LOG" 'frozen 2' 1
backdrop FF0000FF
expect green "held over the second switch"
await_until 120 0.025 shows_red || fail "the held frame outlived its 1 s timeout"
