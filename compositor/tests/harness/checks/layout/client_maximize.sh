#!/usr/bin/env bash
# Client-requested maximize and unmaximize around [layout] client_maximize: a client's maximize
# fills the column by default and the usable-area edges when configured, while an unmaximize
# request exits whichever mode is active instead of following the configured policy.
set -euo pipefail
source "$ANDROMEDA_HARNESS_LIB"

readonly CLIENT="${ANDROMEDA_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly COLUMN_LOG="$ANDROMEDA_RUNTIME_DIR/client-maximize-column.log"
readonly RESTORED_LOG="$ANDROMEDA_RUNTIME_DIR/client-maximize-restored.log"
readonly FIRST_LOG="$ANDROMEDA_RUNTIME_DIR/client-maximize-restored-first.log"
readonly RULED_FIRST_LOG="$ANDROMEDA_RUNTIME_DIR/client-maximize-ruled-first.log"
readonly RULED_AFTER_LOG="$ANDROMEDA_RUNTIME_DIR/client-maximize-ruled-after.log"
readonly OVERRIDE_LOG="$ANDROMEDA_RUNTIME_DIR/client-maximize-override.log"

field_of() {
  "$ANDROMEDA" windows --json | jq -r --arg title "$1" --arg field "$2" '.[] | select(.title == $title) | .[$field]'
}

# The request must come post-opening to pass the gates as client intent.
request_maximize() {
  local title=$1 log=$2
  # RESIZE_FILL_COLOR makes the client follow configured sizes; same value keeps the fill unchanged.
  env LOG_CONFIGURES=1 MAXIMIZE_ON_STDIN=1 RESIZE_FILL_COLOR=0xFF5577AA \
    "$CLIENT" "$title" 640 480 <&"$control_fd" > "$log" 2>&1 &
  CLIENT_PID=$!
  await_lines "$log" mapped 1
  # Queue both bytes up front so the client consumes them back-to-back once its poll loop wakes.
  local committed requested
  committed=$(events "$log" surface-committed)
  requested=$(events "$log" maximize-requested)
  printf s >&"$control_fd"
  printf m >&"$control_fd"
  await_lines "$log" surface-committed $((committed + 1))
  await_lines "$log" maximize-requested $((requested + 1))
}

# Sends one control byte to the live client and waits for a fresh acknowledgement line.
send_command() {
  local byte=$1 log=$2 pattern=$3
  local seen
  seen=$(events "$log" "$pattern")
  printf '%s' "$byte" >&"$control_fd"
  await_lines "$log" "$pattern" $((seen + 1))
}

stop_client() {
  kill -KILL "$CLIENT_PID" 2>/dev/null || true
  wait "$CLIENT_PID" 2>/dev/null || true
}

readonly CONTROL_FIFO="$ANDROMEDA_RUNTIME_DIR/client-maximize-control"
mkfifo "$CONTROL_FIFO"
exec {control_fd}<>"$CONTROL_FIFO"

# honor_restored_maximize accepts a client's opening maximize re-assert (restored-session clients);
# the scrolling extent keeps the tile width distinct from both maximized widths. The key lands in
# the boot config's [general], the file's last table.
cat >> "$ANDROMEDA_CONFIG" <<'EOF'
honor_restored_maximize = true

[animation]
enabled = false

