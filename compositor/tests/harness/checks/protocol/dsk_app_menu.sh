#!/usr/bin/env bash
# dsk_app_menu_v1: a window's menu tree reaches IPC, and app-menu-activate reaches the client only for an enabled leaf
# of the current tree.
set -euo pipefail
source "$ANDROMEDA_HARNESS_LIB"

readonly CLIENT="${ANDROMEDA_SURFACE_PROTOCOLS_CLIENT:-./build-debug/tests/surface-protocols-client}"
readonly LOG="$ANDROMEDA_RUNTIME_DIR/app-menu.log"

mkfifo "$ANDROMEDA_RUNTIME_DIR/app-menu.fifo"
"$CLIENT" app-menu < "$ANDROMEDA_RUNTIME_DIR/app-menu.fifo" > "$LOG" 2>&1 &
exec {menu_fd}> "$ANDROMEDA_RUNTIME_DIR/app-menu.fifo"
await_events "$LOG" mapped 1 "the menu window"
id=$("$ANDROMEDA" windows --json | jq -r '.[] | select(.title == "app-menu") | .id')
if [[ $("$ANDROMEDA" windows --json | jq -c '.[] | select(.title == "app-menu") | .app_menu') != null ]]; then
  echo "a window with no committed tree reports a menu"
  exit 1
fi

menu_serial() { "$ANDROMEDA" windows --json | jq -r ".[] | select(.id == \"$id\") | .app_menu.serial"; }
activate() { "$ANDROMEDA" app-menu-activate "$id" "$1" "$2" > /dev/null 2>&1; }

printf 'menu a\n' >&"$menu_fd"
await_events "$LOG" 'menu a' 1 "tree a"
serial=$(menu_serial)
labels=$("$ANDROMEDA" app-menu "$id" --json | jq -r '[.items[] | .label] | join(",")')
if [[ $labels != "File,Open,,Print,Edit,Copy" ]]; then
  echo "app-menu reports '$labels'"
  exit 1
fi

activate "$serial" 2 || { echo "Open did not activate"; exit 1; }
await_events "$LOG" 'activated 2' 1 "the Open activation"
for item in 1 3 4 99; do
  if activate "$serial" "$item"; then
    echo "item $item (parent, separator, disabled or unknown) activated"
    exit 1
  fi
done

printf 'menu b\n' >&"$menu_fd"
await_events "$LOG" 'menu b' 1 "tree b"
new_serial=$(menu_serial)
if [[ $new_serial == "$serial" ]]; then
  echo "a new tree kept serial $serial"
  exit 1
fi
if activate "$serial" 6; then
  echo "an item of the old tree activated"
  exit 1
fi
activate "$new_serial" 7 || { echo "Paste did not activate"; exit 1; }
await_events "$LOG" 'activated 7' 1 "the Paste activation"
if grep -qE '^activated (1|3|4|6|99)$' "$LOG"; then
  echo "the client got a refused activation: $(grep activated "$LOG" | tr '\n' ' ')"
  exit 1
fi

printf 'menu off\n' >&"$menu_fd"
await_events "$LOG" 'menu off' 1 "the empty tree"
if [[ $(menu_serial) != null ]]; then
  echo "an empty commit kept the menu"
  exit 1
fi
echo "dsk app menu: tree over IPC, leaf activation, stale and inert items refused, empty commit clears"
