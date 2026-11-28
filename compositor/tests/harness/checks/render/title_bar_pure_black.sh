#!/usr/bin/env bash
# Pure black (rulebook §4.3): the dark title bar header keeps its CIE L* distance from view; view goes to black.
set -euo pipefail
source "$(dirname "$ANDROMEDA_HARNESS_LIB")/title_bar.sh"
cat >> "$ANDROMEDA_CONFIG" <<'EOF'
[animation]
enabled = false
[appearance]
scheme = "dark"
pure_black = false
border_width = 0
outer_border_width = 0
corner_radius = 0
[appearance.shadow]
enabled = false
[appearance.title_bar]
mode = "on"
height = 28
[[window_rule]]
match.title = "^ssd-float"
default_floating = true
default_floating_size_px = { width = 480, height = 300 }
default_position = { x = 173, y = 109, anchor = "top_left" }
EOF
"$ANDROMEDA" msg config-reload > /dev/null

env XDG_DECORATION=server "$ANDROMEDA_FRACTIONAL_CLIENT" ssd-float-black 480 300 > "$ANDROMEDA_RUNTIME_DIR/ssd.log" 2>&1 &
title_bar_wait_box ssd-float-black 480x300+173+109
title_bar_wait_field ssd-float-black title_bar true

header_is() {
  local want_r=$1 want_g=$2 want_b=$3 image=$ANDROMEDA_RUNTIME_DIR/header.png red green blue
  "$ANDROMEDA" settle > /dev/null
  grim -o HEADLESS-1 "$image"
  read -r red green blue < <("$ANDROMEDA_PIXEL_PROBE" "$image" pixel 180 85)
  awk -v r="$red" -v g="$green" -v b="$blue" -v wr="$want_r" -v wg="$want_g" -v wb="$want_b" \
    'function d(x) { return x < 0 ? -x : x } BEGIN { exit !(d(r - wr) <= 2 && d(g - wg) <= 2 && d(b - wb) <= 2) }' \
    || { echo "header $red $green $blue, want $want_r $want_g $want_b"; return 1; }
}

# The dark header and view tokens, and the header lowered by the L* of view (scaled in linear sRGB).
read -r header lowered < <(python3 -I - "$(dirname "$ANDROMEDA_HARNESS_LIB")/../../../protocol/design-tokens.toml" <<'PY'
import sys, tomllib
dark = tomllib.load(open(sys.argv[1], "rb"))["scheme"]["dark"]
rgb = lambda h: [int(h[i:i + 2], 16) / 255 for i in (1, 3, 5)]
lin = lambda c: c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4
enc = lambda c: 12.92 * c if c <= 0.0031308 else 1.055 * c ** (1 / 2.4) - 0.055
y = lambda c: sum(w * lin(v) for w, v in zip((0.2126, 0.7152, 0.0722), c))
lstar = lambda v: 116 * v ** (1 / 3) - 16 if v > 216 / 24389 else v * 24389 / 27
yof = lambda l: ((l + 16) / 116) ** 3 if l > 8 else l * 27 / 24389
head = rgb(dark["header"])
scale = yof(lstar(y(head)) - lstar(y(rgb(dark["view"])))) / y(head)
print(",".join(str(round(v * 255)) for v in head), ",".join(str(round(enc(lin(v) * scale) * 255)) for v in head))
PY
)
header_is ${header//,/ }
sed -i 's/^pure_black = false$/pure_black = true/' "$ANDROMEDA_CONFIG"
"$ANDROMEDA" msg config-reload > /dev/null
header_is ${lowered//,/ }
