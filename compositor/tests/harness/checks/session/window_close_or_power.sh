#!/usr/bin/env bash
# Alt+F4's action closes the focused window; with no window it asks the shell for the power menu.
set -euo pipefail

python3 - <<'PY'
import json
import os
import subprocess
import time

andromeda = os.environ["ANDROMEDA"]
client = os.environ["ANDROMEDA_UNMAP_CLIENT"]


def msg(value):
    return subprocess.run([andromeda, "msg", value], capture_output=True, text=True, timeout=5)


def windows():
    result = subprocess.run([andromeda, "windows", "--json"], capture_output=True, text=True, check=True, timeout=5)
    return json.loads(result.stdout)


def await_windows(predicate, description):
    for _ in range(100):
        state = windows()
        if predicate(state):
            return state
        time.sleep(0.025)
    raise SystemExit(f"timed out waiting for {description}: {state}")


log_path = os.path.join(os.environ["ANDROMEDA_RUNTIME_DIR"], "close-or-power.log")
log = open(log_path, "w")
process = subprocess.Popen([client, "close-or-power", "640", "480"], stdout=log, stderr=subprocess.STDOUT)
log.close()
await_windows(lambda s: any(w["title"] == "close-or-power" and w["active"] for w in s), "client window")

result = msg("window-close-or-power-overlay")
if result.returncode != 0:
    raise SystemExit(f"closing the focused window failed: {result.stdout} {result.stderr}")
for _ in range(160):
    if "unmapped" in open(log_path).read().split():
        break
    time.sleep(0.025)
else:
    raise SystemExit("the focused window did not close")
await_windows(lambda s: all(w["title"] != "close-or-power" for w in s), "closed window to leave the list")
process.kill()

# The harness binds no shell, so the power-menu branch reports that it had no shell to ask.
result = msg("window-close-or-power-overlay")
if result.returncode == 0 or "no desktop shell" not in result.stdout + result.stderr:
    raise SystemExit(f"empty desktop did not take the power-menu branch: {result.stdout} {result.stderr}")
PY
