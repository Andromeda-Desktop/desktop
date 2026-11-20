#!/usr/bin/env python3
"""App launch -> first window time, warm and cold, idle or under CPU load (.agent/TASK-launch-fast.md).

Launches go to the shell as the launcher sends them (`app-launch <desktop id>` on its IPC socket); the time is the
compositor's own log line "launch <app> mapped|raised in N ms (token|unmatched)", from token to first map.
A run is warm when the app had a window before it. The bench closes only the windows it opened, and never quits an
app it did not start.

Env: APPS (desktop ids), RUNS (5), LOAD (busy threads, 0), LOAD_WEIGHT (CPUWeight of the load scope, 100),
     LOG (compositor log file; default: follow the andromeda.service journal), ANDROMEDA (compositor CLI),
     ANDROMEDA_SOCKET, XDG_RUNTIME_DIR, WAYLAND_DISPLAY (as for the session under test).
Usage: tools/launch-bench.py, or in `just dev-nested`: LOG=$RUNTIME/andromeda.log with that session's env.
"""
import json
import os
import re
import socket
import statistics
import subprocess
import sys
import time

APPS = os.environ.get("APPS", "dev.andromeda.Files dev.andromeda.Viewer dev.andromeda.Player foot").split()
RUNS = int(os.environ.get("RUNS", "5"))
LOAD = int(os.environ.get("LOAD", "0"))
LOAD_WEIGHT = os.environ.get("LOAD_WEIGHT", "100")
ANDROMEDA = os.environ.get("ANDROMEDA", "andromeda")
SHELL_SOCK = os.path.join(os.environ["XDG_RUNTIME_DIR"], "noctalia-" + os.environ["WAYLAND_DISPLAY"] + ".sock")
LINE = re.compile(r"launch (\S+) (mapped|raised) in (\d+) ms \((token|unmatched)\)")
TIMEOUT_S = 10.0


def windows():
    out = subprocess.run([ANDROMEDA, "windows", "--json"], capture_output=True, text=True, check=True).stdout
    data = json.loads(out)
    return data.get("ok", data) if isinstance(data, dict) else data


def window_ids(app_id):
    return {w["id"] for w in windows() if w.get("app_id") == app_id}


def shell(command):
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
        s.connect(SHELL_SOCK)
        s.sendall(("/\x1e" + command).encode())
        s.shutdown(socket.SHUT_WR)
        reply = s.recv(4096).decode().strip()
    if reply != "ok":
        raise SystemExit(f"shell: {command}: {reply}")


class Log:
    def __init__(self):
        path = os.environ.get("LOG")
        cmd = ["tail", "-n0", "-F", path] if path else ["journalctl", "--user", "-f", "-n0", "-o", "cat", "-u",
                                                         "andromeda.service"]
        self.proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, text=True)
        os.set_blocking(self.proc.stdout.fileno(), False)
        self.buffer = ""

    def wait(self, app_id):
        deadline = time.monotonic() + TIMEOUT_S
        while time.monotonic() < deadline:
            chunk = self.proc.stdout.read()
            if chunk:
                self.buffer += chunk
            for line in self.buffer.splitlines():
                m = LINE.search(line)
                if m and m.group(1) == app_id:
                    self.buffer = self.buffer[self.buffer.index(line) + len(line):]
                    return int(m.group(3)), m.group(2), m.group(4)
            time.sleep(0.005)
        return None


def start_load():
    if LOAD == 0:
        return None
    unit = f"launch-bench-load-{os.getpid()}.scope"
    loops = " & ".join(["while :; do :; done"] * LOAD) + " & wait"
    proc = subprocess.Popen(["systemd-run", "--user", "--scope", "--quiet", "--slice=app.slice", f"--unit={unit}",
                             f"--property=CPUWeight={LOAD_WEIGHT}", "sh", "-c", loops])
    time.sleep(1)
    return unit, proc


def stop_load(load):
    if load is not None:
        subprocess.run(["systemctl", "--user", "stop", load[0]], check=False)
        load[1].wait()


def p95(values):
    return sorted(values)[max(0, round(0.95 * len(values)) - 1)]


def main():
    log = Log()
    load = start_load()
    results = {}
    try:
        for app in APPS:
            for _ in range(RUNS):
                before = window_ids(app)
                kind = "warm" if before else "cold"
                shell("app-launch " + app)
                got = log.wait(app)
                time.sleep(0.3)
                opened = window_ids(app) - before
                for wid in opened:
                    subprocess.run([ANDROMEDA, "msg", f"window-close:{wid}"], capture_output=True, check=False)
                results.setdefault((app, kind), []).append(got)
                time.sleep(0.5)
    finally:
        stop_load(load)
        log.proc.terminate()
    print(f"load={LOAD} threads at CPUWeight {LOAD_WEIGHT}, runs={RUNS}")
    for (app, kind), runs in results.items():
        times = [r[0] for r in runs if r is not None]
        misses = sum(1 for r in runs if r is None)
        unmatched = sum(1 for r in runs if r is not None and r[2] == "unmatched")
        summary = f"p50 {statistics.median(times):.0f} ms  p95 {p95(times)} ms" if times else "no window"
        print(f"{app:28} {kind:4}  {summary}  timeouts {misses}  unmatched {unmatched}")


if __name__ == "__main__":
    sys.exit(main())
