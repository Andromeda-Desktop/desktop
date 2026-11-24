#!/usr/bin/env bash
# The system monitor samples only while something shows its stats: a desktop with only a clock widget never starts
# the sampler, one with a sysmon desktop widget does. The sampler logs "detected stats sources" when it starts. Writes result.txt to $OUT
# (default ./artifacts/sysmon-demand).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/sysmon-demand}
source "$(dirname "$0")/lib.sh"
mkdir -p "$OUT"; : > "$OUT/result.txt"

run_bar() {
  local name=$1 type=$2 extra=$3
  (
    boot_headless 1
    printf '\n[desktop_widgets]\nenabled = true\nschema_version = 2\nwidget_order = ["w"]\n\n[desktop_widgets.widget.w]\ntype = "%s"\noutput = "HEADLESS-1"\ncx = 400.0\ncy = 300.0\n\n[desktop_widgets.widget.w.settings]\n%s\n' \
      "$type" "$extra" >> "$RUNTIME/home/.config/andromeda/shell/config.toml"
    with_noctalia 'sleep 3' # real time: the widget surface maps and is built
    cp "$OUT/noctalia.log" "$OUT/$name.log"
    echo "$name sampler_started=$(grep -c "detected stats sources" "$OUT/$name.log" || true)" >> "$OUT/result.txt"
  )
}
run_bar clock clock ''
run_bar sysmon sysmon 'stat = "cpu_usage"'
cat "$OUT/result.txt"
grep -qx "clock sampler_started=0" "$OUT/result.txt" || { echo "FAIL: sampler ran with nothing showing stats"; exit 1; }
grep -qx "sysmon sampler_started=1" "$OUT/result.txt" || { echo "FAIL: sampler did not start for the sysmon widget"; exit 1; }
echo "PASS; artifacts: $OUT"
