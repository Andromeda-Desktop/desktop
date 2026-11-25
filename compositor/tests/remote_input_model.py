#!/usr/bin/env python3
"""Model test for andromeda_remote_input_v1: random sequences of start, consent answer, connect_eis, motion,
session destroy, revoke and session lock against an isolated compositor. Invariants after every step:
- the consent panel shows exactly while one session asks;
- a session starts only after Enter or R on its own panel, and gets an EIS fd only while granted and unlocked;
- emulated motion moves the pointer only from a granted, connected session while unlocked and no panel shows;
- revoke closes remembered sessions and disconnects their EIS clients; once-sessions keep working.
Usage: remote_input_model.py ANDROMEDA_BUILD [SEED] [STEPS]"""
import os
import queue
import random
import subprocess
import sys
import tempfile
import threading
import time

BUILD = os.path.realpath(sys.argv[1])
SEED = int(sys.argv[2]) if len(sys.argv) > 2 else int(time.time())
STEPS = int(sys.argv[3]) if len(sys.argv) > 3 else 60
RUNTIME = tempfile.mkdtemp(prefix="andromeda-remote-model.")
os.chmod(RUNTIME, 0o700)
ENV = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_SOCKET", "NOTIFY_SOCKET")}
ENV.update(XDG_RUNTIME_DIR=RUNTIME, XDG_CONFIG_HOME=f"{RUNTIME}/config", XDG_STATE_HOME=f"{RUNTIME}/state",
           XDG_DATA_HOME=f"{RUNTIME}/data-home", XDG_CACHE_HOME=f"{RUNTIME}/cache", XDG_DATA_DIRS=f"{RUNTIME}/data",
           DBUS_SESSION_BUS_ADDRESS=f"unix:path={RUNTIME}/no-bus", GSETTINGS_BACKEND="memory", NO_AT_BRIDGE="1",
           WAYLAND_DISPLAY="remote-model-isolated-parent", WLR_BACKENDS="headless", WLR_HEADLESS_OUTPUTS="1")
TIMEOUT = 5.0
trace = []


class Proc:
    def __init__(self, name, argv, env):
        self.name = name
        self.lines = queue.Queue()
        self.log = []
        self.proc = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                     env=env, text=True, bufsize=1)
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        for line in self.proc.stdout:
            self.log.append(line.rstrip())
            self.lines.put(line.rstrip())

    def send(self, line):
        self.proc.stdin.write(line + "\n")
        self.proc.stdin.flush()

    def expect(self, prefix, what):
        deadline = time.monotonic() + TIMEOUT
        while time.monotonic() < deadline:
            try:
                line = self.lines.get(timeout=deadline - time.monotonic())
            except queue.Empty:
                break
            if line.startswith(prefix):
                return line
        fail(f"{self.name}: no '{prefix}' ({what})")

    def drain(self, prefix):
        """Lines starting with prefix that already arrived, without waiting."""
        found = []
        while True:
            try:
                line = self.lines.get_nowait()
            except queue.Empty:
                return found
            if line.startswith(prefix):
                found.append(line)

    def stop(self):
        if self.proc.poll() is None:
            self.proc.kill()
        self.proc.wait()


def fail(message):
    print(f"FAIL seed={SEED}: {message}")
    print("trace: " + " | ".join(trace[-25:]))
    print(f"logs kept in {RUNTIME}")
    for proc in [compositor, *clients.values()]:
        proc.stop()
    sys.exit(1)


config = f"{RUNTIME}/config.toml"
with open(config, "w") as f:
    f.write('[general]\nxwayland = false\nautostart = []\n[animation]\nenabled = false\n'
            '[input.keyboard]\nlayout = "us"\nnumlock_toggle = false\n')
clients = {}
compositor = Proc("compositor", [f"{BUILD}/tests/input-capture-compositor", config, "--trust-binary", f"{BUILD}/"], ENV)
ready = compositor.expect("ready display=", "compositor start")
CLIENT_ENV = dict(ENV, WAYLAND_DISPLAY=ready.split("display=")[1].split()[0],
                  ANDROMEDA_SOCKET=ready.split("socket=")[1].split()[0])


def status():
    compositor.drain("status")
    compositor.send("status")
    line = compositor.expect("status ", "status")
    fields = dict(part.split("=", 1) for part in line.split()[1:] if "=" in part)
    return int(fields["confirmation"]), float(fields["x"]), float(fields["y"])


class Model:
    """Phase per session: idle, asking, once, remembered, closed. connected: has a resumed EIS pointer."""

    def __init__(self):
        self.phase = {}
        self.connected = set()
        # Clients that hold libei devices; a revoke disconnects them but they keep sending.
        self.devices = set()
        self.locked = None
        self.next = 0

    def asking(self):
        return next((name for name, phase in self.phase.items() if phase == "asking"), None)


model = Model()
rng = random.Random(SEED)


def check_panel():
    shown = status()[0]
    if shown != (1 if model.asking() else 0):
        fail(f"panel shown={shown} while asking={model.asking()}")


def action_new():
    name = f"c{model.next}"
    model.next += 1
    clients[name] = Proc(name, [f"{BUILD}/tests/remote-input-client"], CLIENT_ENV)
    clients[name].expect("ready", "client start")
    model.phase[name] = "idle"


