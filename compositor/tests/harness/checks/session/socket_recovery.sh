#!/usr/bin/env bash
# A deleted Wayland or IPC socket file is recreated, so new clients still connect; a name whose lock another process
# holds is left alone.
set -euo pipefail
path=$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY
appears() { for _ in $(seq 50); do [[ -S $1 ]] && return 0; sleep 0.02; done; echo "$1 not recreated"; exit 1; }
connects() { "$ANDROMEDA_GLOBAL_CLIENT" wl_compositor present 5 > /dev/null || { echo "new client failed ($1)"; exit 1; }; }

connects start
rm -f "$path"
appears "$path"
connects "socket deleted"
rm -f "$path" "$path.lock"
appears "$path"
connects "socket and lock deleted"

rm -f "$ANDROMEDA_SOCKET"
appears "$ANDROMEDA_SOCKET"
[[ $(stat -c %a "$ANDROMEDA_SOCKET") == 600 ]] || { echo "recreated IPC socket is not 0600"; exit 1; }
"$ANDROMEDA" msg config-reload > /dev/null

foreign=$path.lock.foreign
exec 9> "$foreign"
flock -n 9
mv "$foreign" "$path.lock"
rm -f "$path"
for _ in $(seq 50); do grep -q "is held by another process" "$ANDROMEDA_LOG" && break; sleep 0.02; done
grep -q "is held by another process" "$ANDROMEDA_LOG" || { echo "no refusal logged for a foreign lock"; exit 1; }
[[ ! -e $path ]] || { echo "socket recreated under a foreign lock"; exit 1; }
