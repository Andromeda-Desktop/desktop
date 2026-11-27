#!/usr/bin/env bash
# Glass style matrix: every wallpaper x glass setting x surface gets a screenshot and its frame cost while text scrolls
# behind the glass. Inside a Wayland session it runs nested in a window (that session's GPU and vsync); without one
# (over SSH on the test laptop) it runs headless at MODE on this machine's GPU.
# Usage: tests/glass-style-matrix.sh. Env: OUT, SECONDS_PER (default 4), MODE (headless, default 1920x1080@60), WALLS,
# CONFIGS, SURFACES (space lists), SCHEME (dark|light), FOOT (0: no scrolling text window, the wallpaper shows),
# PHOTO (an image file, the wall named photo).
# Surfaces: desktop launcher cc settings toast menu (the desktop context menu). $OUT/index.html shows every shot.
# Report: $OUT/report.txt, one row per cell: frames, GPU p50/p99 ms (frame log col 4), CPU record p50/p99 ms (col 12),
# GPU execution p50/p99 ms (col 13), mean blur kpx (col 6), compositor and shell CPU %. Shots: $OUT/<wall>/<config>-<surface>.png.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${OUT:-$ROOT/artifacts/glass-style-matrix}
ANDROMEDA=${ANDROMEDA:-$ROOT/compositor/build-live/andromeda}
NOCTALIA=${NOCTALIA:-$ROOT/shell/build-f7/andromeda-shell}
SECONDS_PER=${SECONDS_PER:-4}
WALLS=${WALLS:-"light dark colour text"}
CONFIGS=${CONFIGS:-"rt frosted frosted-noedge frosted-user liquid liquid-clear liquid-tinted"}
SURFACES=${SURFACES:-"desktop launcher cc settings toast"}
MODE=${MODE:-1920x1080@60}
SCHEME=${SCHEME:-dark}
FOOT=${FOOT:-1}
NESTED_PARENT=
if [[ -n ${WAYLAND_DISPLAY:-} ]]; then
  NESTED_PARENT=$WAYLAND_DISPLAY
  [[ $NESTED_PARENT == /* ]] || NESTED_PARENT=$XDG_RUNTIME_DIR/$NESTED_PARENT
fi
rm -rf "$OUT"
mkdir -p "$OUT"
source "$ROOT/tests/e2e/lib.sh"
export ANDROMEDA_FRAME_LOG=$OUT/frames.log

WALLDIR=$RUNTIME/walls
mkdir -p "$WALLDIR"
magick -size 1280x720 gradient:'#f4f1ea-#d9d2c6' -fill '#ffffff' -draw 'circle 900,200 900,420' -blur 0x30 "$WALLDIR/light.png"
cp "$ROOT/shell/assets/andromeda-wallpaper.png" "$WALLDIR/dark.png"
# Warm beige with sharp diagonal light bands and a lavender corner: the bands show how the bezel bends an edge.
magick -size 1920x1080 gradient:'#d8c8b8-#8a7a72' \
  \( -size 2400x2400 radial-gradient:'#7a6e8c-none' -geometry +700+0 \) -composite \
  -stroke '#00000040' -strokewidth 60 -draw 'line 1060,-40 -100,1120' -draw 'line 2070,-80 720,1420' -blur 0x18 \
  -stroke '#fbf6ef' -strokewidth 34 -draw 'line 1060,-60 -100,1100' -draw 'line 2050,-100 700,1400' -blur 0x1.5 \
  "$WALLDIR/bands.png"
[[ -z ${PHOTO:-} ]] || magick "$PHOTO" -resize 1920x1080^ -gravity center -extent 1920x1080 "$WALLDIR/photo.png"
magick -size 1280x720 -seed 7 plasma:'#ff3b6b-#2b7bff' -blur 0x8 -modulate 100,160 "$WALLDIR/colour.png"
magick -size 48x48 xc:'#e0306a' -fill '#2b6bff' -draw 'rectangle 0,0 23,23 rectangle 24,24 47,47' -write mpr:t +delete -size 1920x1080 tile:mpr:t "$WALLDIR/grid.png"
magick -size 1920x1080 xc:'#f7f7f5' "$WALLDIR/white.png"
magick -size 1920x1080 xc:'#f7f7f5' -fill '#14161c' -draw 'rectangle 960,0 1919,1079' "$WALLDIR/split.png"
# Fine black-on-white text and a grid: smooth walls hide how the lens bends the backdrop.
magick -size 1280x720 xc:'#fafafa' -stroke '#3060c0' -strokewidth 1 \
  -draw "$(for i in $(seq 0 40 1280); do printf 'line %d,0 %d,720 line 0,%d 1280,%d ' "$i" "$i" "$i" "$i"; done)" \
  -stroke none -fill '#111111' -pointsize 15 \
  -annotate +0+0 "$(for i in $(seq 1 40); do printf '%s\n' 'The quick brown fox jumps over the lazy dog 0123456789 The quick brown fox jumps over the lazy dog'; done)" \
  "$WALLDIR/text.png"

boot_headless 1
[[ -n $NESTED_PARENT ]] || printf '\n[output.HEADLESS-1]\nmode = "%s"\n' "$MODE" >> "$RUNTIME/andromeda.toml"
cp "$RUNTIME/andromeda.toml" "$RUNTIME/base.toml"
printf '\n[theme]\nmode = "%s"\n\n[desktop_icons]\nenabled = true\nstart_corner = "top_right"\n' "$SCHEME" >> "$RUNTIME/home/.config/andromeda/shell/config.toml"
POINTER=${POINTER:-$ROOT/compositor/build-debug/tests/pointer-client}
# No idle power-off: a headless run gets no input for its whole length.
printf '\n[idle]\nbehaviors = []\n' >> "$RUNTIME/home/.config/andromeda/shell/config.toml"
# The [appearance] keys of each glass setting.
cat > "$RUNTIME/configs" <<'CFG'
rt reduce_transparency = true
frosted glass_style = "frosted"|glass = 0.5|frost = 0.5|edge_reflection = true
frosted-noedge glass_style = "frosted"|glass = 0.5|frost = 0.5|edge_reflection = false
frosted-user glass_style = "frosted"|glass = 0.45|frost = 0.05|edge_reflection = true
liquid glass_style = "liquid"|liquid_tint = 0.15|liquid_blur = 0.1|liquid = 0.5
liquid-clear glass_style = "liquid"|liquid_tint = 0.0|liquid_blur = 0.0|liquid = 1.0
liquid-tinted glass_style = "liquid"|liquid_tint = 0.5|liquid_blur = 1.0|liquid = 0.5
liquid-light glass_style = "liquid"|liquid_tint = 0.8|liquid_blur = 0.1|liquid = 0.5
clear glass_style = "clear"|liquid = 0.5
tinted glass_style = "tinted"|edge_reflection = true
CFG
export RUNTIME WALLDIR WALLS CONFIGS SURFACES SECONDS_PER SCHEME FOOT POINTER MODE
with_noctalia '
  mono_ns() { python3 -c "import time; print(time.monotonic_ns())"; }
  ticks() { awk "{print \$14 + \$15}" "/proc/$1/stat" 2> /dev/null || echo 0; }
  settle() { "$NOCTALIA" msg status > /dev/null; "$ANDROMEDA" settle > /dev/null; sleep 0.8; }
  close_all() {
    for p in launcher control-center; do "$NOCTALIA" msg panel-close "$p" > /dev/null 2>&1 || true; done
    "$NOCTALIA" msg settings-close > /dev/null 2>&1 || true
    settle
  }
  comp=$(pgrep -f -n "andromeda -c $RUNTIME/andromeda.toml")
  shellpid=$(pgrep -f -n -x "$NOCTALIA")
  if [[ $FOOT == 1 ]]; then
    foot --maximized -o main.font=monospace:size=11 sh -c "while :; do seq 1 400000; done" > /dev/null 2>&1 &
    sleep 2
  fi
  out_w=${MODE%%x*}
  out_h=${MODE#*x}
  out_h=${out_h%%@*}
  for wall in $WALLS; do
    mkdir -p "$OUT/$wall"
    for config in $CONFIGS; do
      keys=$(awk -v c="$config" "\$1 == c {sub(/^[^ ]+ /, \"\"); gsub(/\\|/, \"\\n\"); print}" "$RUNTIME/configs")
      { cat "$RUNTIME/base.toml"; printf "\n[appearance]\nscheme = \"%s\"\n%s\n" "$SCHEME" "$keys"; } > "$RUNTIME/andromeda.toml"
      "$ANDROMEDA" msg config-reload > /dev/null
      settle
      # A config reload puts the configured wallpaper back, so the wall and the scheme follow it.
      "$NOCTALIA" msg wallpaper-set "$WALLDIR/$wall.png" > /dev/null
      "$NOCTALIA" msg theme-mode-set "$SCHEME" > /dev/null
      settle
      for surface in $SURFACES; do
        case $surface in
          desktop) ;;
          launcher) "$NOCTALIA" msg panel-open launcher > /dev/null ;;
          cc) "$NOCTALIA" msg panel-open control-center > /dev/null ;;
          settings) "$NOCTALIA" msg settings-open appearance > /dev/null ;;
          toast) "$NOCTALIA" msg notification-show "Glass $wall $config" "Body text over the backdrop to read" > /dev/null ;;
          menu) "$POINTER" "$out_w" "$out_h" move $((out_w / 3)) $((out_h / 6)) click 273 > /dev/null ;;
        esac
        settle
        t0=$(mono_ns); c0=$(ticks "$comp"); s0=$(ticks "$shellpid")
        sleep "$SECONDS_PER"
        t1=$(mono_ns); c1=$(ticks "$comp"); s1=$(ticks "$shellpid")
        grim "$OUT/$wall/$config-$surface.png"
        echo "$wall/$config/$surface $t0 $t1 $c0 $c1 $s0 $s1" >> "$OUT/cells.txt"
        [[ $surface == menu ]] && "$POINTER" "$out_w" "$out_h" tap 1 > /dev/null
        close_all
      done
    done
  done
'
hz=$(getconf CLK_TCK)
printf '%-36s %6s %7s %7s %7s %7s %7s %7s %7s %6s %6s\n' cell frames gpu50 gpu99 rec50 rec99 exec50 exec99 blurKpx comp% shell% | tee "$OUT/report.txt"
while read -r name t0 t1 c0 c1 s0 s1; do
  read -r n g50 g99 r50 r99 x50 x99 bk < <(awk -v a="$t0" -v b="$t1" '$1 >= a && $1 < b {n++; g[n] = $4; r[n] = $12; x[n] = $13; bk += $6}
    END {if (n == 0) {print "0 - - - - - - -"; exit}
      asort(g); asort(r); asort(x); i50 = int(n * 0.5); i99 = int(n * 0.99); if (i50 < 1) i50 = 1; if (i99 < 1) i99 = 1
      printf "%d %.2f %.2f %.2f %.2f %.2f %.2f %.0f\n", n, g[i50] / 1e6, g[i99] / 1e6, r[i50] / 1e6, r[i99] / 1e6, x[i50] / 1e6, x[i99] / 1e6, bk / n / 1000}' "$OUT/frames.log")
  secs=$(awk -v a="$t0" -v b="$t1" 'BEGIN {print (b - a) / 1e9}')
  cpu() { awk -v a="$1" -v b="$2" -v s="$secs" -v hz="$hz" 'BEGIN {printf "%.1f", (b - a) / hz / s * 100}'; }
  printf '%-36s %6d %7s %7s %7s %7s %7s %7s %7s %6s %6s\n' "$name" "$n" "$g50" "$g99" "$r50" "$r99" "$x50" "$x99" "$bk" "$(cpu "$c0" "$c1")" "$(cpu "$s0" "$s1")" | tee -a "$OUT/report.txt"
done < "$OUT/cells.txt"
# One board per wallpaper and surface: rows = glass settings.
for wall in $WALLS; do
  for surface in $SURFACES; do
    shots=()
    for config in $CONFIGS; do shots+=(-label "$config" "$OUT/$wall/$config-$surface.png"); done
    magick montage "${shots[@]}" -tile 1x -geometry 960x540+4+4 -pointsize 22 -background '#808080' "$OUT/board-$wall-$surface.png"
  done
done

# One page: a table per wallpaper, rows = glass settings, columns = surfaces.
{
  printf '<!doctype html><meta charset="utf-8"><title>Glass matrix %s</title>\n' "$SCHEME"
  printf '<style>body{font:14px sans-serif;background:#888;margin:16px}td,th{padding:4px;vertical-align:top}img{width:420px;display:block}</style>\n'
  printf '<h1>Glass matrix, %s scheme</h1>\n' "$SCHEME"
  for wall in $WALLS; do
    printf '<h2>%s wall</h2><table><tr><th></th>' "$wall"
    for surface in $SURFACES; do printf '<th>%s</th>' "$surface"; done
    printf '</tr>\n'
    for config in $CONFIGS; do
      printf '<tr><th>%s</th>' "$config"
      for surface in $SURFACES; do printf '<td><a href="%s/%s-%s.png"><img src="%s/%s-%s.png"></a></td>' "$wall" "$config" "$surface" "$wall" "$config" "$surface"; done
      printf '</tr>\n'
    done
    printf '</table>\n'
  done
} > "$OUT/index.html"
