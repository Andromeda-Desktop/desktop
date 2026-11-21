#!/usr/bin/env python3
"""A stand-in org.andromeda.Link1 on the session bus for headless shots of the Link app.

It serves the real interface XML with the mockup's sample data (one connected phone, a missed call), then emits a
text, a file offer and notifications once a client calls GetAll. Usage: fake_link1.py <Link1.xml> [sample|away|unpaired]
"""
import itertools
import os
import sys
import socket
import struct
import threading
import time
import zlib

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
connected = MODE in ("sample", "two")
props = {
    "Devices": GLib.Variant("a(ssb)", [] if MODE == "unpaired" else [("p1", "Pixel 8", connected)] + ([("p2", "Galaxy A54", False)] if MODE == "two" else [])),
    "DeviceInfo": GLib.Variant("a{sa{sv}}", {} if MODE == "unpaired" else {"p1": {
        "transport": GLib.Variant("s", "wifi" if connected else ""),
        "connected": GLib.Variant("b", connected),
        "last_seen": GLib.Variant("x", now_ms // 1000 - 12 * 60),
        "battery": GLib.Variant("i", 74)}} | ({"p2": {
        "connected": GLib.Variant("b", False),
        "last_seen": GLib.Variant("x", now_ms // 1000 - 3 * 3600)}} if MODE == "two" else {})),
    "DeviceStatus": GLib.Variant("a{s(ubsi)}", {"p1": (74, False, "wifi", -1)} if connected else {}),
    "DeviceControls": GLib.Variant("a{s(isii)}", {"p1": (50, "normal", 0, 0)} if connected else {}),
    "Paused": GLib.Variant("as", []),
    "Pairing": GLib.Variant("b", False),
    "Grants": GLib.Variant("a{sas}", {"p1": ["notifications", "clipboard", "screen", "files", "messages", "calls"]}),
    "AutoAccept": GLib.Variant("as", []),
    "NotificationsWhileLocked": GLib.Variant("b", False),
    "Commands": GLib.Variant("a(ss)", [("Lock", "loginctl lock-session"), ("Suspend", "systemctl suspend"),
                                       ("Backup", "rsync -a ~/Documents /mnt/backup")]),
    "Texts": GLib.Variant("a{sa(ttsstb)}", {"p1": texts} if connected else {}),
    "Contacts": GLib.Variant("a{sa(sas)}", {"p1": contacts}),
    "Calls": GLib.Variant("a{sa(sstus)}", {"p1": [
        ("+14155550199", "Mum", ago(0, 18, 20), 0, "missed"),
        (MAYA, "", ago(0, 12, 4), 192, "outgoing"),
        (DAD, "Dad", ago(1, 19, 40), 768, "incoming"),
        (PARCEL, "", ago(2, 10, 2), 41, "incoming"),
    ]} if connected else {}),
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
    if os.environ.get("HOTSPOT"):
        emit("HotspotRequested", "(ss)", "p1", "DIRECT-Link-4F2A")
    page = os.environ.get("PAGE", "waiting")
    if page == "activity":
        emit_activity()
    if page != "waiting":
        return False  # no shell popups over another page's caption
    emit("TransferOffered", "(ssa(st))", "t1", "p1", [("Quarterly report Q3 final (2).pdf", 2_400_000)])
    emit("NotificationPosted", "(sssssaya(ssb)b)", "p1", "n1", "Slack", "#release", "v2026.11.6 is out", b"",
         [("reply", "Reply", True)], False)
    emit("NotificationPosted", "(sssssaya(ssb)b)", "p1", "n2", "Calendar", "Dentist", "Tomorrow 09:30", b"", [], False)
    return False


def emit_activity():
    home = os.path.expanduser("~")
    emit("TransferOffered", "(ssa(st))", "t3", "p1", [("IMG_20261120_181203.jpg", 3_400_000)])
    emit("TransferProgress", "(stt)", "t3", 3_400_000, 3_400_000)
    emit("TransferFinished", "(ssas)", "t3", "done", [home + "/Pictures/IMG_20261120_181203.jpg"])
    emit("TransferProgress", "(stt)", "t2", 14_000_000, 14_000_000)
    emit("TransferFinished", "(ssas)", "t2", "done", [])
    emit("TransferProgress", "(stt)", "t5", 0, 2_000_000)
    emit("TransferFinished", "(ssas)", "t5", "failed", [])
    emit("TransferProgress", "(stt)", "t4", 10_000_000, 160_000_000)
    GLib.timeout_add(500, lambda: emit("TransferProgress", "(stt)", "t4", 11_050_000, 160_000_000) or False)
    emit("TransferOffered", "(ssa(st))", "t1", "p1", [("Quarterly report Q3 final (2).pdf", 2_400_000)])


PALETTE = [((255, 40, 90), (150, 60, 200)), ((10, 25, 60), (60, 150, 210)), ((250, 250, 248), (225, 220, 210)),
           ((120, 40, 200), (40, 60, 255)), ((240, 90, 170), (200, 60, 160))]
photos = [(f"/DCIM/Camera/IMG_{i:03}.jpg", f"IMG_{i:03}.jpg", (ago(0, 18, 0) if i < 11 else ago(1, 12, 0)) // 1000 - i * 60)
          for i in range(18)]


def png(index, size=64):
    """A diagonal gradient PNG: one of the mockup's five photo tones."""
    a, b = PALETTE[index % len(PALETTE)]
    rows = b"".join(b"\0" + bytes(round(a[c] + (b[c] - a[c]) * (x + y) / (2 * size)) for x in range(size) for c in range(3))
                    for y in range(size))
    chunk = lambda kind, data: struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))


def stream_mirror(sock):
    """MIRROR env: an Annex B H.264 clip with an AUD per access unit; one unit per access unit at MIRROR_FPS (30), looped."""
    fps = int(os.environ.get("MIRROR_FPS", "30"))
    data = open(os.environ["MIRROR"], "rb").read()
    starts = [i for i in range(len(data) - 4) if data[i:i + 5] == b"\0\0\0\x01\x09"] + [len(data)]
    units = [data[a:b] for a, b in zip(starts, starts[1:])]
    try:
        start = time.monotonic()
        for k in itertools.count():
            n = k % len(units)
            sock.sendall(struct.pack(">IQB", len(units[n]), k * 1_000_000 // fps, 1 if n == 0 else 0) + units[n])
            time.sleep(max(0.0, start + (k + 1) / fps - time.monotonic()))
    except OSError:
        sock.close()


def call(_conn, _sender, _path, _iface, method, params, invocation):
    print("call", method, params, flush=True)
    if method == "OpenMirror":
        viewer, phone = socket.socketpair()
        threading.Thread(target=stream_mirror, args=(phone,), daemon=True).start()
        fds = Gio.UnixFDList.new_from_array([viewer.detach()])
        invocation.return_value_with_unix_fd_list(GLib.Variant("(huu)", (0, 1080, 2400)), fds)
        if os.environ.get("CORNERS"):  # corner radius in 1/10000 of the width, sent as the phone's ScreenShape
            radius = int(os.environ["CORNERS"])
            GLib.timeout_add(300, lambda: emit("ScreenShape", "(saqa(qqqq))", params[0], [radius] * 4, []) and False)
        return
    if method == "ListFiles":
        folders = {"/": [
                       ("DCIM", True, 0, ago(0, 17, 0) // 1000), ("Documents", True, 0, ago(4, 10, 0) // 1000),
                       ("Download", True, 0, ago(0, 16, 0) // 1000),
                       ("Quarterly report Q3 final (2).pdf", False, 2_400_000, ago(0, 18, 31) // 1000),
                       ("boarding-pass-LH2034.pdf", False, 182_000, ago(3, 9, 0) // 1000),
                       ("IMG_20261120_181203.jpg", False, 3_100_000, ago(0, 18, 12) // 1000)]}
        invocation.return_value(GLib.Variant("(a(sbtt))", (folders.get(params[1], []),)))
        return
    if method == "RecentPhotos":
        invocation.return_value(GLib.Variant("(a(sst))", (photos if connected else [],)))
        return
    if method == "Thumbnail":
        index = next(i for i, p in enumerate(photos) if p[0] == params[1])
        invocation.return_value(GLib.Variant("(ay)", (png(index),)))
        return
    if method == "SavePhotoTo":
        saved = os.path.join(params[2], params[1].rsplit("/", 1)[1])
        with open(saved, "wb") as out:
            out.write(b"photo")
        invocation.return_value(GLib.Variant("(s)", (saved,)))
        return
    if method == "SavePhoto":
        invocation.return_value(GLib.Variant("(s)", ("/home/user/Downloads/" + params[1].rsplit("/", 1)[1],)))
        return
    if method in ("StartPairing", "CancelPairing"):
        props["Pairing"] = GLib.Variant("b", method == "StartPairing")
        conn.emit_signal(None, PATH, "org.freedesktop.DBus.Properties", "PropertiesChanged",
                         GLib.Variant("(sa{sv}as)", (IFACE, {"Pairing": props["Pairing"]}, [])))
    if method == "StartPairing":
        invocation.return_value(GLib.Variant("(ss)", ("482913", "andromeda-link://pair?code=482913&host=sitish-laptop")))
        return
    invocation.return_value(None)


slow = [float(os.environ.get("SLOW", "0"))]  # seconds the first property read waits: a slow daemon start


def get(_conn, _sender, _path, _iface, name):
    if slow[0] > 0:
        time.sleep(slow[0])
        slow[0] = 0
    if name in props:
        return props[name]
    sig = iface.lookup_property(name).signature
    empty = {"b": False, "s": ""}
    return GLib.Variant(sig, {} if sig.startswith("a{") else [] if sig[0] == "a" else empty.get(sig, 0))


conn.register_object(PATH, iface, call, get, None)
quick = {"Visible": GLib.Variant("b", True), "Name": GLib.Variant("s", "sitish-laptop")}
conn.register_object(PATH, node.lookup_interface(IFACE + ".QuickShare"), call, lambda *a: quick[a[4]], None)
# The app's own bus name: its proxy listens from then on.
Gio.bus_watch_name_on_connection(conn, "dev.andromeda.Link", Gio.BusNameWatcherFlags.NONE,
                                 lambda *_: GLib.timeout_add(800, emit_sample), None)
Gio.bus_own_name_on_connection(conn, IFACE, Gio.BusNameOwnerFlags.NONE, None, None)
print("fake link1 ready", flush=True)
GLib.MainLoop().run()
