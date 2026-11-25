#!/usr/bin/env python3
"""Drives org.freedesktop.portal.RemoteDesktop through the real frontend, one stdin command per line:
  session <persist_mode> <restore_token|->   sources   start   move <dx> <dy>   abs <stream> <x> <y>
  key <evdev code>   eis   clipboard   set <mime> <text>   read <mime>   close   quit
SelectionOwnerChanged and SelectionTransfer print lines too; a transfer is answered with the text of the last set.
Each result is one stdout line."""
import os
import stat
import sys
import threading

import gi

gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib  # noqa: E402

BUS = Gio.bus_get_sync(Gio.BusType.SESSION)
NAME = "org.freedesktop.portal.Desktop"
PATH = "/org/freedesktop/portal/desktop"
IFACE = "org.freedesktop.portal.RemoteDesktop"
SENDER = BUS.get_unique_name()[1:].replace(".", "_")
LOOP = GLib.MainLoop()
state = {"session": None, "counter": 0, "text": ""}
CLIPBOARD = "org.freedesktop.portal.Clipboard"


def say(line):
    print(line, flush=True)


def request(method, args, on_response, iface=IFACE):
    """Calls a Request-returning method and passes the Response (code, results) to on_response."""
    state["counter"] += 1
    token = f"t{os.getpid()}_{state['counter']}"
    handle = f"/org/freedesktop/portal/desktop/request/{SENDER}/{token}"
    sub = None

    def response(_bus, _sender, _path, _iface, _signal, params):
        BUS.signal_unsubscribe(sub)
        on_response(*params.unpack())

    sub = BUS.signal_subscribe(NAME, "org.freedesktop.portal.Request", "Response", handle, None,
                               Gio.DBusSignalFlags.NO_MATCH_RULE, response)
    BUS.call_sync(NAME, PATH, iface, method, args(token), None,
                  Gio.DBusCallFlags.NONE, -1, None)


def on_owner_changed(_bus, _sender, _path, _iface, _signal, params):
    session, options = params.unpack()
    if session == state["session"]:
        say(f"owner-changed owner={int(options.get('session_is_owner', False))} "
            f"types={','.join(options.get('mime_types', [])) or '-'}")


def on_transfer(_bus, _sender, _path, _iface, _signal, params):
    session, mime, serial = params.unpack()
    if session != state["session"]:
        return
    try:
        reply, fds = BUS.call_with_unix_fd_list_sync(
            NAME, PATH, CLIPBOARD, "SelectionWrite", GLib.Variant("(ou)", (session, serial)),
            None, Gio.DBusCallFlags.NONE, -1, None, None)
        fd = fds.get(reply.unpack()[0])
        with os.fdopen(fd, "wb") as pipe:
            pipe.write(state["text"].encode())
        ok = True
    except (GLib.Error, OSError) as error:
        say(f"write-failed {error}")
        ok = False
    BUS.call_sync(NAME, PATH, CLIPBOARD, "SelectionWriteDone", GLib.Variant("(oub)", (session, serial, ok)),
                  None, Gio.DBusCallFlags.NONE, -1, None)
    say(f"transferred {mime} ok={int(ok)}")


def read_pipe(fd):
    with os.fdopen(fd, "rb") as pipe:
        data = pipe.read().decode(errors="replace")
    GLib.idle_add(lambda: say(f"read {data or '-'}"))


def notify(method, args):
    try:
        BUS.call_sync(NAME, PATH, IFACE, method, GLib.Variant(f"(oa{{sv}}{args[0]})", (state["session"], {}, *args[1:])),
                      None, Gio.DBusCallFlags.NONE, -1, None)
        return True
    except GLib.Error as error:
        say(f"error {error.message}")
        return False


