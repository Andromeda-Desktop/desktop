#!/usr/bin/env bash
# Destroying an idle inhibitor on a visible surface lets the session go idle: the compositor recomputes inside the
# inhibitor's destroy signal, before wlroots unlinks it.
set -euo pipefail

readonly CLIENT="${ANDROMEDA_IDLE_INHIBIT_CLIENT:-./build-debug/tests/idle-inhibit-client}"
CLIENT_LOG="$ANDROMEDA_RUNTIME_DIR/idle-inhibit-release.log"

"$CLIENT" release > "$CLIENT_LOG" 2>&1 &

for _ in $(seq 40); do
  grep -q '^released$' "$CLIENT_LOG" && break
  sleep 0.05
done
if ! grep -q '^released$' "$CLIENT_LOG"; then
  echo "idle inhibitor client never released: $(cat "$CLIENT_LOG")"
  exit 1
fi
if sed -n '/^released$/q;p' "$CLIENT_LOG" | grep -q '^idled$'; then
  echo "visible surface failed to inhibit idle before the release: $(tr "\n" " " < "$CLIENT_LOG")"
  exit 1
fi

after_release() { sed -n '/^released$/,$p' "$CLIENT_LOG" | grep -q '^idled$'; }
for _ in $(seq 40); do
  after_release && break
  sleep 0.05
done
if ! after_release; then
  echo "a destroyed inhibitor still inhibits idle: $(cat "$CLIENT_LOG")"
  exit 1
fi

echo "idle goes on once the inhibitor is destroyed"
