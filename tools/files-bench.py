#!/usr/bin/env python3
"""Launch a file manager on a folder and measure first buffer, settle, CPU and PSS.

Metrics follow Flea's field bench: settled = last process-tree CPU tick before 500 ms
of quiet. First buffer = first wl_surface.attach (WAYLAND_DEBUG), so both apps use one clock.
"""
import argparse, json, os, re, signal, statistics, subprocess, sys, threading, time

APPS = {
    "files": lambda root, d: [f"{root}/apps/files/build/andromeda-files", d],
    "flea": lambda root, d: [os.environ.get("FLEA_BIN", "/data/bench-ext/flea/target/release/flea"), "--gui", d],
}
TICK = os.sysconf("SC_CLK_TCK")


def tree(pid):
    out, todo = [], [pid]
    while todo:
        p = todo.pop()
        out.append(p)
        try:
            for t in os.listdir(f"/proc/{p}/task"):
                with open(f"/proc/{p}/task/{t}/children") as f:
                    todo += [int(c) for c in f.read().split()]
        except OSError:
            pass
    return out


def cpu_ticks(pids):
    s = 0
    for p in pids:
        try:
            with open(f"/proc/{p}/stat") as f:
                v = f.read().rsplit(")", 1)[1].split()
            s += int(v[11]) + int(v[12]) + int(v[13]) + int(v[14])
        except OSError:
            pass
    return s


def pss_kb(pids):
    s = 0
    for p in pids:
        try:
            with open(f"/proc/{p}/smaps_rollup") as f:
                for line in f:
                    if line.startswith("Pss:"):
                        s += int(line.split()[1])
        except OSError:
            pass
    return s


def run_once(app, folder, root, quiet_ms, deadline_s, log):
    env = dict(os.environ, WAYLAND_DEBUG="1", ANDROMEDA_LOG_LEVEL="debug")
    t0 = time.monotonic()
    proc = subprocess.Popen(APPS[app](root, folder), env=env, stdout=subprocess.DEVNULL,
                            stderr=subprocess.PIPE, start_new_session=True)
    marks = {}
    logf = open(log, "w")

    def reader():
        for raw in proc.stderr:
            now = time.monotonic()
            line = raw.decode(errors="replace")
            if "wl_surface" in line and ".attach(" in line and "first_buffer" not in marks:
                marks["first_buffer"] = now
            if "queue}" not in line.lower():
                logf.write(f"{(now - t0) * 1000:9.1f} {line}")
                m = re.search(r"(first frame at|listed \d+ in|statted \d+ in) ([\d.]+) ?ms", line)
                if m:
                    marks.setdefault(m.group(1).split()[0], float(m.group(2)))

    th = threading.Thread(target=reader, daemon=True)
    th.start()
    last_cpu, last_change, best_pss = -1, t0, 0
    while time.monotonic() - t0 < deadline_s and proc.poll() is None:
        pids = tree(proc.pid)
        c = cpu_ticks(pids)
        now = time.monotonic()
        if c != last_cpu:
            last_cpu, last_change = c, now
        best_pss = max(best_pss, pss_kb(pids))
        if "first_buffer" in marks and now - last_change >= quiet_ms / 1000:
            break
        time.sleep(0.02)
    pids = tree(proc.pid)
    res = {
        "first_buffer_ms": round((marks["first_buffer"] - t0) * 1000) if "first_buffer" in marks else None,
        "settled_ms": round((last_change - t0) * 1000),
        "cpu_s": round(cpu_ticks(pids) / TICK, 2),
        "pss_mib": round(pss_kb(pids) / 1024, 1),
        "peak_pss_mib": round(best_pss / 1024, 1),
        "app_log": {k: v for k, v in marks.items() if k != "first_buffer"},
        "exited": proc.poll(),
    }
    try:
        os.killpg(proc.pid, signal.SIGTERM)
        proc.wait(3)
    except (ProcessLookupError, subprocess.TimeoutExpired):
        os.killpg(proc.pid, signal.SIGKILL)
    logf.close()
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("app", choices=APPS)
    ap.add_argument("folder")
    ap.add_argument("--runs", type=int, default=3)
    ap.add_argument("--quiet-ms", type=int, default=500)
    ap.add_argument("--deadline", type=float, default=60)
    ap.add_argument("--out", default="/tmp/files-bench")
    a = ap.parse_args()
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    os.makedirs(a.out, exist_ok=True)
    rows = []
    for i in range(a.runs):
        r = run_once(a.app, a.folder, root, a.quiet_ms, a.deadline, f"{a.out}/{a.app}-{i}.log")
        print(json.dumps(r), flush=True)
        rows.append(r)
        time.sleep(1)
    med = {k: statistics.median([r[k] for r in rows if r[k] is not None] or [0])
           for k in ("first_buffer_ms", "settled_ms", "cpu_s", "pss_mib", "peak_pss_mib")}
    print("median", a.app, json.dumps(med))


if __name__ == "__main__":
    main()
