#!/usr/bin/env python3
"""Random clipboard event sequences over two RemoteDesktop sessions with clipboard access, a local app (wl-copy,
wl-paste) and the session lock. After each step the real stack must match a model of the one selection:
a local paste returns the last writer's text, SelectionRead returns it, every unpaused session hears each change
once with session_is_owner true only for the session whose source holds the selection, and a paused session moves
no data and hears nothing until the unlock, which announces the current selection once.
Usage: clipboard_model.py SEED STEPS LOCK_CLIENT RUNTIME A B (A and B: client names with <name>.fifo/<name>.log)."""
import os
import random
import subprocess
import sys
import time

SEED, STEPS, LOCK_CLIENT, RUNTIME = int(sys.argv[1]), int(sys.argv[2]), sys.argv[3], sys.argv[4]
SESSIONS = sys.argv[5:7]
TIMEOUT = 6.0
rng = random.Random(SEED)
fifos = {name: open(f"{RUNTIME}/{name}.fifo", "w", buffering=1) for name in SESSIONS}
copies = []
lock = None
model = {"owner": None, "text": None}
heard = {name: 0 for name in SESSIONS}
LOCK_LOG = f"{RUNTIME}/model-lock.log"


def fail(message):
    print(f"seed {SEED}: {message}", flush=True)
    sys.exit(1)


def lines(name):
    with open(f"{RUNTIME}/{name}.log", encoding="utf-8", errors="replace") as log:
        return log.read().splitlines()


def count(name, prefix):
    return sum(1 for line in lines(name) if line.startswith(prefix))


def wait(predicate, what):
    deadline = time.monotonic() + TIMEOUT
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(0.02)
    fail(f"timed out: {what}")


def send(name, command):
    fifos[name].write(command + "\n")


def expected_flag(name):
    """The start of the line the session must have heard last; wl-copy adds text aliases after text/plain."""
    return f"owner-changed owner={int(model['owner'] == name)} types={'text/plain' if model['text'] else '-'}"


def last_flag(name):
    return ([line for line in lines(name) if line.startswith("owner-changed")] or [""])[-1]


def check_announced(changed):
    """Each unpaused session heard exactly one more change when `changed`, with the model's owner and types."""
    for name in SESSIONS:
        if lock is not None:
            if count(name, "owner-changed") != heard[name]:
                fail(f"{name} heard a change while locked")
            continue
        if changed:
            heard[name] += 1
        wait(lambda: count(name, "owner-changed") >= heard[name], f"{name} hears change {heard[name]}")
        last = last_flag(name)
        if count(name, "owner-changed") != heard[name]:
            fail(f"{name} heard {count(name, 'owner-changed')} changes, expected {heard[name]}")
        if not last.startswith(expected_flag(name)):
            fail(f"{name}: '{last}', expected '{expected_flag(name)}' (model {model})")


def paste():
    result = subprocess.run(["wl-paste", "--no-newline", "--type", "text/plain"], capture_output=True, text=True,
                            timeout=10)
    return result.stdout if result.returncode == 0 else None