[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0

[layout.scrolling]
default_extent_fraction = 0.5

# The ruled cases below match this rule; every other case in this check must not.
[[window_rule]]
match.title = "^client-maximize-ruled-"
default_maximize = true

EOF
"$ANDROMEDA" msg config-reload > /dev/null

# Policy off: the client's maximize fills the column (1264x704 usable), not the edges.
request_maximize client-maximize-column "$COLUMN_LOG"
await_field client-maximize-column w 1264
await_field client-maximize-column h 704
await_field client-maximize-column x 8
await_field client-maximize-column y 8

# Policy enabled under the still column-maximized client: the unmaximize request must restore the
# tile instead of routing into the edges mode's no-op exit.
cat >> "$ANDROMEDA_CONFIG" <<'EOF'

[layout]
client_maximize = "edges"
EOF
"$ANDROMEDA" msg config-reload > /dev/null

send_command M "$COLUMN_LOG" 'unmaximize-requested$'
await_field client-maximize-column w 628
await_field client-maximize-column h 704

# A compositor-driven column maximize is undone by the client's unmaximize request too.
"$ANDROMEDA" msg window-toggle-maximize > /dev/null
await_field client-maximize-column w 1264
send_command M "$COLUMN_LOG" 'unmaximize-requested$'
await_field client-maximize-column w 628

# With the policy on, the client's own maximize targets the edges.
send_command m "$COLUMN_LOG" 'maximize-requested$'
# IPC x/y report the layout slot, whose cross-axis inset the edges override only fixes at presentation.
await_field client-maximize-column w 1280
await_field client-maximize-column h 720
await_field client-maximize-column x 0

# Leaving the edges with a tile-width column restores the tile in one step.
send_command M "$COLUMN_LOG" 'unmaximize-requested$'
await_field client-maximize-column w 628

# Column-maximize underneath the edges stacks both states; the unmaximize leaves the edges first
# and the full-width column on the next request.
"$ANDROMEDA" msg window-toggle-maximize > /dev/null
await_field client-maximize-column w 1264
"$ANDROMEDA" msg window-toggle-maximize-to-edges > /dev/null
await_field client-maximize-column w 1280
send_command M "$COLUMN_LOG" 'unmaximize-requested$'
await_field client-maximize-column w 1264
send_command M "$COLUMN_LOG" 'unmaximize-requested$'
await_field client-maximize-column w 628
stop_client

# A client that re-asserts maximize while opening (restored session) is honored and lands in the
# edges mode under the policy; its unmaximize request restores the tile in one step, and a fresh
# post-map request re-enters the edges.
env LOG_CONFIGURES=1 MAXIMIZE_ON_STDIN=1 REQUEST_MAXIMIZED_AFTER_MAP=1 RESIZE_FILL_COLOR=0xFF5577AA \
  "$CLIENT" client-maximize-restored 640 480 <&"$control_fd" > "$RESTORED_LOG" 2>&1 &
CLIENT_PID=$!
await_lines "$RESTORED_LOG" mapped 1
await_field client-maximize-restored w 1280
await_field client-maximize-restored h 720
send_command M "$RESTORED_LOG" 'unmaximize-requested$'
await_field client-maximize-restored w 628
send_command m "$RESTORED_LOG" 'maximize-requested$'
await_field client-maximize-restored w 1280
stop_client

# A client that restores maximize before its first commit reaches the same state: the opening configure already carries
# the edge size, so it opens maximized to the edges and one unmaximize restores the tile.
env LOG_CONFIGURES=1 MAXIMIZE_ON_STDIN=1 REQUEST_MAXIMIZED=1 RESIZE_FILL_COLOR=0xFF5577AA \
  "$CLIENT" client-maximize-restored-first 640 480 <&"$control_fd" > "$FIRST_LOG" 2>&1 &
CLIENT_PID=$!
await_lines "$FIRST_LOG" mapped 1
await_field client-maximize-restored-first w 1280
await_field client-maximize-restored-first h 720
send_command M "$FIRST_LOG" 'unmaximize-requested$'
await_field client-maximize-restored-first w 628
stop_client

# A default_maximize rule whose client restores maximize before map opens in the edges above the rule's full-width
# column, so unmaximize is two-step.
env LOG_CONFIGURES=1 MAXIMIZE_ON_STDIN=1 REQUEST_MAXIMIZED=1 RESIZE_FILL_COLOR=0xFF5577AA \
  "$CLIENT" client-maximize-ruled-first 640 480 <&"$control_fd" > "$RULED_FIRST_LOG" 2>&1 &
CLIENT_PID=$!
await_lines "$RULED_FIRST_LOG" mapped 1
await_field client-maximize-ruled-first w 1280
await_field client-maximize-ruled-first h 720
send_command M "$RULED_FIRST_LOG" 'unmaximize-requested$'
await_field client-maximize-ruled-first w 1264
send_command M "$RULED_FIRST_LOG" 'unmaximize-requested$'
await_field client-maximize-ruled-first w 628
stop_client

# The after-map re-assert converges on the same stacked state.
env LOG_CONFIGURES=1 MAXIMIZE_ON_STDIN=1 REQUEST_MAXIMIZED_AFTER_MAP=1 RESIZE_FILL_COLOR=0xFF5577AA \
  "$CLIENT" client-maximize-ruled-after 640 480 <&"$control_fd" > "$RULED_AFTER_LOG" 2>&1 &
CLIENT_PID=$!
await_lines "$RULED_AFTER_LOG" mapped 1
await_field client-maximize-ruled-after w 1280
await_field client-maximize-ruled-after h 720
send_command M "$RULED_AFTER_LOG" 'unmaximize-requested$'
await_field client-maximize-ruled-after w 1264
send_command M "$RULED_AFTER_LOG" 'unmaximize-requested$'
await_field client-maximize-ruled-after w 628
stop_client

# A workspace override sets "column" for that workspace: the client's maximize fills the column again.
cat >> "$ANDROMEDA_CONFIG" <<'EOF'

[[workspace]]
index = 1
layout.client_maximize = "column"
EOF
"$ANDROMEDA" msg config-reload > /dev/null
request_maximize client-maximize-override "$OVERRIDE_LOG"
await_field client-maximize-override w 1264
await_field client-maximize-override h 704
stop_client


# A floating layout places windows as floats, but their client maximize follows the workspace policy too.
cat >> "$ANDROMEDA_CONFIG" <<'EOF'
layout.mode = "floating"
EOF
"$ANDROMEDA" msg config-reload > /dev/null
readonly STACKING_LOG="$ANDROMEDA_RUNTIME_DIR/client-maximize-floating-layout.log"
request_maximize client-maximize-floating-layout "$STACKING_LOG"
await_field client-maximize-floating-layout floating true
await_field client-maximize-floating-layout maximized_to_edges false
await_field client-maximize-floating-layout w 1264
await_field client-maximize-floating-layout h 664
send_command M "$STACKING_LOG" 'unmaximize-requested$'
await_field client-maximize-floating-layout maximized false
"$ANDROMEDA" settle > /dev/null
floating-layout_box() {
  "$ANDROMEDA" windows --json | jq -r '.[] | select(.title == "client-maximize-floating-layout") | "\(.w)x\(.h)+\(.x)+\(.y)"'
}
readonly STACKING_RESTORE=$(floating-layout_box)

sed -i 's/layout.client_maximize = "column"/layout.client_maximize = "edges"/' "$ANDROMEDA_CONFIG"
"$ANDROMEDA" msg config-reload > /dev/null
send_command m "$STACKING_LOG" 'maximize-requested$'
await_field client-maximize-floating-layout maximized_to_edges true
await_field client-maximize-floating-layout w 1280
await_field client-maximize-floating-layout h 680
await_field client-maximize-floating-layout x 0
await_field client-maximize-floating-layout y 40

# Reloading the policy must still let the client leave its active edges state and restore its original box.
sed -i 's/layout.client_maximize = "edges"/layout.client_maximize = "column"/' "$ANDROMEDA_CONFIG"
"$ANDROMEDA" msg config-reload > /dev/null
send_command M "$STACKING_LOG" 'unmaximize-requested$'
await_field client-maximize-floating-layout maximized_to_edges false
await_field client-maximize-floating-layout maximized false
"$ANDROMEDA" settle > /dev/null
if [[ $(floating-layout_box) != "$STACKING_RESTORE" ]]; then
  echo "floating-layout unmaximize did not restore $STACKING_RESTORE: $(floating-layout_box)"
  exit 1
fi

sed -i 's/layout.client_maximize = "column"/layout.client_maximize = "edges"/' "$ANDROMEDA_CONFIG"
"$ANDROMEDA" msg config-reload > /dev/null
# The normal maximize action retains its margin even when the client policy selects the edges.
"$ANDROMEDA" msg window-toggle-maximize > /dev/null
await_field client-maximize-floating-layout maximized true
await_field client-maximize-floating-layout maximized_to_edges false
await_field client-maximize-floating-layout w 1264
send_command M "$STACKING_LOG" 'unmaximize-requested$'
await_field client-maximize-floating-layout maximized false
stop_client

for restored in REQUEST_MAXIMIZED REQUEST_MAXIMIZED_AFTER_MAP; do
  title="client-maximize-floating-layout-$restored"
  log="$ANDROMEDA_RUNTIME_DIR/$title.log"
  env LOG_CONFIGURES=1 MAXIMIZE_ON_STDIN=1 "$restored=1" RESIZE_FILL_COLOR=0xFF5577AA \
    "$CLIENT" "$title" 640 480 <&"$control_fd" > "$log" 2>&1 &
  CLIENT_PID=$!
  await_lines "$log" mapped 1
  await_field "$title" maximized_to_edges true
  await_field "$title" w 1280
  await_field "$title" h 680
  if [[ $restored == REQUEST_MAXIMIZED && $(grep -m1 '^configured-size=' "$log") != configured-size=1280x720 ]]; then
    echo "floating-layout restored maximize did not configure the edges before map: $(cat "$log")"
    exit 1
  fi
  send_command M "$log" 'unmaximize-requested$'
  await_field "$title" maximized_to_edges false
  await_field "$title" maximized false
  stop_client
done


echo "client maximize targets column width by default (1264x704), edges when configured (1280x720) for post-map, restored, and default_maximize-ruled requests, column under a workspace override; unmaximize restores the tile (628x704)"
