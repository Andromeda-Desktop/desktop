#!/usr/bin/env python3
"""Fail when code bypasses a core owner (.agent/TASK-core.md): each rule bans a call pattern outside its owner files.

Today's violations are listed in tools/core-check-allow.txt as "rule path count"; a count may only fall.
--update rewrites the list after a count fell; it refuses when any count rose.
"""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ALLOW = os.path.join(ROOT, "tools/core-check-allow.txt")
CPP = (".c", ".cc", ".cpp", ".h", ".hpp")
CPP_DIRS = ("compositor/src/", "shell/src/", "libdesk-ui/src/", "libdesk-app/src/", "libdesk-system/src/",
            "libdesk-term/src/", "apps/", "portal/src/", "indexer/src/", "andromeda-preview/", "session/", "system/")
LINE_COMMENT = re.compile(r"^\s*//.*$", re.M)
SKIP = re.compile(r"(^|/)(tests?|subprojects|build[^/]*|greeter)/|_test\.|^shell/src/greeter/|^greeter/")

# id: (why and owner, path prefixes, suffixes, pattern, owner paths exempt from the rule)
RULES = {
    "blur-mark": ("the scene keys its blur cache; no hand marks (C1)", ("compositor/src/",), CPP,
                  r"markBlurBackgroundDirty|wlr_scene_optimized_blur_mark_dirty", ()),
    "blur-create": ("blur nodes come from SurfaceBlur (C1b)", ("compositor/src/",), CPP,
                    r"\bwlr_scene_blur_create\(", ("compositor/src/scene/surface_blur.cpp",)),
    "fade-draw": ("fade and slide alpha come from scene/effect_draw.h (C2)", ("compositor/src/",), CPP,
                  r"std::sqrt\(\w*[Aa]lpha|m_perBufferFade\s*&&|\blifecycleEffect\([^)]*\)\s*!=\s*nullptr\s*\)?\s*[?;]",
                  ("compositor/src/scene/effect_draw.h",)),
    "shadow-create": ("shadow nodes come from SurfaceShadow and mirrorShadow; HintRect owns the accent glow (C3)",
                      ("compositor/src/",), CPP, r"\bwlr_scene_shadow_create\(",
                      ("compositor/src/scene/surface_shadow.cpp", "compositor/src/scene/hint_rect.cpp")),
    "comp-bus": ("the compositor talks D-Bus through bus::Client, one async connection per bus on the loop (AR3); "
                 "scanout_keeper is the capped sync hand-off, cli/ runs in its own process", ("compositor/src/",), CPP,
                 r"\bsd_bus_(open\w*|new|default\w*)\s*\(",
                 ("compositor/src/core/loop_bus.cpp", "compositor/src/core/scanout_keeper.cpp", "compositor/src/cli/")),
    "bus-floating": ("a bus callback with a floating slot outlives its owner: use bus::Client keep() or call() (AR3)",
                     ("compositor/src/",), CPP, r"\bsd_bus_\w+_async\(\s*[^,()]+,\s*nullptr\s*,", ()),
    "thread-detach": ("blocking work runs on BlockingPool (libdesk-ui core/blocking_pool.h): capped, counted, joined (AR3); "
                      "app_loop ends the process on its GL-init timeout", CPP_DIRS, CPP,
                      r"^\s*\}\)\.detach\(\);|std::thread\(.*\)\.detach\(\)", ("libdesk-app/src/app/app_loop.cpp",)),
    "wake-fd": ("a thread wakes a loop through sys::WakeFd (libdesk-ui core/sys/sys.h) or DeferredCall; in the compositor "
                "through MainLoopWorker (core/main_loop_worker.h) (AR3)", CPP_DIRS, CPP, r"\beventfd\s*\(",
                ("libdesk-ui/src/core/sys/sys_linux.cpp", "compositor/src/core/main_loop_worker.cpp")),
    "clipboard-wait": ("clipboard file I/O runs as SharedWorker jobs; only the destructor drain waits (AR3 C9)",
                       ("shell/src/clipboard/",), CPP, r"\.wait(_for|_until)?\(", ()),
    "schedule-frame": ("redraws go through the Output frame owner (C8)", ("compositor/src/",), CPP,
                       r"\bwlr_output_schedule_frame\(", ("compositor/src/output/",)),
    "output-at": ("output under a point via Server::outputAt (C11)", ("compositor/src/",), CPP,
                  r"\bwlr_output_layout_output_at\(", ("compositor/src/server/server_outputs.cpp",)),
    "glass-raw": ("island glass via glass_surface.h (sendIslandGlass, islandGlassTint, applyIslandTone) and "
                  "Style::islandGlass (C6)", ("shell/src/",), CPP,
                  r"\bsetGlassMaterial\(|\bsetPaletteTone\(|\bglassMaterial\(", ()),
    "dsk-create": ("one dsk_surface_v1 per wl_surface: WaylandSurface::dsk() (a second is a protocol error)",
                   CPP_DIRS, CPP, r"make_unique<DskSurfaceClient>|\bnew DskSurfaceClient\b",
                   ("libdesk-ui/src/wayland/surface.cpp",)),
    "shell-timer": ("shell repeats under 1 s or of unknown interval go through the frame host (C5)",
                    ("shell/src/shell/",), CPP, r"\bstartRepeating\(\s*(?!std::chrono::(?:seconds|minutes|hours)\b)([\w:]*)", ()),
    "const-frame-tick": ("needsFrameTick states a fact, never a constant true (C5)", ("shell/src/",), CPP,
                         r"needsFrameTick\(\)[^{;]*\{\s*return\s+true;", ()),
    "hand-spring": ("springs from tokens via Style::spring, motionSpring in the compositor (C11)", CPP_DIRS, CPP,
                    r"\.damping\s*=\s*[\w.:>-]+\.damping\b", ("libdesk-ui/src/ui/style.h", "compositor/src/config/config.h")),
    "radius-scale": ("radii through Style::scaledRadius, windowCornerRadius in the compositor (C11)", CPP_DIRS, CPP,
                     r"Tokens::\w*[Rr]adius\w*\s*\*|appearance\.cornerRadius\s*\*",
                     ("libdesk-ui/src/ui/style.cpp", "compositor/src/config/config.h")),
    "settings-control": ("a toggle or slider that writes config (setOverride) comes from SettingsControlFactory; "
                         "callback controls on live state use ui:: directly", ("shell/src/shell/settings/",), CPP,
                         r"\bui::(?:toggle|slider)\(\{(?:(?!\}\);)[\s\S]){0,1200}?\bsetOverride\b",
                         ("shell/src/shell/settings/settings_control_factory.cpp",)),
    "shell-string": ("command strings run only through process::shellArgv/runShell (libdesk-system) or execShell "
                     "(compositor core/process) or portal AsyncProcess; fixed commands pass argv (C12)", CPP_DIRS, CPP,
                     r"\"(/bin/)?sh\"\s*,\s*\"-l?c\"",
                     ("libdesk-system/src/core/process/process.cpp", "compositor/src/core/process.cpp",
                      "portal/src/dbus/request.cpp")),
    "raw-spawn": ("child processes via libdesk-system process; owners in binaries without it: compositor core/process "
                  "(forkChild, spawnProgram), portable libdesk-ui code core/sys, portal AsyncProcess, indexer Extractor "
                  "(C12)", CPP_DIRS, CPP, r"\b(v?fork|posix_spawnp?)\s*\(",
                  ("libdesk-system/src/core/process/", "compositor/src/core/process.cpp", "libdesk-ui/src/core/sys/",
                   "portal/src/dbus/request.cpp", "indexer/src/content_worker.cpp")),
    "exit-poll": ("child exit by pidfd in libdesk-system process; the compositor reaps in core/process (C12)", CPP_DIRS,
                  CPP, r"\bWNOHANG\b", ("libdesk-system/src/core/process/", "compositor/src/core/process.cpp")),
    "toml-include": ("toml++ through protocol/toml.h (its warning pragmas)", ("compositor/src/", "shell/src/", "libdesk-",
                     "apps/"), CPP, r"#include\s*<toml\+\+/toml\.hpp>", ("libdesk-ui/protocols/toml.h",)),
    "heap-trim": ("heap trim via heap::releaseFree (protocol/heap.h) or sys::releaseFreeHeap in libdesk-ui (C12)",
                  CPP_DIRS, CPP, r"\bmalloc_trim\s*\(", ()),
    "sync-image": ("Image::load decides sync or async decode (C14)", ("shell/src/",), CPP,
                   r"\b(setSourceFile|setSourceFileAsync|setSourceFileBlurred|reloadSourceFile)\(", ()),
    "logind-sleep": ("suspend comes from SessionActivity (Compositor1.Apps Activity, AR2); System1 alone reads logind "
                     "PrepareForSleep, the compositor only as the fallback without System1", CPP_DIRS, CPP + (".rs",),
                     r"\"PrepareForSleep\"|receive_prepare_for_sleep", ("system/crates/andromeda-system/src/login.rs",
                                          "compositor/src/server/sleep_watch.cpp")),
    "log-owner": ("one logger: protocol/log.{h,cpp}; binaries supply only log_platform hooks (the compositor also its "
                  "wlr bridge)", CPP_DIRS, CPP, r"\bvoid\s+logMessage\s*\(\s*LogLevel[^;]*\)\s*\{", ()),
    "raw-inotify": ("file watching goes through libdesk-ui DirectoryWatcher / FileWatcher; binaries without "
                    "libdesk-ui own one watcher each (compositor config + socket, portal config, indexer, agentd)",
                    CPP_DIRS, CPP + (".rs",), r"\binotify_(init1?|add_watch|rm_watch)\b|\binotify::|fs::inotify",
                    ("libdesk-ui/src/core/files/directory_watcher.cpp", "compositor/src/config/config_watcher.cpp",
                     "compositor/src/server/socket_recovery.cpp", "portal/src/config/config.cpp",
                     "indexer/src/live_index.cpp", "system/crates/andromeda-agentd/src/provider.rs")),
    "notify-client": ("apps send notifications through AppNotifier (libdesk-app), system crates through "
                      "andromeda_rt::notify; the shell serves them", CPP_DIRS, CPP + (".rs",),
                      r"callMethod(Async)?\(\"(Notify|CloseNotification)\"\)|\"(Notify|CloseNotification)\",",
                      ("libdesk-app/src/app/app_notifier_dbus.cpp", "system/crates/andromeda-rt/src/notify.rs",
                       "shell/src/dbus/notification/")),
    "rust-drop": ("D-Bus and job errors are logged, not dropped (C13)", ("system/", "apps/", "andromeda-preview/"),
                  (".rs",), r"let _ = [^;]*\.await|\.await\.ok\(\)", ()),
    "std-fs-async": ("no blocking file I/O on a daemon's runtime thread: tokio::fs or spawn_blocking (AR3 5e)",
                     ("system/", "apps/", "andromeda-preview/"), (".rs",),
                     r"\bstd::fs::\w+\s*\(|\bPath::new\([^;]*\)\.exists\(\)", ()),
    "peer-cred": ("socket peers are checked by ipc::acceptPeer / checkPeer (protocol/ipc_channel.h) or "
                  "andromeda_rt::ipc (AR4)", CPP_DIRS + ("protocol/",), CPP + (".rs",),
                  r"SOL_SOCKET\s*,\s*SO_PEER(CRED|PIDFD)\b|sockopt::Peer(Credentials|Pidfd)\b",
                  ("protocol/ipc_channel.", "system/crates/andromeda-rt/src/ipc.rs")),
    "bus-name": ("the System1 and bus-daemon names come from busname:: (protocol/bus_names.h) (C13)", CPP_DIRS, CPP,
                 r"\"(org\.andromeda\.System1|org\.freedesktop\.DBus|/org/freedesktop/DBus)\"", ()),
    "logind-direct": ("logind is reached through LogindService (Inhibit, session lock) or System1 (C13); own owners: "
                      "compositor sleep watch, greeter (GIO launcher), Files vault guard (own GIO main loop)", CPP_DIRS, CPP,
                      r"\"org\.freedesktop\.login1[\".]",
                      ("libdesk-system/src/dbus/logind/logind_service.", "compositor/src/server/sleep_watch.cpp",
                       "shell/src/greeter/", "apps/files/src/vaults.cpp")),
    "owner-watch": ("bus name owners are followed through NameOwnerWatch (libdesk-system dbus/name_owner_watch.h) "
                    "(C13); the compositor has no libdesk-system and keeps its sd-bus matches",
                    CPP_DIRS, CPP, r"uponSignal\(\"NameOwnerChanged\"\)|member='NameOwnerChanged'",
                    ("libdesk-system/src/dbus/name_owner_watch.", "compositor/src/")),
    "reply-drop": ("an async D-Bus reply nothing reads goes to logReply (libdesk-system dbus/reply_log.h): failures "
                   "are logged, not dropped (C13)", CPP_DIRS, CPP,
                   r"uponReplyInvoke\(\[[^\]]*\]\(std::optional<sdbus::Error>[^)]*\)\s*\{\s*\}", ()),
    "uri-codec": ("file URIs, percent escapes and text/uri-list go through protocol/uri.h (C17)", CPP_DIRS,
                  CPP + (".mm",), r"\bg_filename_(to|from)_uri\s*\(|\"text/uri-list\"|\"file://\"\s*\+", ()),
    "key-names": ("chord modifier and key aliases come from key_names:: (protocol/key_names.h); each grammar keeps "
                  "only its own extra tokens (C17)", CPP_DIRS, CPP,
                  r"==\s*\"(ctrl|control|ctl|shift|alt|option|super|logo|win|meta|mod4|esc|del|enter)\"", ()),
    "app-executable": ("apps start through appd: the app is a library, the executable only calls it (C7)",
                       ("apps/",), ("meson.build",), r"^executable\('(andromeda-[\w-]+)'", ()),
    "app-desk-ui": ("apps get libdesk-ui only through libdesk-app's desk_app_dep (AR7)",
                    ("apps/",), ("meson.build",), r"subproject\('libdesk-ui'|\b_?desk_ui_dep\b", ()),
}


