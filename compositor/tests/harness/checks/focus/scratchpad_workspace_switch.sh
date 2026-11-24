#!/usr/bin/env bash
# Switching workspaces must not steal keyboard focus from a visible scratchpad.
# refocus(Output*) retains a focused, foreground scratchpad across a workspace
# switch; explicit actions use refocusExplicit().
set -euo pipefail

source "$ANDROMEDA_HARNESS_LIB"

readonly CLIENT="${ANDROMEDA_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly SCRATCHPAD=switch
readonly WS1=scratchpad-switch-ws1
readonly WS2=scratchpad-switch-ws2
readonly SCRATCH=scratchpad-switch-foreground

cat >> "$ANDROMEDA_CONFIG" <<'EOF'

[animation]
enabled = false

[[scratchpad]]
name = "switch"

[[window_rule]]
match.title = "^scratchpad-switch-foreground$"
default_floating = true
default_floating_size_px = { width = 420, height = 260 }
EOF
"$ANDROMEDA" msg config-reload > /dev/null

windows() { "$ANDROMEDA" windows --json; }

field_of() {
  windows | jq -r --arg title "$1" --arg field "$2" '.[] | select(.title == $title) | .[$field]'
}

"$CLIENT" "$WS1" > "$ANDROMEDA_RUNTIME_DIR/$WS1.log" 2>&1 &
await_count 1
"$ANDROMEDA" msg workspace-switch:2 > /dev/null
"$CLIENT" "$WS2" > "$ANDROMEDA_RUNTIME_DIR/$WS2.log" 2>&1 &
await_count 2
"$ANDROMEDA" msg workspace-switch:1 > /dev/null
await_field "$WS1" active true

"$CLIENT" "$SCRATCH" 420 260 > "$ANDROMEDA_RUNTIME_DIR/$SCRATCH.log" 2>&1 &
await_count 3
scratch_id=$(field_of "$SCRATCH" id)
"$ANDROMEDA" msg "window-focus:$scratch_id" > /dev/null
"$ANDROMEDA" msg "window-move-to-scratchpad:$SCRATCHPAD" > /dev/null
await_field "$SCRATCH" scratchpad "$SCRATCHPAD"
"$ANDROMEDA" msg "scratchpad-toggle:$SCRATCHPAD" > /dev/null
await_field "$SCRATCH" active true
"$ANDROMEDA" msg "window-focus:$scratch_id" > /dev/null
await_field "$SCRATCH" active true

# The workspace switch under test: focus must stay on the foreground scratchpad.
"$ANDROMEDA" msg workspace-switch:2 > /dev/null
await_field "$SCRATCH" active true
for _ in $(seq 5); do
  sleep 0.1
  if [[ $(field_of "$SCRATCH" active) != true || $(field_of "$WS2" active) != false ]]; then
    echo "workspace switch stole focus from the visible scratchpad: $(windows)"
    exit 1
  fi
done

echo "workspace switch keeps focus on the visible scratchpad"
