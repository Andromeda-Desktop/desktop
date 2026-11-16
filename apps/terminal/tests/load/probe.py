#!/usr/bin/env python3
"""Load probe that runs inside the terminal under test.

gen DIR MIB             write the corpora to DIR (outside the terminal, once)
run FILE RESULT         write FILE to the tty, then wait for the DSR reply; RESULT gets "ns ok|timeout"
dsr RESULT              one DSR round trip on an idle terminal
keys RESULT [FLOOD]     raw mode; log "char ns" per key until '~'; with FLOOD, a thread loops FLOOD to the tty
                        and sends a DSR every 200 ms, logged as "dsr ns"
stream KIND MIB         generate a chaos stream on the fly (single_line) to stdout
All times are CLOCK_REALTIME ns, the clock of `date +%s%N` in the harness.
"""
import base64
import os
import random
import re
import select
import sys
import termios
import threading
import time
import tty
import zlib

CHUNK = 65536
DSR = b"\x1b[6n"
DSR_REPLY = re.compile(rb"\x1b\[(\d+);(\d+)R")


def put(path, data):
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        f.write(data)
    os.rename(tmp, path)


def write_all(fd, data):
    view = memoryview(data)
    while view:
        n = os.write(fd, view[:CHUNK])
        view = view[n:]


def wait_dsr(fd, timeout):
    buf = b""
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        r, _, _ = select.select([fd], [], [], max(0.0, end - time.monotonic()))
        if r:
            buf += os.read(fd, 4096)
            if DSR_REPLY.search(buf):
                return True
    return False


class Raw:
    def __enter__(self):
        self.fd = sys.stdin.fileno()
        self.old = termios.tcgetattr(self.fd)
        tty.setraw(self.fd)
        termios.tcflush(self.fd, termios.TCIFLUSH)
        return self.fd

    def __exit__(self, *_):
        # Replies to queries in the stream must not reach the shell as typed commands.
        termios.tcflush(self.fd, termios.TCIFLUSH)
        termios.tcsetattr(self.fd, termios.TCSADRAIN, self.old)


def words(rng, n):
    alpha = "abcdefghijklmnopqrstuvwxyz0123456789"
    return [
        "".join(rng.choice(alpha) for _ in range(rng.randint(1, 10))) for _ in range(n)
    ]


def gen_block(kind, rng):
    """About 1 MiB of one corpus kind; the corpus repeats blocks to its size."""
    out = []
    size = 0
    if kind == "dense_cells":
        out.append("\x1b[H")
    while size < 1 << 20:
        if kind == "ascii":
            line = " ".join(words(rng, rng.randint(10, 30))) + "\n"
        elif kind == "sgr":
            parts = []
            for w in words(rng, rng.randint(8, 20)):
                fg = ";".join(str(rng.randrange(256)) for _ in range(3))
                bg = ";".join(str(rng.randrange(256)) for _ in range(3))
                attr = rng.choice(["1", "3", "4", "4:3", "7", "9", "2", "0"])
                parts.append(f"\x1b[{attr};38;2;{fg};48;2;{bg}m{w}")
            line = " ".join(parts) + "\x1b[0m\n"
        elif kind == "unicode":
            pool = [
                "漢字",
                "かな",
                "한국어",
                "é",
                "ạ̈",
                "👍",
                "👩‍👩‍👧",
                "🇩🇪",
                "🏳️‍🌈",
                "ﷺ",
                "∑∫√",
                "│─┼",
                "⣿⡇",
                "",
                "ab",
            ]
            line = "".join(rng.choice(pool) for _ in range(rng.randint(10, 40))) + "\n"
        elif kind == "scroll_region":
            line = "\x1b[5;20r\x1b[20;1H" + " ".join(words(rng, 12)) + "\n\x1b[r"
        elif kind == "cursor":
            line = "".join(
                f"\x1b[{rng.randint(1, 50)};{rng.randint(1, 150)}H{rng.choice(words(rng, 3))}"
                for _ in range(20)
            )
        elif kind == "long_lines":
            line = "x" * rng.randint(5000, 20000) + "\n"
        elif kind == "alt_screen":
            line = (
                "\x1b[?1049h\x1b[2J"
                + "\n".join(" ".join(words(rng, 10)) for _ in range(50))
                + "\x1b[?1049l\n"
            )
        elif kind == "dense_cells":
            line = "".join(
                f"\x1b[38;5;{rng.randrange(256)};48;5;{rng.randrange(256)}m{chr(rng.randint(33, 126))}"
                for _ in range(150)
            )
            line += "\r\n" if rng.random() > 0.02 else "\x1b[H"
        elif kind == "vtfuzz":
            line = fuzz_seq(rng)
        else:
            raise SystemExit(f"unknown corpus {kind}")
        out.append(line)
        size += len(line.encode("utf-8", "surrogatepass"))
    return "".join(out).encode("utf-8", "surrogatepass")


