#!/usr/bin/env bash
# Maximize, float, and unfloat keeps the tile maximized until the maximize binding restores its original tile size.
# Repeated round trips must also preserve the separate floating restore box.
set -euo pipefail

source "$ANDROMEDA_HARNESS_LIB"

readonly CLIENT="${ANDROMEDA_FRACTIONAL_CLIENT:-./build-debug/tests/fractional-client}"

cat >> "$ANDROMEDA_CONFIG" <<'EOF'

[animation]
enabled = false
duration_ms = 120

[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0

[[window_rule]]
match.title = "^maximize-float-(remembered|floating)-"
default_floating = true
default_floating_size_px = { width = 420, height = 270 }
default_position = { x = 160, y = 120, anchor = "top_left" }
EOF
"$ANDROMEDA" msg config-reload > /dev/null

field() {
  "$ANDROMEDA" windows --json | jq -r --arg title "$TITLE" --arg key "$1" \
    '[.[] | select(.title == $title)] | if length == 0 then "<absent>" else .[0][$key] end'
}


for mode in column edges remembered-column remembered-edges; do
  if [[ $mode == remembered-column ]]; then
    sed -i '/^\[animation\]$/ { n; s/^enabled = false$/enabled = true/; }' "$ANDROMEDA_CONFIG"
    "$ANDROMEDA" msg config-reload > /dev/null
  fi
  TITLE="maximize-float-$mode"
  "$CLIENT" "$TITLE" > "$ANDROMEDA_RUNTIME_DIR/$TITLE.log" 2>&1 &
  client_pid=$!
  await_field "$TITLE" title "$TITLE"
  window_id=$(field id)
  "$ANDROMEDA" msg "window-focus:$window_id" > /dev/null
  float_action=window-toggle-floating
  if [[ $mode == remembered-* ]]; then
    float_action="window-toggle-floating:$window_id"
  fi
  if [[ $mode == remembered-* ]]; then
    await_field "$TITLE" floating true
    await_field "$TITLE" w 420
    await_field "$TITLE" h 270
    await_field "$TITLE" x 160
    await_field "$TITLE" y 120
    "$ANDROMEDA" msg "$float_action" > /dev/null
  fi
  await_field "$TITLE" floating false
  "$ANDROMEDA" settle > /dev/null
  tile_width=$(field w)
  tile_height=$(field h)
  restore_width=$tile_width
  restore_height=$tile_height
  if [[ $mode == remembered-* ]]; then
    restore_width=420
    restore_height=270
  fi
  action=window-toggle-maximize
  edges=false
  if [[ $mode == *edges ]]; then
    action=window-toggle-maximize-to-edges
    edges=true
  fi

  "$ANDROMEDA" msg "$action" > /dev/null
  await_field "$TITLE" maximized true
  "$ANDROMEDA" settle > /dev/null
  max_width=$(field w)
  max_height=$(field h)
  if ((max_width <= tile_width)); then
    echo "$TITLE: maximize did not enlarge the tile ($tile_width to $max_width)"
    exit 1
  fi

  "$ANDROMEDA" msg "$float_action" > /dev/null
  await_field "$TITLE" floating true
  await_field "$TITLE" maximized true
  await_field "$TITLE" maximized_to_edges "$edges"
  await_field "$TITLE" w "$max_width"

  # Unfloating must not restore the normal size automatically. Only the following maximize binding should do that.
  "$ANDROMEDA" msg "$float_action" > /dev/null
  await_field "$TITLE" floating false
  await_field "$TITLE" maximized true
  await_field "$TITLE" maximized_to_edges "$edges"
  "$ANDROMEDA" settle > /dev/null
  await_field "$TITLE" w "$max_width"
  await_field "$TITLE" h "$max_height"

  # Keep the same restore extent through another full float/unfloat cycle while still maximized.
  "$ANDROMEDA" msg "$float_action" > /dev/null
  await_field "$TITLE" floating true
  await_field "$TITLE" maximized true
  "$ANDROMEDA" msg "$float_action" > /dev/null
  await_field "$TITLE" floating false
  await_field "$TITLE" maximized true
  "$ANDROMEDA" settle > /dev/null
  await_field "$TITLE" w "$max_width"
  await_field "$TITLE" h "$max_height"

  "$ANDROMEDA" msg "$action" > /dev/null
  await_field "$TITLE" maximized false
  await_field "$TITLE" w "$tile_width"
  await_field "$TITLE" h "$tile_height"

  # The maximized floating geometry must not overwrite the remembered normal floating size or position.
  "$ANDROMEDA" msg "$float_action" > /dev/null
  await_field "$TITLE" floating true
  await_field "$TITLE" maximized false
  await_field "$TITLE" w "$restore_width"
  await_field "$TITLE" h "$restore_height"
  if [[ $mode == remembered-* ]]; then
    await_field "$TITLE" x 160
    await_field "$TITLE" y 120
  fi

  # A second cycle must capture the restored size, not a stale maximized configure.
  "$ANDROMEDA" msg "$action" > /dev/null
  await_field "$TITLE" maximized true
  await_field "$TITLE" w "$max_width"
  "$ANDROMEDA" msg "$action" > /dev/null
  await_field "$TITLE" maximized false
  await_field "$TITLE" w "$restore_width"
  await_field "$TITLE" h "$restore_height"

  "$ANDROMEDA" msg "$float_action" > /dev/null
  await_field "$TITLE" floating false
  await_field "$TITLE" w "$tile_width"
  await_field "$TITLE" h "$tile_height"
  kill -TERM "$client_pid"
  wait "$client_pid" 2>/dev/null || true
  await_field "$TITLE" title "<absent>"
done

for mode in floating-column floating-edges; do
  TITLE="maximize-float-$mode"
  "$CLIENT" "$TITLE" > "$ANDROMEDA_RUNTIME_DIR/$TITLE.log" 2>&1 &
  client_pid=$!
  await_field "$TITLE" floating true
  await_field "$TITLE" w 420
  await_field "$TITLE" h 270
  "$ANDROMEDA" msg "window-focus:$(field id)" > /dev/null
  action=window-toggle-maximize
  max_width=1264
  if [[ $mode == *edges ]]; then
    action=window-toggle-maximize-to-edges
    max_width=1280
  fi
  "$ANDROMEDA" msg "$action" > /dev/null
  await_field "$TITLE" maximized true
  "$ANDROMEDA" settle > /dev/null
  await_field "$TITLE" w "$max_width"
  "$ANDROMEDA" msg window-toggle-floating > /dev/null
  await_field "$TITLE" floating false
  await_field "$TITLE" maximized true
  await_field "$TITLE" w "$max_width"
  "$ANDROMEDA" msg "$action" > /dev/null
  await_field "$TITLE" maximized false
  await_field "$TITLE" w 420
  kill -TERM "$client_pid"
  wait "$client_pid" 2>/dev/null || true
  await_field "$TITLE" title "<absent>"
done

echo "maximize, float, and unfloat preserved maximization until the binding restored the original tile extent"
