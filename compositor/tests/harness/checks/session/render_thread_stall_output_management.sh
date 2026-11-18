#!/usr/bin/env bash
# harness: outputs=2
# An output-management apply (output-disable) during a 3 s stalled GPU submit draws the remaining output on the render
# thread and commits after the stall: other IPC answers within 250 ms meanwhile, and the action replies only after
# HEADLESS-2 is disabled.
set -euo pipefail

"$ANDROMEDA" stall-submit 3000 > /dev/null
"$ANDROMEDA" windows --json > /dev/null
done_file="$ANDROMEDA_RUNTIME_DIR/disable.done"
( "$ANDROMEDA" msg output-disable:HEADLESS-2 > /dev/null || { echo "output-disable failed" >&2; exit 1; }
  "$ANDROMEDA" outputs --json | jq -r '.[] | select(.name == "HEADLESS-2") | .enabled' > "$done_file" ) &
# A raw socket request times the compositor's reply without the CLI's process start; IPC answers one request per connection.
slowest=$(python3 - "$ANDROMEDA_SOCKET" <<'PY'
import json
import socket
import sys
import time

slowest = 0.0
end = time.monotonic() + 2
while time.monotonic() < end:
    start = time.monotonic()
    client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    client.connect(sys.argv[1])
    client.sendall(b'{"cmd":"windows"}\n')
    reply = b""
    while not reply.endswith(b"\n"):
        chunk = client.recv(65536)
        if not chunk:
            sys.exit("the compositor closed the IPC connection during the stall")
        reply += chunk
    client.close()
    if "ok" not in json.loads(reply):
        sys.exit(f"windows failed during the stall: {reply[:200]!r}")
    slowest = max(slowest, time.monotonic() - start)
    time.sleep(0.1)
print(int(slowest * 1000))
PY
)
(( slowest < 250 )) || { echo "IPC took $slowest ms while output-disable ran during the stalled submit, want < 250"; exit 1; }
for _ in $(seq 100); do
  [[ -s "$done_file" ]] && break
  sleep 0.1
done
[[ -s "$done_file" ]] || { echo "output-disable did not reply after the stalled submit returned"; exit 1; }
[[ $(< "$done_file") == false ]] || { echo "output-disable replied before HEADLESS-2 was disabled"; exit 1; }
timeout 10 "$ANDROMEDA" settle > /dev/null || { echo "the outputs did not settle after the stalled submit returned"; exit 1; }
echo "output-disable during a 3 s stalled submit leaves IPC under 250 ms (slowest $slowest ms) and replies once applied"
