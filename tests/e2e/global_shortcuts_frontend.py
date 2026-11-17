#!/usr/bin/env python3
"""Stands in for xdg-desktop-portal: owns org.freedesktop.portal.Desktop and binds one shortcut on the shell's
GlobalShortcuts impl. Usage: global_shortcuts_frontend.py <app-id> <shortcut-id> <description>
Prints "BOUND <response> <id>=<trigger_description>...", then "SIGNAL <name> <id>" per press signal, and exits
after the first Deactivated or after 20 s."""
import sys

from gi.repository import Gio, GLib

app_id, shortcut_id, description = sys.argv[1:4]
impl = "org.freedesktop.impl.portal.desktop.andromeda.shell"
path = "/org/freedesktop/portal/desktop"
iface = "org.freedesktop.impl.portal.GlobalShortcuts"
session = "/org/freedesktop/portal/desktop/session/1_1/e2e"

bus = Gio.bus_get_sync(Gio.BusType.SESSION)
owned = bus.call_sync("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "RequestName",
                      GLib.Variant("(su)", ("org.freedesktop.portal.Desktop", 4)), GLib.VariantType("(u)"),
                      Gio.DBusCallFlags.NONE, 5000, None).unpack()[0]
if owned != 1:
    sys.exit(f"could not own org.freedesktop.portal.Desktop ({owned})")

loop = GLib.MainLoop()


def on_signal(_bus, _sender, _path, _iface, name, params):
    handle, sid = params.unpack()[:2]
    if handle != session:
        return
    print(f"SIGNAL {name} {sid}", flush=True)
    if name == "Deactivated":
        loop.quit()


for name in ("Activated", "Deactivated"):
    bus.signal_subscribe(impl, iface, name, path, None, Gio.DBusSignalFlags.NONE, on_signal)


def call(method, signature, args):
    return bus.call_sync(impl, path, iface, method, GLib.Variant(signature, args), GLib.VariantType("(ua{sv})"),
                         Gio.DBusCallFlags.NONE, 10000, None).unpack()


call("CreateSession", "(oosa{sv})", ("/org/freedesktop/portal/desktop/request/1_1/c", session, app_id, {}))
response, results = call("BindShortcuts", "(ooa(sa{sv})sa{sv})", (
    "/org/freedesktop/portal/desktop/request/1_1/b", session,
    [(shortcut_id, {"description": GLib.Variant("s", description)})], "", {}))
listed = " ".join(f"{sid}={props.get('trigger_description', '')}" for sid, props in results.get("shortcuts", []))
print(f"BOUND {response} {listed}", flush=True)
GLib.timeout_add_seconds(20, loop.quit)
loop.run()