def fuzz_seq(rng):
    """One random escape sequence: valid shapes with hostile parameters, and broken ones."""
    p = lambda: str(rng.choice([0, 1, 2, 7, 255, 65535, 2**31, 2**63, 10**30]))
    pick = rng.randrange(12)
    if pick == 0:
        return "\x1b[" + ";".join(p() for _ in range(rng.randint(0, 40))) + chr(rng.randint(0x40, 0x7E))
    if pick == 1:
        return "\x1b[?" + ";".join(p() for _ in range(rng.randint(1, 8))) + rng.choice("hlrs$p")
    if pick == 2:
        return f"\x1b]{rng.choice([0, 2, 4, 7, 8, 10, 11, 12, 22, 52, 104, 133, 777, 9, 99, 1337, 66])};" + "".join(
            chr(rng.randint(32, 0x2FFF)) for _ in range(rng.randint(0, 4000))) + rng.choice(["\x07", "\x1b\\", ""])
    if pick == 3:
        return "\x1bP" + "".join(chr(rng.randint(32, 126)) for _ in range(rng.randint(0, 2000))) + "\x1b\\"
    if pick == 4:
        return "\x1b_G" + "".join(chr(rng.randint(32, 126)) for _ in range(rng.randint(0, 2000))) + "\x1b\\"
    if pick == 5:
        return "".join(chr(rng.randint(0, 31)) for _ in range(rng.randint(1, 20)))
    if pick == 6:
        return "\x1b" + chr(rng.randint(32, 126))
    if pick == 7:
        return "\x1b[" + rng.choice(["38;2", "48;2", "58;2", "38:2::", "4:", "38;5"]) + ";" + p() + "m"
    if pick == 8:
        return bytes(rng.randrange(256) for _ in range(rng.randint(1, 64))).decode("latin-1")
    if pick == 9:
        return rng.choice(["\x1b[?1049h", "\x1b[?1049l", "\x1b[?2026h", "\x1b[?2026l", "\x1bc", "\x1b[!p", "\x1b#8"])
    if pick == 10:
        return "\x1b[" + p() + ";" + p() + "H" + "W" * rng.randint(0, 10)
    return " ".join(words(rng, 5)) + "\n"


def gen_chaos(kind, rng):
    if kind == "osc52":
        return b"\x1b]52;c;" + base64.b64encode(os.urandom(8 << 20)) + b"\x07\n"
    if kind == "titles":
        return "".join(f"\x1b]2;title {i} {'x' * (i % 300)}\x07" for i in range(100000)).encode()
    if kind == "links":
        return "".join(f"\x1b]8;id={i};https://example.com/{i}\x1b\\link {i}\x1b]8;;\x1b\\\n" for i in range(100000)).encode()
    if kind == "kitty_images":
        out = []
        for i in range(400):
            raw = zlib.compress(os.urandom(64 * 64 * 4), 0)
            data = base64.b64encode(raw)
            for off in range(0, len(data), 4096):
                more = 1 if off + 4096 < len(data) else 0
                head = f"a=T,f=32,s=64,v=64,o=z,i={i + 1}," if off == 0 else ""
                out.append(f"\x1b_G{head}m={more};".encode() + data[off:off + 4096] + b"\x1b\\")
            out.append(b"\n")
        return b"".join(out)
    if kind == "urandom":
        return os.urandom(64 << 20)
    raise SystemExit(f"unknown chaos corpus {kind}")