def tracked():
    out = subprocess.run(["git", "-C", ROOT, "ls-files", "--cached", "--others", "--exclude-standard"], capture_output=True, text=True, check=True).stdout
    return [p for p in out.splitlines() if not SKIP.search(p) and os.path.isfile(os.path.join(ROOT, p))]


def _span_end(text, start, open_ch, close_ch):
    depth = 0
    for i in range(start, len(text)):
        if text[i] == open_ch:
            depth += 1
        elif text[i] == close_ch:
            depth -= 1
            if depth == 0:
                return i
    return len(text)


def async_hits(text, rx):
    """Matches inside async fn bodies and async blocks, outside spawn_blocking arguments."""
    inside = [(m.end() - 1, _span_end(text, m.end() - 1, "{", "}"))
              for m in re.finditer(r"\basync\s+(?:fn\b[^{;]*|move\s*)?\{", text)]
    blocking = [(m.end() - 1, _span_end(text, m.end() - 1, "(", ")"))
                for m in re.finditer(r"\bspawn_blocking\s*\(", text)]
    return [m.group(0) for m in rx.finditer(text)
            if any(a <= m.start() <= b for a, b in inside) and not any(a <= m.start() <= b for a, b in blocking)]


def outside_function(text, rx, name):
    """Matches outside the body of function `name` (the one place a rule allows its pattern)."""
    bodies = [(m.end() - 1, _span_end(text, m.end() - 1, "{", "}"))
              for m in re.finditer(rf"\b{re.escape(name)}\(\)\s*\{{", text)]
    return [m.group(0) for m in rx.finditer(text) if not any(a <= m.start() <= b for a, b in bodies)]


