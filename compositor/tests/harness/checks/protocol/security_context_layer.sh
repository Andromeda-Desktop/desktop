#!/usr/bin/env bash
# A sandboxed app's layer surface maps only after the user allows layer-shell for the app in the confirm panel. A deny
# closes the surface and holds for the session, an allow covers the app's later surfaces, a client that leaves while
# asking takes the panel away, and a rule that grants the global skips the panel.
set -euo pipefail
source "$ANDROMEDA_HARNESS_LIB"

readonly APP_LOG="$ANDROMEDA_RUNTIME_DIR/security-context-layer.log"

mapped_layers() { "$ANDROMEDA" layers --json | jq '[.[] | select(.mapped)] | length'; }
log_count() { grep -c "$1" "$ANDROMEDA_LOG" || true; }

await_log() {
  local pattern=$1 expected=$2
  for _ in $(seq 60); do
    (($(log_count "$pattern") >= expected)) && return 0
    sleep 0.1
  done
  echo "timed out waiting for $expected '$pattern' in the compositor log: $(tail -n 5 "$ANDROMEDA_LOG")"
  return 1
}

expect_mapped() {
  local expected=$1 reason=$2
  "$ANDROMEDA" settle > /dev/null
  if [[ $(mapped_layers) != "$expected" ]]; then
    echo "$reason: expected $expected mapped layers, got $("$ANDROMEDA" layers --json)"
    exit 1
  fi
}

# The sandbox wrapper forks the layer client; stopping the wrapper alone leaves the client connected.
stop_app() {
  pkill -P "$1" || true
  wait "$1" || true
}

sandboxed_layer() {
  SECURITY_CONTEXT_APP_ID=$1 exec "$ANDROMEDA_SECURITY_CONTEXT_CLIENT" "$ANDROMEDA_LAYER_CLIENT" HEADLESS-1 0 overlay-layer
}

# Deny: the surface never maps, the client gets closed and exits, and the app is not asked again this session.
sandboxed_layer org.andromeda.Denied > "$APP_LOG" 2>&1 &
pid=$!
await_log "layer-shell access requested app='org.andromeda.harness/org.andromeda.Denied'" 1
expect_mapped 0 "a surface waiting for access"
"$ANDROMEDA_POINTER_CLIENT" 2560 720 tap 1
if ! wait "$pid"; then
  echo "denied client failed: $(< "$APP_LOG")"
  exit 1
fi
(sandboxed_layer org.andromeda.Denied) > "$APP_LOG" 2>&1 || {
  echo "second denied client failed: $(< "$APP_LOG")"
  exit 1
}
if (($(log_count "layer-shell access requested app='org.andromeda.harness/org.andromeda.Denied'") != 1)); then
  echo "a denied app was asked again"
  exit 1
fi

# A client that leaves while asking takes the panel away; its app is asked again next time.
sandboxed_layer org.andromeda.Gone > "$APP_LOG" 2>&1 &
pid=$!
await_log "layer-shell access requested app='org.andromeda.harness/org.andromeda.Gone'" 1
stop_app "$pid"
sandboxed_layer org.andromeda.Gone > "$APP_LOG" 2>&1 &
pid=$!
await_log "layer-shell access requested app='org.andromeda.harness/org.andromeda.Gone'" 2
stop_app "$pid"

# Allow: the surface maps; the app's next surface maps without a panel.
sandboxed_layer org.andromeda.Allowed > "$APP_LOG" 2>&1 &
pid=$!
await_log "layer-shell access requested app='org.andromeda.harness/org.andromeda.Allowed'" 1
"$ANDROMEDA_POINTER_CLIENT" 2560 720 tap 28
expect_mapped 1 "an allowed surface"
stop_app "$pid"
sandboxed_layer org.andromeda.Allowed > "$APP_LOG" 2>&1 &
pid=$!
for _ in $(seq 60); do
  [[ $(mapped_layers) == 1 ]] && break
  sleep 0.1
done
expect_mapped 1 "a second surface of an allowed app"

# A revoke closes the app's surface, and its next surface asks again.
"$ANDROMEDA" layer-shell-revoke org.andromeda.harness/org.andromeda.Allowed > /dev/null
if ! wait "$pid"; then
  echo "revoked client failed: $(< "$APP_LOG")"
  exit 1
fi
expect_mapped 0 "a revoked app"
sandboxed_layer org.andromeda.Allowed > "$APP_LOG" 2>&1 &
pid=$!
await_log "layer-shell access requested app='org.andromeda.harness/org.andromeda.Allowed'" 2
expect_mapped 0 "a revoked app asking again"
stop_app "$pid"

# A rule that grants the global skips the panel.
cat >> "$ANDROMEDA_CONFIG" << 'EOF'

[[security_context_rule]]
match.app_id = '^org\.andromeda\.Ruled$'
allow_globals = ["zwlr_layer_shell_v1"]
EOF
"$ANDROMEDA" msg config-reload > /dev/null
sandboxed_layer org.andromeda.Ruled > "$APP_LOG" 2>&1 &
pid=$!
for _ in $(seq 60); do
  [[ $(mapped_layers) == 1 ]] && break
  sleep 0.1
done
expect_mapped 1 "a rule-granted surface"
if (($(log_count "org.andromeda.Ruled") != 0)); then
  echo "a rule-granted app was asked"
  exit 1
fi
stop_app "$pid"
echo "sandboxed layer surfaces map only after the user allows the app"
