#!/usr/bin/env python3
# A session compositor stand-in for System1's sleep delay: HoldSleep, then on each PrepareForSleep(true) print
# "prepare" and, with argv[1] == "ack", call SleepReady. Prints "held <budget usec>" once registered.
import sys

import dbus
import dbus.mainloop.glib
from gi.repository import GLib

dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
bus = dbus.SystemBus()
login = dbus.Interface(bus.get_object("org.andromeda.System1", "/org/andromeda/System1/Login"),
                       "org.andromeda.System1.Login")


def on_sleep(start):
    if start:
        print("prepare", flush=True)
        if sys.argv[1] == "ack":
            login.SleepReady()


bus.add_signal_receiver(on_sleep, "PrepareForSleep", "org.andromeda.System1.Login", "org.andromeda.System1",
                        "/org/andromeda/System1/Login")
print(f"held {login.HoldSleep()}", flush=True)
GLib.MainLoop().run()