def whole_seconds(text, name):
    """True when `name` is a constant defined in `text` as std::chrono seconds, minutes or hours."""
    return bool(name) and re.search(rf"\b{re.escape(name)}\s*=\s*std::chrono::(?:seconds|minutes|hours)\(", text) is not None


def scan():
    found = {}
    files = tracked()
    for rule, (_, dirs, suffixes, pattern, owners) in RULES.items():
        rx = re.compile(pattern, re.M)
        for path in files:
            if not path.startswith(dirs) or not path.endswith(suffixes) or path.startswith(owners):
                continue
            text = open(os.path.join(ROOT, path), encoding="utf-8", errors="replace").read()
            if path.endswith(CPP):
                text = LINE_COMMENT.sub("", text)
            if rule == "std-fs-async":
                hits = async_hits(text, rx)
            elif rule == "clipboard-wait":
                hits = outside_function(text, rx, "ClipboardService::drainPendingWrites")
            elif rule == "shell-timer":
                hits = [m for m in rx.finditer(text) if not whole_seconds(text, m.group(1))]
            else:
                hits = rx.findall(text) if rule != "app-executable" else [
                    m.group(1) for m in rx.finditer(text) if f"shared_library('{m.group(1)}'" not in text]
            if hits:
                found[(rule, path)] = len(hits)
    return found


