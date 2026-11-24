#!/usr/bin/env bash
# harness: outputs=2
# A scratchpad assigned to an enabled output follows that output when its
# layout position changes. Its output-local geometry and backdrop move with it.
set -euo pipefail

source "$ANDROMEDA_HARNESS_LIB"

readonly CLIENT="${ANDROMEDA_FRACTIONAL_CLIENT:-./build-debug/tests/fractional-client}"
readonly POINTER="${ANDROMEDA_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
readonly BASELINE="$(< "$ANDROMEDA_CONFIG")"
readonly TITLE=scratchpad-output-reposition
readonly NAME=reposition
readonly FIRST_SHOT="$ANDROMEDA_RUNTIME_DIR/reposition-first.png"
readonly SECOND_SHOT="$ANDROMEDA_RUNTIME_DIR/reposition-second.png"

write_config() {
  local second_mode=${3:-1280x720}
  local second_scale=${4:-1.0}
  {
    printf '%s\n' "$BASELINE"
    printf '\n[animation]\nenabled = false\n'
    printf '\n[animation.scratchpad]\nenabled = false\ndim = 0.5\nblur = false\n'
    printf '\n[colors]\nbackdrop = "#FFFFFFFF"\n'
    printf '\n[appearance]\nborder_width = 0\nouter_border_width = 0\ncorner_radius = 0\n'
    printf '\n[output.HEADLESS-1]\nenabled = true\nmode = "1280x720"\nposition = [%s, 0]\nworkspaces = ["LEFT"]\n' "$1"
    printf '\n[output.HEADLESS-2]\nenabled = true\nmode = "%s"\nscale = %s\nposition = [%s, 0]\nworkspaces = ["RIGHT"]\n' "$second_mode" "$second_scale" "$2"
    printf '\n[[scratchpad]]\nname = "%s"\n' "$NAME"
    printf '\n[[window_rule]]\nmatch.title = "^%s$"\ndefault_output = "HEADLESS-2"\ndefault_floating = true\ndefault_floating_size_px = { width = 360, height = 220 }\ndefault_position = { x = 240, y = 180, anchor = "top_left" }\n' "$TITLE"
  } > "$ANDROMEDA_CONFIG"
}

windows() { "$ANDROMEDA" windows --json; }

field_of() {
  windows | jq -r --arg title "$TITLE" --arg field "$1" '.[] | select(.title == $title) | .[$field]'
}


output_origin() {
  "$ANDROMEDA" outputs \
    | awk -v name="$1" '$1 == name { found = 1; next } found && /Position:/ { split($2, p, ","); print p[1], p[2]; exit }'
}

output_origin_is() {
  local actual_x actual_y
  read -r actual_x actual_y < <(output_origin "$1")
  [[ $actual_x == "$2" && $actual_y == "$3" ]]
}

expect_output_origin() {
  await_until "$AWAIT_TRIES" "$AWAIT_STEP" output_origin_is "$1" "$2" "$3" && return 0
  echo "expected $1 at $2,$3, got $(output_origin "$1")"
  "$ANDROMEDA" outputs
  return 1
}

sample_rgb() {
  local image=$1 x=$2 y=$3
  magick "$image" -crop "20x20+$((x - 10))+$((y - 10))" \
    -format '%[fx:round(255*mean.r)] %[fx:round(255*mean.g)] %[fx:round(255*mean.b)]' info:
}

assert_content() {
  local label=$1 image=$2 red green blue
  read -r red green blue <<< "$(sample_rgb "$image" 420 290)"
  if ((red > 30 || green < 80 || blue < 80)); then
    echo "$label does not contain the scratchpad client: red=$red green=$green blue=$blue"
    return 1
  fi
}

assert_no_content() {
  local label=$1 image=$2 red green blue
  read -r red green blue <<< "$(sample_rgb "$image" 420 290)"
  if ((red < 200)); then
    echo "$label still contains the scratchpad client or backdrop: red=$red green=$green blue=$blue"
    return 1
  fi
}

assert_dimmed() {
  local label=$1 image=$2 red green blue
  read -r red green blue <<< "$(sample_rgb "$image" 20 20)"
  if ((red < 100 || red > 160 || green < 100 || green > 160 || blue < 100 || blue > 160)); then
    echo "$label backdrop is not dimmed: red=$red green=$green blue=$blue"
    return 1
  fi
}

