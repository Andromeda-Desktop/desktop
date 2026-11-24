#!/usr/bin/env bash
# The shell serves org.freedesktop.impl.portal.GlobalShortcuts on a compositor session: a stand-in frontend binds a
# shortcut that [global_shortcuts] assigns Ctrl+F9 (no approval page), `noctalia msg global-shortcuts` lists it, and a
# Ctrl+F9 press emits Activated then Deactivated for that session. A caller that does not own
# org.freedesktop.portal.Desktop is refused. Writes frontend.txt and list.txt to $OUT (default ./artifacts/global-shortcuts).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/global-shortcuts}
source "$(dirname "$0")/lib.sh"
boot_headless 1
printf '\n[global_shortcuts."e2e.app"]\ntoggle = "Ctrl+F9"\n' >> "$RUNTIME/andromeda.toml"

FRONTEND="$ROOT/tests/e2e/global_shortcuts_frontend.py" with_noctalia '
  WAIT_S=5
  "$ANDROMEDA" msg config-reload > /dev/null
  denied=$(gdbus call --session --dest org.freedesktop.impl.portal.desktop.andromeda.shell \
    --object-path /org/freedesktop/portal/desktop --method org.freedesktop.impl.portal.GlobalShortcuts.CreateSession \
    /org/freedesktop/portal/desktop/request/1_1/x /org/freedesktop/portal/desktop/session/1_1/x e2e.app "{}" 2>&1 || true)
  [[ $denied == *AccessDenied* ]] || { echo "FAIL: a non-frontend caller was served: $denied"; exit 1; }

  wtype -s 3600000 &
  python3 "$FRONTEND" e2e.app toggle "Toggle the thing" > "$OUT/frontend.txt" 2>&1 &
  frontend=$!
  wait_for "the bind did not answer" eval "grep -q ^BOUND \"$OUT/frontend.txt\""
  grep -qx "BOUND 0 toggle=Ctrl+F9" "$OUT/frontend.txt" || { echo "FAIL: $(cat "$OUT/frontend.txt")"; exit 1; }
  "$NOCTALIA" msg global-shortcuts > "$OUT/list.txt"
  wtype -M ctrl -k F9 -m ctrl
  wait "$frontend"
'
cat "$OUT/frontend.txt" "$OUT/list.txt"
grep -qx "e2e.app toggle Toggle the thing" "$OUT/list.txt" || { echo "FAIL: shortcut not listed"; exit 1; }
[[ $(grep ^SIGNAL "$OUT/frontend.txt" | tr '\n' '|') == "SIGNAL Activated toggle|SIGNAL Deactivated toggle|" ]] \
  || { echo "FAIL: no Activated then Deactivated"; exit 1; }
echo "PASS; artifacts: $OUT"
