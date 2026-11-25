#!/usr/bin/env python3
"""Usage: vnc-shot.py SOCKET OUT.ppm. Saves one full frame from a QEMU VNC unix socket (no auth, Raw encoding)."""
import socket
import struct
import sys


def read(s, n):
    buf = bytearray()
    while len(buf) < n:
        chunk = s.recv(n - len(buf))
        if not chunk:
            raise EOFError("VNC server closed the connection")
        buf += chunk
    return bytes(buf)


s = socket.socket(socket.AF_UNIX)
s.settimeout(10)
s.connect(sys.argv[1])
read(s, 12)
s.sendall(b"RFB 003.008\n")
types = read(s, read(s, 1)[0])
if 1 not in types:
    sys.exit(f"VNC server offers no 'None' security: {list(types)}")
s.sendall(b"\x01")
if struct.unpack(">I", read(s, 4))[0] != 0:
    sys.exit("VNC security handshake failed")
s.sendall(b"\x01")
w, h = struct.unpack(">HH", read(s, 4))
read(s, 16)
read(s, struct.unpack(">I", read(s, 4))[0])
# 32 bpp, depth 24, little endian, true colour, 8 bits per channel: red at bit 16, green 8, blue 0.
s.sendall(struct.pack(">B3xBBBBHHHBBB3x", 0, 32, 24, 0, 1, 255, 255, 255, 16, 8, 0))
s.sendall(struct.pack(">BxHi", 2, 1, 0))
s.sendall(struct.pack(">BBHHHH", 3, 0, 0, 0, w, h))
fb = bytearray(w * h * 3)
covered = 0
while covered < w * h:
    kind = read(s, 1)[0]
    if kind == 2:
        continue
    if kind == 1:
        read(s, 5)
        read(s, struct.unpack(">H", read(s, 2))[0] * 6)
        continue
    if kind == 3:
        read(s, 3)
        read(s, struct.unpack(">I", read(s, 4))[0])
        continue
    if kind != 0:
        sys.exit(f"unexpected VNC message {kind}")
    read(s, 1)
    for _ in range(struct.unpack(">H", read(s, 2))[0]):
        x, y, rw, rh, enc = struct.unpack(">HHHHi", read(s, 12))
        if enc != 0:
            # A pseudo-encoding resize: the next update carries the pixels.
            if enc == -223:
                w, h = rw, rh
                fb = bytearray(w * h * 3)
                covered = 0
                s.sendall(struct.pack(">BBHHHH", 3, 0, 0, 0, w, h))
                continue
            sys.exit(f"unexpected VNC encoding {enc}")
        px = read(s, rw * rh * 4)
        for row in range(rh):
            src = px[row * rw * 4:(row + 1) * rw * 4]
            line = bytearray(rw * 3)
            line[0::3], line[1::3], line[2::3] = src[2::4], src[1::4], src[0::4]
            off = ((y + row) * w + x) * 3
            fb[off:off + rw * 3] = line
        covered += rw * rh
with open(sys.argv[2], "wb") as f:
    f.write(b"P6\n%d %d\n255\n" % (w, h) + fb)