def action_start(name):
    clients[name].send("start 2 -")
    if model.asking() is None and model.locked is None:
        model.phase[name] = "asking"
        for _ in range(100):
            if status()[0] == 1:
                return
        fail(f"{name}: no consent panel")
    line = clients[name].expect("started", "start refused while busy or locked")
    if "status=0" in line:
        fail(f"{name}: started while another panel showed or the session was locked: {line}")
    model.phase[name] = "closed"


def action_answer(key):
    name = model.asking()
    compositor.send(f"tap {key}")
    line = clients[name].expect("started", "consent answer")
    if key == 1:
        if "status=0" in line:
            fail(f"{name}: Escape started the session")
        model.phase[name] = "closed"
        return
    if "status=0 capabilities=3" not in line:
        fail(f"{name}: allowed but got {line}")
    model.phase[name] = "remembered" if key == 19 else "once"


def action_connect(name):
    client = clients[name]
    granted = model.phase[name] in ("once", "remembered") and model.locked is None
    if name in model.connected:
        return
    client.drain("pointer-ready")
    client.send("connect")
    if granted:
        client.expect("eis-fd", "granted connect")
        client.expect("pointer-ready", "granted pointer")
        model.connected.add(name)
        model.devices.add(name)
    else:
        line = client.expect("result", "refused connect")
        if "status=2" not in line:
            fail(f"{name}: connect while {model.phase[name]} locked={model.locked is not None} gave {line}")


def action_move(name):
    compositor.send("at 100 100")
    for _ in range(50):
        _, x0, y0 = status()
        if (x0, y0) == (100.0, 100.0):
            break
    clients[name].send("move 40 30")
    clients[name].expect("moved", "motion sent")
    # Any consent panel pauses emulated input, so a granted session cannot click its own approval.
    expected = name in model.connected and model.locked is None and model.asking() is None
    for _ in range(60 if expected else 15):
        _, x1, y1 = status()
        if (x1, y1) != (x0, y0):
            break
    moved = (x1 - x0, y1 - y0)
    if expected and moved != (40.0, 30.0):
        fail(f"{name}: a granted motion moved the pointer by {moved}")
    if not expected and moved != (0.0, 0.0):
        fail(f"{name}: motion moved the pointer by {moved} while {model.phase[name]}, locked={model.locked is not None}")


def action_destroy(name):
    clients.pop(name).stop()
    del model.phase[name]
    model.connected.discard(name)
    model.devices.discard(name)


def action_revoke():
    result = subprocess.run([f"{BUILD}/andromeda", "input-capture-revoke", "all"], env=CLIENT_ENV,
                            capture_output=True, text=True)
    if result.returncode != 0:
        fail(f"revoke failed: {result.stdout}{result.stderr}")
    for name, phase in list(model.phase.items()):
        if phase == "remembered":
            clients[name].expect("closed", "revoke closes a remembered session")
            if name in model.connected:
                clients[name].expect("ei-disconnected", "revoke disconnects EIS")
            model.phase[name] = "closed"
            model.connected.discard(name)


def action_lock():
    model.locked = Proc("lock", [f"{BUILD}/tests/lock-client"], CLIENT_ENV)
    model.locked.expect("locked", "session lock")


def action_unlock():
    model.locked.send("unlock")
    model.locked.expect("unlocked", "session unlock")
    model.locked.stop()
    model.locked = None
    for name in model.connected:
        clients[name].drain("pointer-ready")


try:
    for _ in range(2):
        action_new()
    for step in range(STEPS):
        names = list(model.phase)
        choices = [("new", None)] if len(names) < 4 else []
        choices += [("start", n) for n in names if model.phase[n] == "idle"]
        if model.asking() and model.locked is None:
            choices += [("answer", k) for k in (1, 28, 19)]
        choices += [("connect", n) for n in names if n not in model.connected]
        choices += [("move", n) for n in names if n in model.devices]
        choices += [("destroy", n) for n in names]
        choices += [("revoke", None)]
        if model.asking() is None:
            choices += [("unlock", None)] if model.locked else [("lock", None)]
        weights = {"new": 1, "start": 3, "answer": 3, "connect": 3, "move": 5, "destroy": 1, "revoke": 1, "lock": 1,
                   "unlock": 3}
        kind, arg = rng.choices(choices, [weights[kind] for kind, _ in choices])[0]
        expected = kind == "move" and arg in model.connected and model.locked is None and model.asking() is None
        trace.append(f"{step}:{kind}{'' if arg is None else ' ' + str(arg)}{'+' if expected else ''}")
        {"new": lambda _: action_new(), "start": action_start, "answer": action_answer, "connect": action_connect,
         "move": action_move, "destroy": action_destroy, "revoke": lambda _: action_revoke(),
         "lock": lambda _: action_lock(), "unlock": lambda _: action_unlock()}[kind](arg)
        check_panel()
finally:
    for proc in [*clients.values(), *([model.locked] if model.locked else []), compositor]:
        proc.stop()
counts = {}
for entry in trace:
    kind = entry.split(":", 1)[1].split()[0] + ("+" if entry.endswith("+") else "")
    counts[kind] = counts.get(kind, 0) + 1
print(f"remote input model: {STEPS} steps hold (seed={SEED}) {counts}")
