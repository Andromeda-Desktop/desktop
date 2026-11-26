#!/usr/bin/env bash
# Session power actions reach logind through andromeda-system: a mock org.freedesktop.login1 on the test's private
# system bus records each *WithFlags call while `noctalia msg session suspend|reboot|shutdown|hibernate|suspend-then-hibernate` runs. Hibernate and
# suspend-then-hibernate are also gated on CanHibernate/CanSuspendThenHibernate: with the mock reporting "no", the
# call to logind must never happen. Writes calls.txt to $OUT (default ./artifacts/power-actions).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/power-actions}
source "$(dirname "$0")/lib.sh"
boot_headless 1
: > "$OUT/calls.txt"

MOCK="$(cd "$(dirname "$0")" && pwd)/mock_logind.py"
with_noctalia '
  start_mock() {
    : > "$OUT/mock.log"
    python3 '"$MOCK"' "$OUT/calls.txt" > "$OUT/mock.log" 2>&1 &
    mock=$!
    poll_for 5 grep -q ready "$OUT/mock.log" || { echo "FAIL: mock logind did not start: $(cat "$OUT/mock.log")"; exit 1; }
    start_andromeda_system
  }
  expect() {
    local action=$1 want=$2
    "$NOCTALIA" msg session "$action" > /dev/null
    poll_for 5 grep -qx "$want" "$OUT/calls.txt" && return 0
    echo "FAIL $action: logind never received \"$want\"; calls: $(tr "\n" "," < "$OUT/calls.txt")"
    exit 1
  }
  expect_no_call() {
    local action=$1 forbidden=$2
    local log_from calls_from
    log_from=$(wc -l < "$OUT/noctalia.log")
    calls_from=$(wc -l < "$OUT/calls.txt")
    "$NOCTALIA" msg session "$action" > /dev/null
    sleep 0.3
    tail -n +"$((calls_from + 1))" "$OUT/calls.txt" | grep -qx "$forbidden" \
      && { echo "FAIL $action: logind received \"$forbidden\" despite CanX=no"; exit 1; }
    tail -n +"$((log_from + 1))" "$OUT/noctalia.log" | grep -q "unavailable" \
      || { echo "FAIL $action: no gating warning logged"; exit 1; }
  }

  start_mock
  expect suspend "SuspendWithFlags 1"
  expect reboot "RebootWithFlags 1"
  expect shutdown "PowerOffWithFlags 1"
  expect hibernate "HibernateWithFlags 1"
  expect suspend-then-hibernate "SuspendThenHibernateWithFlags 1"
  kill "$mock" "$system"; wait "$mock" "$system" 2>/dev/null || true

  MOCK_CAN_HIBERNATE=no MOCK_CAN_SUSPEND_THEN_HIBERNATE=no start_mock
  expect_no_call hibernate "HibernateWithFlags 1"
  expect_no_call suspend-then-hibernate "SuspendThenHibernateWithFlags 1"
  kill "$mock" "$system"
'
echo "PASS: $(tr '\n' ',' < "$OUT/calls.txt"); artifacts: $OUT"