assert_bright() {
  local label=$1 image=$2 red green blue
  read -r red green blue <<< "$(sample_rgb "$image" 20 20)"
  if ((red < 230 || green < 230 || blue < 230)); then
    echo "$label backdrop is not clear: red=$red green=$green blue=$blue"
    return 1
  fi
}

write_config 0 1280
"$ANDROMEDA" msg config-reload > /dev/null
expect_output_origin HEADLESS-1 0 0
expect_output_origin HEADLESS-2 1280 0
"$POINTER" 2560 720 move 1920 360

"$CLIENT" "$TITLE" > "$ANDROMEDA_RUNTIME_DIR/$TITLE.log" 2>&1 &
await_count 1
"$ANDROMEDA" msg "window-focus:$(field_of id)" > /dev/null
"$ANDROMEDA" msg "window-move-to-scratchpad:$NAME" > /dev/null
"$ANDROMEDA" msg "scratchpad-toggle:$NAME" > /dev/null
await_field "$TITLE" scratchpad "$NAME"
await_field "$TITLE" workspace ""
await_field "$TITLE" x 1520
await_field "$TITLE" y 180
await_field "$TITLE" w 360
await_field "$TITLE" h 220
await_field "$TITLE" active true

grim -o HEADLESS-2 "$FIRST_SHOT"
assert_content "HEADLESS-2 before the layout change" "$FIRST_SHOT"
assert_dimmed "HEADLESS-2 before the layout change" "$FIRST_SHOT"

# Changing only the scale must preserve keyboard focus on the visible
# scratchpad instead of selecting or clearing the workspace focus beneath it.
write_config 0 1280 1280x720 1.25
"$ANDROMEDA" msg config-reload > /dev/null
expect_output_origin HEADLESS-2 1280 0
await_field "$TITLE" x 1472
await_field "$TITLE" y 144
await_field "$TITLE" active true

# Swap the live outputs without touching the scratchpad. The same Output object
# remains its owner, so its output-local geometry must stay at 240,180.
write_config 1280 0
"$ANDROMEDA" msg config-reload > /dev/null
expect_output_origin HEADLESS-1 1280 0
expect_output_origin HEADLESS-2 0 0
await_field "$TITLE" scratchpad "$NAME"
await_field "$TITLE" workspace ""
await_field "$TITLE" x 240
await_field "$TITLE" y 180
await_field "$TITLE" w 360
await_field "$TITLE" h 220

grim -o HEADLESS-1 "$FIRST_SHOT"
grim -o HEADLESS-2 "$SECOND_SHOT"
assert_no_content "HEADLESS-1 after the layout change" "$FIRST_SHOT"
assert_bright "HEADLESS-1 after the layout change" "$FIRST_SHOT"
assert_content "HEADLESS-2 after the layout change" "$SECOND_SHOT"
assert_dimmed "HEADLESS-2 after the layout change" "$SECOND_SHOT"

"$ANDROMEDA" msg "scratchpad-focus-next:$NAME" > /dev/null
await_field "$TITLE" active true

# A maximized scratchpad tracks a live mode change, while its saved floating
# box retains the same proportional output-local position.
"$ANDROMEDA" msg window-toggle-maximize-to-edges > /dev/null
await_field "$TITLE" x 0
await_field "$TITLE" y 0
await_field "$TITLE" w 1280
await_field "$TITLE" h 720
write_config 960 0 960x540
"$ANDROMEDA" msg config-reload > /dev/null
expect_output_origin HEADLESS-1 960 0
expect_output_origin HEADLESS-2 0 0
await_field "$TITLE" x 0
await_field "$TITLE" y 0
await_field "$TITLE" w 960
await_field "$TITLE" h 540
"$ANDROMEDA" msg "scratchpad-focus-next:$NAME" > /dev/null
await_field "$TITLE" active true
"$ANDROMEDA" msg window-toggle-maximize-to-edges > /dev/null
await_field "$TITLE" x 180
await_field "$TITLE" y 135
await_field "$TITLE" w 360
await_field "$TITLE" h 220

"$ANDROMEDA" msg "window-restore-from-scratchpad:$NAME" > /dev/null
await_field "$TITLE" scratchpad ""
for _ in $(seq 80); do
  workspace=$(field_of workspace)
  [[ $workspace == HEADLESS-2:* ]] && break
  sleep 0.1
done
if [[ $workspace != HEADLESS-2:* ]]; then
  echo "restored window lost its saved HEADLESS-2 workspace: $(windows)"
  exit 1
fi

echo "scratchpad geometry, backdrop, and focus followed enabled output changes"
