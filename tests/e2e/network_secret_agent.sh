#!/usr/bin/env bash
# The shell's NetworkManager secret agent answers only NetworkManager: a direct bus caller gets AccessDenied from
# GetSecrets (it could otherwise raise the password prompt and read the typed secret), while the mock NM's own call
# reaches the agent (NoSecrets, as it asks without interaction). Writes calls.txt to $OUT
# (default ./artifacts/network-secret-agent).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/network-secret-agent}
source "$(dirname "$0")/lib.sh"
boot_headless 1
: > "$OUT/calls.txt"

MOCK="$(cd "$(dirname "$0")" && pwd)/mock_nm.py"
WAIT="$(cd "$(dirname "$0")/../.." && pwd)/tools/wait.sh"
with_noctalia '
  python3 '"$MOCK"' "$OUT/calls.txt" > "$OUT/mock.log" 2>&1 &
  '"$WAIT"' --log "$OUT/mock.log" --match ready --timeout 5 || true
  '"$WAIT"' --log "$OUT/calls.txt" --match "^Agent " --timeout 10 || true
  agent=$(sed -n "s/^Agent //p" "$OUT/calls.txt" | tail -1)
  [[ -n $agent ]] || { echo "FAIL: the agent never registered with NetworkManager"; exit 1; }
  sys() { gdbus call --address "$DBUS_SYSTEM_BUS_ADDRESS" "$@" 2>&1 || true; }
  denied=$(sys -d "$agent" -o /org/freedesktop/NetworkManager/SecretAgent \
    -m org.freedesktop.NetworkManager.SecretAgent.GetSecrets "{\"connection\": {\"id\": <\"wifi\">}}" \
    /org/freedesktop/NetworkManager/Settings/1 802-11-wireless-security "[]" 1)
  [[ $denied == *"Only the owner of org.freedesktop.NetworkManager"* ]] \
    || { echo "FAIL: a direct caller reached GetSecrets: $denied"; exit 1; }
  asked=$(sys -d org.freedesktop.NetworkManager -o /org/freedesktop/NetworkManager -m org.test.MockNM.AskSecrets)
  [[ $asked == *"NoSecrets"* ]] || { echo "FAIL: NetworkManager could not reach the agent: $asked"; exit 1; }
  kill %2
'
echo "PASS; artifacts: $OUT"
