#!/usr/bin/env python3
"""A stand-in org.andromeda.Link1 on the session bus for headless shots of the Link app.

It serves the real interface XML with the mockup's sample data (one connected phone, a missed call), then emits a
text, a file offer and notifications once a client calls GetAll. Usage: fake_link1.py <Link1.xml> [sample|away|unpaired]
"""
import os
import sys
import time

import gi

gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib  # noqa: E402

XML, MODE = sys.argv[1], (sys.argv[2] if len(sys.argv) > 2 else "sample")
PATH, IFACE = "/org/andromeda/Link1", "org.andromeda.Link1"
now_ms = int(time.time() * 1000)
today = time.localtime()


def at(hour, minute):
    return int(time.mktime((today.tm_year, today.tm_mon, today.tm_mday, hour, minute, 0, 0, 0, -1)) * 1000)


def ago(days, hour, minute):
    return min(at(hour, minute) - days * 86_400_000, now_ms - 60_000)


MAYA, DAD, PARCEL, JONAS, LENA = "+4915112340001", "+491701234567", "+49 151 2345 6789", "+4915790001111", "+4917622223333"
# (id, thread, address, body, date ms, sent): the mockup's threads; ids 7, 8 and 5 arrive as TextReceived (unread).
texts = [
    (1, 1, MAYA, "Dinner Friday? The new place on Oranienstraße", ago(1, 19, 2), False),
    (2, 1, MAYA, "Yes! 19:00 works", ago(1, 19, 5), True),
    (7, 1, MAYA, "Leaving the office now", ago(0, 18, 30), False),
    (8, 1, MAYA, "Running 10 min late, order me the pasta?", ago(0, 18, 42), False),
    (5, 2, DAD, "Call me when you're free", ago(0, 17, 5), False),
    (6, 3, PARCEL, "Ihr Paket wird heute zwischen 14:00 und 16:00 Uhr zugestellt. Sendungsverfolgung: "
                   "https://example.com/a/very/long/path/that/never/breaks/0123456789abcdef", ago(0, 12, 10), False),
    (4, 4, JONAS, "Sounds good, see you Friday", ago(2, 16, 0), False),
    (3, 5, LENA, "Photo", ago(3, 9, 30), False),
]
contacts = [("Maya Okafor", [MAYA]), ("Dad", ["0170 1234567"]), ("Jonas Weber", [JONAS]), ("Lena", [LENA])]
connected = MODE == "sample"
props = {
    "Devices": GLib.Variant("a(ssb)", [] if MODE == "unpaired" else [("p1", "Pixel 8", connected)]),
    "DeviceInfo": GLib.Variant("a{sa{sv}}", {} if MODE == "unpaired" else {"p1": {
        "transport": GLib.Variant("s", "wifi" if connected else ""),
        "connected": GLib.Variant("b", connected),
        "last_seen": GLib.Variant("x", now_ms // 1000 - 3600),
        "battery": GLib.Variant("i", 74)}}),
    "DeviceStatus": GLib.Variant("a{s(ubsi)}", {"p1": (74, False, "wifi", -1)} if connected else {}),
    "DeviceControls": GLib.Variant("a{s(isii)}", {"p1": (50, "normal", 0, 0)} if connected else {}),
    "Paused": GLib.Variant("as", []),
    "Pairing": GLib.Variant("b", False),
    "Texts": GLib.Variant("a{sa(ttsstb)}", {"p1": texts} if connected else {}),
    "Contacts": GLib.Variant("a{sa(sas)}", {"p1": contacts}),
    "Calls": GLib.Variant("a{sa(sstus)}", {"p1": [("+14155550199", "Mum", min(at(18, 20), now_ms - 60_000), 0, "missed")]}
                          if connected else {}),
}

node = Gio.DBusNodeInfo.new_for_xml(open(XML).read())
iface = node.lookup_interface(IFACE)
conn = Gio.bus_get_sync(Gio.BusType.SESSION)


def emit(name, signature, *args):
    conn.emit_signal(None, PATH, IFACE, name, GLib.Variant(signature, args))


def emit_sample():
    if not connected:
        return False
    for (id_, _thread, address, body, _at, _sent) in texts:
        if id_ in (5, 7, 8):
            emit("TextReceived", "(stss)", "p1", id_, address, body)
    if os.environ.get("PAGE", "waiting") != "waiting":
        return False  # no shell popups over another page's caption
    emit("TransferOffered", "(ssa(st))", "t1", "p1", [("Quarterly report Q3 final (2).pdf", 2_400_000)])
    emit("NotificationPosted", "(sssssaya(ssb)b)", "p1", "n1", "Slack", "#release", "v2026.11.6 is out", b"",
         [("reply", "Reply", True)], False)
    emit("NotificationPosted", "(sssssaya(ssb)b)", "p1", "n2", "Calendar", "Dentist", "Tomorrow 09:30", b"", [], False)
    return False


def call(_conn, _sender, _path, _iface, method, params, invocation):
    print("call", method, params, flush=True)
    invocation.return_value(None)


def get(_conn, _sender, _path, _iface, name):
    if name in props:
        return props[name]
    sig = iface.lookup_property(name).signature
    empty = {"b": False, "s": ""}
    return GLib.Variant(sig, {} if sig.startswith("a{") else [] if sig[0] == "a" else empty.get(sig, 0))


conn.register_object(PATH, iface, call, get, None)
# The app's own bus name: its proxy listens from then on.
Gio.bus_watch_name_on_connection(conn, "dev.andromeda.Link", Gio.BusNameWatcherFlags.NONE,
                                 lambda *_: GLib.timeout_add(800, emit_sample), None)
Gio.bus_own_name_on_connection(conn, IFACE, Gio.BusNameOwnerFlags.NONE, None, None)
print("fake link1 ready", flush=True)
GLib.MainLoop().run()
