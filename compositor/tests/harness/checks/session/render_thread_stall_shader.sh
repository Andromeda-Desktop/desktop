#!/usr/bin/env bash
# A config reload that adds an effect shader while a GPU submit is stuck for 3 s compiles once the render thread
# returns the GL context: the main loop never waits for it, so IPC answers within 250 ms, and the program is ready
# after the stall.
set -euo pipefail
readonly CLIENT="${ANDROMEDA_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
cat > "${ANDROMEDA_CONFIG%/*}/stall.glsl" <<'GLSL'
vec4 animation(vec2 uv) { return andromeda_sample(uv); }
GLSL

"$ANDROMEDA" stall-submit 3000 > /dev/null
FILL_COLOR=0xFFFF0000 "$CLIENT" shader-stall 400 240 > "$ANDROMEDA_RUNTIME_DIR/client.log" 2>&1 &
for _ in $(seq 40); do
  "$ANDROMEDA" windows --json | jq -e 'any(.[]; .title == "shader-stall")' > /dev/null && break
  sleep 0.05
done
cat >> "$ANDROMEDA_CONFIG" <<'CONF'

[effects.preset.windows_in_stall]
kind = "animation"
shader = "stall.glsl"
[animation.windows_in]
effect = "windows_in_stall"
CONF
t0=$(date +%s%N)
"$ANDROMEDA" msg config-reload > /dev/null
slowest=$(( ($(date +%s%N) - t0) / 1000000 ))
start=$(date +%s%N)
while (( $(date +%s%N) - start < 2000000000 )); do
  t0=$(date +%s%N)
  "$ANDROMEDA" windows --json > /dev/null
  ms=$(( ($(date +%s%N) - t0) / 1000000 ))
  (( ms > slowest )) && slowest=$ms
  sleep 0.1
done
(( slowest < 250 )) || { echo "IPC took $slowest ms while a reload compiled a shader during the stalled submit, want < 250"; exit 1; }
timeout 10 "$ANDROMEDA" settle > /dev/null || { echo "the output did not draw after the stalled submit returned"; exit 1; }
grep -q "failed to compile" "$ANDROMEDA_LOG" && { echo "$(grep -m1 'failed to compile' "$ANDROMEDA_LOG")"; exit 1; }
echo "a reload that adds a shader during a 3 s stalled submit leaves IPC under 250 ms (slowest $slowest ms) and compiles it"
