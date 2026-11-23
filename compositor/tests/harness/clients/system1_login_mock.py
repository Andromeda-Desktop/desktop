#!/usr/bin/env python3
# org.andromeda.System1 Login on the bus DBUS_SYSTEM_BUS_ADDRESS names, for sleep hold checks. Prints "up", then
# "hold" per HoldSleep (budget 1.2 s) and "ready MS" per SleepReady, MS since the last test.Login.Sleep(true).
# test.Login.Sleep(b) emits PrepareForSleep(b).
import time

import dbus
import dbus.mainloop.glib
import dbus.service
from gi.repository import GLib

LOGIN = "org.andromeda.System1.Login"
SLEEP_AT = [0.0]


def say(line):
    print(line, flush=True)


class Login(dbus.service.Object):
    @dbus.service.method(LOGIN, out_signature="t")
    def HoldSleep(self):
        say("hold")
        return dbus.UInt64(1_200_000)

    @dbus.service.method(LOGIN)
    def SleepReady(self):
        say(f"ready {int((time.monotonic() - SLEEP_AT[0]) * 1000)}")

    @dbus.service.method("test.Login", in_signature="b")
    def Sleep(self, start):
        if start:
            SLEEP_AT[0] = time.monotonic()
        self.PrepareForSleep(start)

    @dbus.service.signal(LOGIN, signature="b")
    def PrepareForSleep(self, start):
        pass


dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
bus = dbus.SystemBus()
name = dbus.service.BusName("org.andromeda.System1", bus)
login = Login(bus, "/org/andromeda/System1/Login")
say("up")
GLib.MainLoop().run()
