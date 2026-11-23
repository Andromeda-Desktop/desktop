#!/usr/bin/env python3
# SleepWatch (tests/unit/sleep_watch_child.cpp, argv[1]) against a mock logind and a mock System1 Login on a private
# bus (run under dbus-run-session). Without System1 it holds its own logind delay lock until ready() or the late
# callback; with System1 it calls HoldSleep, drops the own lock, follows only System1's PrepareForSleep and answers
# SleepReady; when System1 goes, the own lock comes back.
import os
import subprocess
import sys
import time

import dbus
import dbus.mainloop.glib
import dbus.service
from gi.repository import GLib

LOGIND = "org.freedesktop.login1"
MANAGER = "org.freedesktop.login1.Manager"
SYSTEM1 = "org.andromeda.System1"
LOGIN = "org.andromeda.System1.Login"
# Budgets: the child's late callback runs 1 s before the hold budget ends, so 0.2 s after PrepareForSleep(true).
LOGIND_DELAY_MAX_US = 1_700_000
SYSTEM1_BUDGET_US = 1_200_000
LATE_S = 0.2

seen = {"inhibit": 0, "released": 0, "get": 0, "ready": 0}
holds = []
lines = []


def fail(what):
    print(f"FAIL: {what}\nchild: {lines}\ncounts: {seen} holds: {holds}", file=sys.stderr)
    sys.exit(1)


def released(fd, _condition):
    seen["released"] += 1
    os.close(fd)
    return False


class Logind(dbus.service.Object):
    @dbus.service.method(MANAGER, in_signature="ssss", out_signature="h")
    def Inhibit(self, what, who, why, mode):
        if (what, mode) != ("sleep", "delay"):
            fail(f"Inhibit {what} {mode}")
        read, write = os.pipe()
        GLib.io_add_watch(write, GLib.PRIORITY_DEFAULT, GLib.IO_ERR, released)
        seen["inhibit"] += 1
        fd = dbus.types.UnixFd(read)
        os.close(read)
        return fd

    @dbus.service.method("org.freedesktop.DBus.Properties", in_signature="ss", out_signature="v")
    def Get(self, interface, name):
        seen["get"] += 1
        return dbus.UInt64(LOGIND_DELAY_MAX_US, variant_level=1)

    @dbus.service.signal(MANAGER, signature="b")
    def PrepareForSleep(self, start):
        pass


class Login(dbus.service.Object):
    @dbus.service.method(LOGIN, out_signature="t", sender_keyword="sender")
    def HoldSleep(self, sender=None):
        holds.append(sender)
        return dbus.UInt64(SYSTEM1_BUDGET_US)

    @dbus.service.method(LOGIN, in_signature="")
    def SleepReady(self):
        seen["ready"] += 1

    @dbus.service.signal(LOGIN, signature="b")
    def PrepareForSleep(self, start):
        pass


def on_output(fd, _condition):
    data = os.read(fd, 4096)
    lines.extend(data.decode().split())
    return bool(data)


def wait(predicate, what, timeout=3.0):
    end = time.monotonic() + timeout
    while not predicate():
        if time.monotonic() > end:
            fail(what)
        GLib.MainContext.default().iteration(True)


def settle(seconds):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        GLib.MainContext.default().iteration(True)


def said(word):
    return " ".join(lines).count(word)


dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
bus = dbus.SessionBus()
logind_name = dbus.service.BusName(LOGIND, bus)
logind = Logind(bus, "/org/freedesktop/login1")
login = Login(bus, "/org/andromeda/System1/Login")
GLib.timeout_add(10, lambda: True)

env = dict(os.environ, DBUS_SYSTEM_BUS_ADDRESS=os.environ["DBUS_SESSION_BUS_ADDRESS"])
child = subprocess.Popen([sys.argv[1]], stdin=subprocess.PIPE, stdout=subprocess.PIPE, env=env)
GLib.io_add_watch(child.stdout.fileno(), GLib.PRIORITY_DEFAULT, GLib.IO_IN | GLib.IO_HUP, on_output)


def tell_ready():
    child.stdin.write(b"ready\n")
    child.stdin.flush()


try:
    # No System1: the own logind delay lock holds each sleep until ready().
    wait(lambda: seen["inhibit"] == 1 and seen["get"] == 1, "no own delay lock without System1")
    logind.PrepareForSleep(True)
    wait(lambda: said("sleeping 1") == 1, "PrepareForSleep(true) not followed")
    settle(LATE_S / 2)
    if seen["released"] != 0:
        fail("the own lock ended before ready()")
    tell_ready()
    wait(lambda: seen["released"] == 1, "ready() kept the own lock")
    logind.PrepareForSleep(False)
    wait(lambda: said("sleeping 0") == 1 and seen["inhibit"] == 2, "no new own lock after resume")
    start = time.monotonic()
    logind.PrepareForSleep(True)
    wait(lambda: said("late") == 1 and seen["released"] == 2, "no late callback at the logind budget")
    if not LATE_S * 0.75 <= time.monotonic() - start <= LATE_S + 0.4:
        fail(f"late after {time.monotonic() - start:.2f} s, wanted {LATE_S} s")
    logind.PrepareForSleep(False)
    wait(lambda: seen["inhibit"] == 3, "no own lock after the second resume")

    # System1 appears: HoldSleep replaces the own lock; logind's own signal no longer counts.
    system1_name = dbus.service.BusName(SYSTEM1, bus)
    wait(lambda: len(holds) == 1 and seen["released"] == 3, "System1 did not replace the own lock")
    sleeps = said("sleeping 1")
    logind.PrepareForSleep(True)
    settle(LATE_S)
    if said("sleeping 1") != sleeps:
        fail("logind's PrepareForSleep counted while System1 holds sleep")
    login.PrepareForSleep(True)
    wait(lambda: said("sleeping 1") == sleeps + 1, "System1's PrepareForSleep not followed")
    settle(LATE_S / 2)
    if seen["ready"] != 0:
        fail("SleepReady before ready()")
    tell_ready()
    wait(lambda: seen["ready"] == 1, "ready() sent no SleepReady")
    login.PrepareForSleep(False)
    wait(lambda: said("sleeping 0") == 3, "System1's resume not followed")
    start = time.monotonic()
    login.PrepareForSleep(True)
    wait(lambda: said("late") == 2 and seen["ready"] == 2, "no late SleepReady at System1's budget")
    if not LATE_S * 0.75 <= time.monotonic() - start <= LATE_S + 0.4:
        fail(f"late after {time.monotonic() - start:.2f} s, wanted {LATE_S} s")
    if seen["inhibit"] != 3:
        fail("an own lock while System1 holds sleep")
    login.PrepareForSleep(False)

    # System1 goes: the own lock comes back.
    bus.release_name(SYSTEM1)
    wait(lambda: seen["inhibit"] == 4, "no own lock after System1 left")
finally:
    child.stdin.close()
    child.wait(timeout=5)
print("PASS")