THROUGHPUT = ["ascii", "sgr", "unicode", "scroll_region", "cursor", "long_lines", "alt_screen", "dense_cells"]
CHAOS = ["vtfuzz", "osc52", "titles", "links", "kitty_images", "urandom"]


def gen(directory, mib):
    os.makedirs(directory, exist_ok=True)
    for kind in THROUGHPUT + CHAOS:
        path = os.path.join(directory, kind)
        if os.path.exists(path):
            continue
        rng = random.Random(kind)
        if kind in CHAOS and kind != "vtfuzz":
            data = gen_chaos(kind, rng)
        else:
            blocks = [gen_block(kind, rng) for _ in range(4)]
            data = b"".join(blocks[i % 4] for i in range(mib))
        with open(path + ".tmp", "wb") as f:
            f.write(data)
        os.rename(path + ".tmp", path)


def run(path, result):
    with open(path, "rb") as f:
        data = f.read()
    with Raw() as fd:
        t0 = time.time_ns()
        write_all(sys.stdout.fileno(), data)
        os.write(sys.stdout.fileno(), b"\x1b[0m\x1b[r\x1b[?1049l" + DSR)
        ok = wait_dsr(fd, 120)
        t1 = time.time_ns()
    os.write(sys.stdout.fileno(), b"\x1bc")
    put(result, f"{t1 - t0} {'ok' if ok else 'timeout'}\n")


def dsr(result):
    with Raw() as fd:
        t0 = time.time_ns()
        os.write(sys.stdout.fileno(), DSR)
        ok = wait_dsr(fd, 10)
        t1 = time.time_ns()
    put(result, f"{t1 - t0} {'ok' if ok else 'timeout'}\n")


def keys(result, flood):
    log = []
    stop = threading.Event()
    pending = []
    lock = threading.Lock()

    def writer():
        with open(flood, "rb") as f:
            data = f.read()
        out = sys.stdout.fileno()
        last = 0.0
        pos = 0
        while not stop.is_set():
            write_all(out, data[pos:pos + CHUNK])
            pos = (pos + CHUNK) % len(data)
            if time.monotonic() - last > 0.2:
                last = time.monotonic()
                with lock:
                    pending.append(time.time_ns())
                os.write(out, DSR)

    with Raw() as fd:
        thread = None
        if flood:
            thread = threading.Thread(target=writer, daemon=True)
            thread.start()
        buf = b""
        done = False
        while not done:
            r, _, _ = select.select([fd], [], [], 60)
            if not r:
                break
            now = time.time_ns()
            buf += os.read(fd, 4096)
            while True:
                m = DSR_REPLY.search(buf)
                if not m:
                    break
                with lock:
                    if pending:
                        log.append(f"dsr {now - pending.pop(0)}")
                buf = buf[: m.start()] + buf[m.end():]
            for b in buf:
                if b == 0x1B:
                    break
                ch = chr(b)
                if ch == "~":
                    done = True
                    break
                log.append(f"{ch} {now}")
            esc = buf.find(b"\x1b")
            buf = buf[esc:] if esc >= 0 else b""
        stop.set()
        if thread:
            thread.join()
    os.write(sys.stdout.fileno(), b"\x1b[0m\x1bc")
    put(result, "\n".join(log) + "\n")


def stream(kind, mib):
    out = sys.stdout.fileno()
    if kind == "single_line":
        block = b"y" * CHUNK
        for _ in range(mib * 16):
            write_all(out, block)
        os.write(out, b"\n")
        return
    raise SystemExit(f"unknown stream {kind}")


def main():
    cmd, args = sys.argv[1], sys.argv[2:]
    if cmd == "gen":
        gen(args[0], int(args[1]))
    elif cmd == "run":
        run(args[0], args[1])
    elif cmd == "dsr":
        dsr(args[0])
    elif cmd == "keys":
        keys(args[0], args[1] if len(args) > 1 else "")
    elif cmd == "stream":
        stream(args[0], int(args[1]))
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()
