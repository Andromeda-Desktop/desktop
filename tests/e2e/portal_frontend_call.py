#!/usr/bin/env python3
"""Stands in for xdg-desktop-portal for one call: owns org.freedesktop.portal.Desktop, calls an impl method and
prints the reply as JSON. Takes the name over from a stand-in that has not left the bus yet.
Usage: portal_frontend_call.py <dest> <interface.Method> [<GVariant tuple text> [<object path>]]"""
import json
import sys

from gi.repository import Gio, GLib

dest, member = sys.argv[1:3]
interface, method = member.rsplit(".", 1)
args = GLib.Variant.parse(None, sys.argv[3], None, None) if len(sys.argv) > 3 and sys.argv[3] else None
bus = Gio.bus_get_sync(Gio.BusType.SESSION)
owned = bus.call_sync("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "RequestName",
                      GLib.Variant("(su)", ("org.freedesktop.portal.Desktop", 3)), GLib.VariantType("(u)"),
                      Gio.DBusCallFlags.NONE, 5000, None).unpack()[0]
if owned != 1:
    sys.exit(f"could not own org.freedesktop.portal.Desktop ({owned})")
path = sys.argv[4] if len(sys.argv) > 4 else "/org/freedesktop/portal/desktop"
reply = bus.call_sync(dest, path, interface, method, args, None,
                      Gio.DBusCallFlags.NONE, 10000, None)
print(json.dumps(reply.unpack()))