def command(line):
    words = line.split()
    if not words:
        return
    action = words[0]
    if action == "session":
        mode, token = int(words[1]), words[2]

        def created(code, results):
            if code != 0:
                say(f"session-failed response={code}")
                return
            state["session"] = results["session_handle"]
            BUS.signal_subscribe(NAME, "org.freedesktop.portal.Session", "Closed", state["session"], None,
                                 Gio.DBusSignalFlags.NO_MATCH_RULE, lambda *_: say("session-closed"))
            BUS.signal_subscribe(NAME, CLIPBOARD, "SelectionOwnerChanged", PATH, None, Gio.DBusSignalFlags.NONE,
                                 on_owner_changed)
            BUS.signal_subscribe(NAME, CLIPBOARD, "SelectionTransfer", PATH, None, Gio.DBusSignalFlags.NONE,
                                 on_transfer)
            options = {"types": GLib.Variant("u", 3), "persist_mode": GLib.Variant("u", mode)}
            if token != "-":
                options["restore_token"] = GLib.Variant("s", token)
            request("SelectDevices", lambda t: GLib.Variant("(oa{sv})", (state["session"], {
                "handle_token": GLib.Variant("s", t), **options})),
                lambda code, _results: say(f"selected response={code}"))

        request("CreateSession", lambda t: GLib.Variant("(a{sv})", ({
            "handle_token": GLib.Variant("s", t), "session_handle_token": GLib.Variant("s", f"s{t}")},)), created)
    elif action == "sources":
        request("SelectSources", lambda t: GLib.Variant("(oa{sv})", (state["session"], {
            "handle_token": GLib.Variant("s", t), "types": GLib.Variant("u", 1)})),
            lambda code, _results: say(f"sources response={code}"), "org.freedesktop.portal.ScreenCast")
    elif action == "start":
        def started(code, results):
            streams = ",".join(str(node) for node, _props in results.get("streams", [])) or "-"
            if results.get("clipboard_enabled"):
                say("clipboard-enabled")
            say(f"started response={code} devices={results.get('devices', 0)} "
                f"streams={streams} token={results.get('restore_token', '-')}")

        request("Start", lambda t: GLib.Variant("(osa{sv})", (state["session"], "", {
            "handle_token": GLib.Variant("s", t)})), started)
        say("start-called")
    elif action == "move":
        if notify("NotifyPointerMotion", ("dd", float(words[1]), float(words[2]))):
            say("moved")
    elif action == "abs":
        if notify("NotifyPointerMotionAbsolute", ("udd", int(words[1]), float(words[2]), float(words[3]))):
            say("absed")
    elif action == "key":
        code = int(words[1])
        if notify("NotifyKeyboardKeycode", ("iu", code, 1)) and notify("NotifyKeyboardKeycode", ("iu", code, 0)):
            say("keyed")
    elif action == "eis":
        try:
            reply, fds = BUS.call_with_unix_fd_list_sync(
                NAME, PATH, IFACE, "ConnectToEIS", GLib.Variant("(oa{sv})", (state["session"], {})),
                None, Gio.DBusCallFlags.NONE, -1, None, None)
            fd = fds.get(reply.unpack()[0])
            say("eis-fd " + ("socket" if stat.S_ISSOCK(os.fstat(fd).st_mode) else "other"))
            os.close(fd)
        except GLib.Error as error:
            say(f"eis-failed {error.message}")
    elif action == "clipboard":
        try:
            BUS.call_sync(NAME, PATH, CLIPBOARD, "RequestClipboard", GLib.Variant("(oa{sv})", (state["session"], {})),
                          None, Gio.DBusCallFlags.NONE, -1, None)
            say("clipboard-requested")
        except GLib.Error as error:
            say(f"error {error.message}")
    elif action == "set":
        state["text"] = " ".join(words[2:])
        try:
            BUS.call_sync(NAME, PATH, CLIPBOARD, "SetSelection", GLib.Variant("(oa{sv})", (state["session"], {
                "mime_types": GLib.Variant("as", [words[1]])})), None, Gio.DBusCallFlags.NONE, -1, None)
            say("set")
        except GLib.Error as error:
            say(f"error {error.message}")
    elif action == "read":
        try:
            reply, fds = BUS.call_with_unix_fd_list_sync(
                NAME, PATH, CLIPBOARD, "SelectionRead", GLib.Variant("(os)", (state["session"], words[1])),
                None, Gio.DBusCallFlags.NONE, -1, None, None)
            # A thread reads: the data may come from this client's own SelectionTransfer answer.
            threading.Thread(target=read_pipe, args=(fds.get(reply.unpack()[0]),), daemon=True).start()
        except GLib.Error as error:
            say(f"error {error.message}")
    elif action == "close":
        BUS.call_sync(NAME, state["session"], "org.freedesktop.portal.Session", "Close", None, None,
                      Gio.DBusCallFlags.NONE, -1, None)
        say("closed")
    elif action == "quit":
        LOOP.quit()


def on_stdin(channel, condition):
    if condition & GLib.IOCondition.HUP and not condition & GLib.IOCondition.IN:
        LOOP.quit()
        return False
    line = channel.readline()
    if not line:
        LOOP.quit()
        return False
    command(line.strip())
    return True


GLib.io_add_watch(GLib.IOChannel.unix_new(sys.stdin.fileno()), GLib.PRIORITY_DEFAULT,
                  GLib.IOCondition.IN | GLib.IOCondition.HUP, on_stdin)
say("ready")
LOOP.run()
