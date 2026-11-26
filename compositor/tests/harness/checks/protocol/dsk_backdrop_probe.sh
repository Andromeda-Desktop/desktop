#!/usr/bin/env bash
# dsk_surface_v1.set_backdrop_probe: a clear layer surface with no blur reports the luminance of the backdrop under it
# (backdrop_luminance), and reports again when the backdrop changes from white to black. The probe draws nothing.
set -euo pipefail
source "$ANDROMEDA_HARNESS_LIB"

readonly LAYER_CLIENT="${ANDROMEDA_LAYER_CLIENT:-./build-debug/tests/layer-client}"
readonly LOG="$ANDROMEDA_RUNTIME_DIR/dsk-probe.log"
readonly FIFO="$ANDROMEDA_RUNTIME_DIR/dsk-probe.fifo"
readonly SHOT="$ANDROMEDA_RUNTIME_DIR/dsk-probe.png"
readonly REGION="400x20+100+10"

if [[ ! -x $LAYER_CLIENT ]]; then
  echo "required harness clients are not built"
  exit 1
fi

printf '\n[animation]\nenabled = false\n\n[colors]\nbackdrop = "#FFFFFFFF"\n' >> "$ANDROMEDA_CONFIG"
"$ANDROMEDA" msg config-reload > /dev/null

pids=()
trap 'kill "${pids[@]}" 2> /dev/null || true' EXIT
mkfifo "$FIFO"
exec {control_fd}<> "$FIFO"
LAYER_DSK_VERSION=21 LAYER_CONTROL=1 "$LAYER_CLIENT" HEADLESS-1 40 <&"$control_fd" > "$LOG" 2>&1 &
pids+=($!)
await_lines "$LOG" ready 1

layer() {
  local label
  read -r _ label _ <<< "$1"
  printf '%s\n' "$1" >&"$control_fd"
  await_lines "$LOG" "done $label " 1
}

layer "color plain 00000000"
layer "dsk-probe on 1"
await_lines "$LOG" "dsk-luminance " 1
bright=$(grep -m1 '^dsk-luminance ' "$LOG" | cut -d' ' -f2)
if ! awk -v l="$bright" 'BEGIN { exit !(l > 0.8) }'; then
  echo "a white backdrop reads as luminance $bright, want > 0.8"
  exit 1
fi

"$ANDROMEDA" settle
grim -o HEADLESS-1 "$SHOT"
read -r white < <("$ANDROMEDA_PIXEL_PROBE" "$SHOT" count 'r > 0.95 && g > 0.95 && b > 0.95' "$REGION")
if ((white < 7900)); then
  echo "the probe drew over the backdrop: $white of 8000 pixels white"
  exit 1
fi

sed -i 's/^backdrop = "#FFFFFFFF"$/backdrop = "#000000FF"/' "$ANDROMEDA_CONFIG"
"$ANDROMEDA" msg config-reload > /dev/null
await_lines "$LOG" "dsk-luminance " 2
dark=$(grep '^dsk-luminance ' "$LOG" | sed -n 2p | cut -d' ' -f2)
if ! awk -v l="$dark" 'BEGIN { exit !(l < 0.2) }'; then
  echo "a black backdrop reads as luminance $dark, want < 0.2"
  exit 1
fi
echo "ok: white $bright, black $dark"