def load():
    allow = {}
    if os.path.exists(ALLOW):
        for line in open(ALLOW):
            if line.strip() and not line.startswith("#"):
                rule, path, count = line.split()
                allow[(rule, path)] = int(count)
    return allow


def write(found):
    with open(ALLOW, "w") as f:
        f.write("# core-check allowlist: rule path count. Counts only fall; tools/core-check.py --update after a fix.\n")
        for (rule, path), count in sorted(found.items()):
            f.write(f"{rule} {path} {count}\n")


def main():
    found, allow = scan(), load()
    if "--init" in sys.argv:
        write(found)
        print(f"core-check: wrote {len(found)} entries")
        return 0
    rose = {k: v for k, v in found.items() if v > allow.get(k, 0)}
    fell = {k: v for k, v in allow.items() if found.get(k, 0) < v}
    for (rule, path), count in sorted(rose.items()):
        print(f"core-check: {path}: {rule} x{count} (allowed {allow.get((rule, path), 0)}): {RULES[rule][0]}")
    if rose:
        return 1
    if fell and "--update" in sys.argv:
        write(found)
    elif fell:
        print(f"core-check: {len(fell)} counts fell; tighten with tools/core-check.py --update")
    total = sum(found.values())
    print(f"core-check: ok, {total} allowlisted violations in {len(RULES)} rules")
    return 0


if __name__ == "__main__":
    sys.exit(main())
