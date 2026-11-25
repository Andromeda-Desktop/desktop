#!/usr/bin/env bash
# Privileged globals go only to trusted binaries (installed table or --trust-binary): a copy of global-client outside the
# trusted paths sees none of them but keeps every ordinary global, and the trusted binary sees them on each new connection.
set -euo pipefail

readonly GLOBAL_CLIENT="${ANDROMEDA_GLOBAL_CLIENT:-./build-debug/tests/global-client}"
readonly UNTRUSTED="$ANDROMEDA_RUNTIME_DIR/untrusted-global-client"
cp "$GLOBAL_CLIENT" "$UNTRUSTED"

for global in dsk_shell_v1 zwlr_screencopy_manager_v1 ext_data_control_manager_v1 zwp_virtual_keyboard_manager_v1 \
  ext_foreign_toplevel_list_v1 dsk_output_manager_v1; do
  "$UNTRUSTED" "$global" absent
  "$GLOBAL_CLIENT" "$global" present
  "$GLOBAL_CLIENT" "$global" present
done
"$UNTRUSTED" wl_compositor present
"$UNTRUSTED" xdg_wm_base present