def step(index):
    global lock
    action = rng.choice(["copy", "set", "set", "race", "read", "paste", "lock"])
    text = f"t{index}"
    print(f"seed {SEED} step {index}: {action} (model {model}, locked {lock is not None})", file=sys.stderr, flush=True)
    if action == "copy":
        copies.append(subprocess.Popen(["wl-copy", "--foreground", "--type", "text/plain", text],
                                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
        wait(lambda: paste() == text, f"the local copy {text} owns the selection")
        model.update(owner="local", text=text)
        check_announced(True)
    elif action == "set":
        name = rng.choice(SESSIONS)
        errors = count(name, "error")
        sets = count(name, "set")
        send(name, f"set text/plain {text}")
        wait(lambda: count(name, "set") > sets or count(name, "error") > errors, f"{name} set answered")
        if lock is not None:
            # The frontend forwards SetSelection without waiting and the portal refuses it, but an owning app now
            # serves its new text to later transfers: only the bytes change, not the owner.
            if model["owner"] == name:
                model["text"] = text
            check_announced(False)
            return
        model.update(owner=name, text=text)
        check_announced(True)
    elif action == "race" and lock is None:
        # A session's set and a local copy in flight together: the compositor orders them; the stack must agree.
        name = rng.choice(SESSIONS)
        before = {other: count(other, "owner-changed") for other in SESSIONS}
        sets = count(name, "set")
        send(name, f"set text/plain {text}s")
        copy = subprocess.Popen(["wl-copy", "--foreground", "--type", "text/plain", f"{text}l"],
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        copies.append(copy)
        wait(lambda: count(name, "set") > sets, f"{name} race set answered")
        wait(lambda: copy.poll() is not None or paste() == f"{text}l", "the local copy settled")
        winner = "local" if paste() == f"{text}l" else name
        model.update(owner=winner, text=f"{text}l" if winner == "local" else f"{text}s")
        for other in SESSIONS:
            wait(lambda: count(other, "owner-changed") > before[other] and last_flag(other).startswith(expected_flag(other)),
                 f"{other} settles on '{expected_flag(other)}' after the race (model {model})")
            heard[other] = count(other, "owner-changed")
    elif action == "read":
        name = rng.choice(SESSIONS)
        errors = count(name, "error")
        reads = count(name, "read ")
        send(name, "read text/plain")
        wait(lambda: count(name, "read ") > reads or count(name, "error") > errors, f"{name} read answered")
        got = [line for line in lines(name) if line.startswith("read ")][-1][5:] if count(name, "read ") > reads else None
        if lock is not None or model["text"] is None:
            if got is not None:
                fail(f"{name} read '{got}' while locked or empty")
        elif got != model["text"]:
            fail(f"{name} read '{got}', expected '{model['text']}'")
    elif action == "paste":
        got = paste()
        want = model["text"]
        if lock is not None and model["owner"] in SESSIONS:
            want = ""  # a paused session's source answers nothing
        if (got or "") != (want or ""):
            fail(f"local paste got '{got}', expected '{want}' (model {model}, locked {lock is not None})")
    elif action == "lock":
        if lock is None:
            # A FIFO held open read-write: the client unlocks on "unlock" and gives up on end of input.
            fifo = f"{RUNTIME}/model-lock-{SEED}-{index}.fifo"
            os.mkfifo(fifo)
            lock = subprocess.Popen([LOCK_CLIENT], stdin=os.open(fifo, os.O_RDWR), stdout=open(LOCK_LOG, "w"),
                                    stderr=subprocess.STDOUT)
            lock.command = open(fifo, "w", buffering=1)
            wait(lambda: "locked" in open(LOCK_LOG).read().split(), "the lock client locks")
            # The portal hears the lock through paused: wait until each session refuses a read.
            for name in SESSIONS:
                def refused(name=name):
                    errors = count(name, "error")
                    reads = count(name, "read ")
                    send(name, "read text/plain")
                    wait(lambda: count(name, "error") > errors or count(name, "read ") > reads, f"{name} probe read")
                    return count(name, "error") > errors
                wait(refused, f"{name} pauses after the lock")
            check_announced(False)
        else:
            lock.command.write("unlock\n")
            lock.wait(timeout=10)
            lock = None
            check_announced(True)


try:
    # Seeds share one compositor: start from the selection the previous seed left.
    for name in SESSIONS:
        heard[name] = count(name, "owner-changed")
        if last_flag(name).startswith("owner-changed owner=1"):
            model["owner"] = name
    model["text"] = paste() or None
    if model["text"] is not None and model["owner"] is None:
        model["owner"] = "local"
    for index in range(STEPS):
        step(index)
    if lock is not None:
        lock.command.write("unlock\n")
        lock.wait(timeout=10)
    print(f"seed {SEED}: {STEPS} steps hold", flush=True)
finally:
    for copy in copies:
        copy.kill()
