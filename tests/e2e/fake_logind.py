#!/usr/bin/env python3
"""A minimal org.freedesktop.login1 for tests: Inhibit returns a pipe fd and prints "INHIBIT <what> <mode>";
a line "sleep" or "wake" on stdin emits PrepareForSleep(true) or (false); "OWNED" means clients can call it.
With FAKE_COMPOSITOR_RELAY=1 it also stands in for the compositor on the session bus: Compositor1.Apps Activity
follows the line."""
import os
import sys

from gi.repository import Gio, GLib

XML = """<node><interface name="org.freedesktop.login1.Manager">
<method name="Inhibit"><arg type="s" direction="in"/><arg type="s" direction="in"/><arg type="s" direction="in"/>
<arg type="s" direction="in"/><arg type="h" direction="out"/></method>
<signal name="PrepareForSleep"><arg type="b"/></signal>
</interface></node>"""
keep = []
session = []


def on_call(conn, sender, path, iface, method, params, invocation):
    what, _who, _why, mode = params.unpack()
    read, write = os.pipe()
    keep.append(read)
    fds = Gio.UnixFDList.new_from_array([write])
    print(f"INHIBIT {what} {mode}", flush=True)
    invocation.return_value_with_unix_fd_list(GLib.Variant("(h)", (0,)), fds)


def on_bus(conn, name):
    node = Gio.DBusNodeInfo.new_for_xml(XML)
    conn.register_object("/org/freedesktop/login1", node.interfaces[0], on_call, None, None)

    def on_stdin(source, condition):
        line = sys.stdin.readline()
        if not line:
            return False
        if line.strip() in ("sleep", "wake"):
            conn.emit_signal(None, "/org/freedesktop/login1", "org.freedesktop.login1.Manager", "PrepareForSleep",
                             GLib.Variant("(b)", (line.strip() == "sleep",)))
            for bus in session:
                activity = "sleeping" if line.strip() == "sleep" else "active"
                bus.emit_signal(None, "/org/andromeda/Compositor1/Apps", "org.freedesktop.DBus.Properties",
                                "PropertiesChanged", GLib.Variant("(sa{sv}as)", (
                                    "org.andromeda.Compositor1.Apps", {"Activity": GLib.Variant("s", activity)}, [])))
            print(f"{line.strip().upper()} sent", flush=True)
        return True

    GLib.io_add_watch(sys.stdin.fileno(), GLib.IO_IN | GLib.IO_HUP, on_stdin)


Gio.bus_own_name(Gio.BusType.SYSTEM, "org.freedesktop.login1", Gio.BusNameOwnerFlags.NONE, on_bus,
                 lambda conn, name: print("OWNED", flush=True), None)
if os.environ.get("FAKE_COMPOSITOR_RELAY"):
    Gio.bus_own_name(Gio.BusType.SESSION, "org.andromeda.Compositor1", Gio.BusNameOwnerFlags.DO_NOT_QUEUE,
                     lambda conn, name: session.append(conn), None, None)
GLib.MainLoop().run()
