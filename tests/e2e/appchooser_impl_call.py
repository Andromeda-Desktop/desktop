#!/usr/bin/env python3
"""Calls org.freedesktop.impl.portal.AppChooser or Access on the Files picker process directly.
Usage: appchooser_impl_call.py choose <handle> <choice>... | access <handle>
Prints "PID <pid>", then "RESULT <response> <choice or id=value,...>" once the dialog answers.
SIGUSR1 makes it call Request.Close on <handle>, as the frontend does when the app closes its request."""
import os
import signal
import sys

from gi.repository import Gio, GLib

kind, handle = sys.argv[1], sys.argv[2]
bus = Gio.bus_get_sync(Gio.BusType.SESSION)
# The impl accepts only the frontend: on the private test bus this script is it.
owned = bus.call_sync("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "RequestName",
                      GLib.Variant("(su)", ("org.freedesktop.portal.Desktop", 3)), GLib.VariantType("(u)"),
                      Gio.DBusCallFlags.NONE, 5000, None).unpack()[0]
if owned != 1:
    sys.exit(f"could not own org.freedesktop.portal.Desktop ({owned})")
if kind == "choose":
    iface, method = "org.freedesktop.impl.portal.AppChooser", "ChooseApplication"
    args = GLib.Variant("(ossasa{sv})", (handle, "org.test.Caller", "", sys.argv[3:],
                                         {"content_type": GLib.Variant("s", "text/plain")}))
else:
    iface, method = "org.freedesktop.impl.portal.Access", "AccessDialog"
    options = {"grant_label": GLib.Variant("s", "Allow"),
               "choices": GLib.Variant("a(ssa(ss)s)", [("remember", "Remember", [], "true")])}
    args = GLib.Variant("(osssssa{sv})", (handle, "org.test.Caller", "", "Use the camera?", "Sub", "Body", options))
dest = "org.freedesktop.impl.portal.desktop.andromeda.Files"
loop = GLib.MainLoop()


def close_request():
    bus.call_sync(dest, handle, "org.freedesktop.impl.portal.Request", "Close", None, None,
                  Gio.DBusCallFlags.NONE, 5000, None)
    return GLib.SOURCE_REMOVE


def answered(_bus, res):
    response, results = bus.call_finish(res).unpack()
    detail = results.get("choice", "") or ",".join(f"{k}={v}" for k, v in results.get("choices", []))
    print(f"RESULT {response} {detail}", flush=True)
    loop.quit()


GLib.unix_signal_add(GLib.PRIORITY_DEFAULT, signal.SIGUSR1, close_request)
print(f"PID {os.getpid()}", flush=True)
bus.call(dest, "/org/freedesktop/portal/desktop", iface, method, args, GLib.VariantType("(ua{sv})"),
         Gio.DBusCallFlags.NONE, 120000, None, answered)
loop.run()
