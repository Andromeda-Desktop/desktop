#!/usr/bin/env python3
"""Packs an SVG app icon into a macOS .icns at every size. Usage: icns.py ICON.svg OUT.icns
The macOS tile is 824 of 1024 px; Andromeda app SVGs draw their tile at 120 of 128, so the art renders at 86%."""
import struct
import subprocess
import sys
from pathlib import Path

TYPES = [("icp4", 16), ("icp5", 32), ("ic11", 32), ("ic12", 64), ("ic07", 128), ("ic08", 256), ("ic13", 256),
         ("ic09", 512), ("ic14", 512), ("ic10", 1024)]


def png(svg, size):
    inner = round(size * 0.86)
    margin = (size - inner) // 2
    return subprocess.run(["rsvg-convert", "-w", str(inner), "-h", str(inner), "--page-width", str(size),
                           "--page-height", str(size), "--left", str(margin), "--top", str(margin), svg],
                          check=True, capture_output=True).stdout


svg, out = sys.argv[1], sys.argv[2]
chunks = b"".join(kind.encode() + struct.pack(">I", len(data) + 8) + data
                  for kind, size in TYPES for data in [png(svg, size)])
Path(out).write_bytes(b"icns" + struct.pack(">I", len(chunks) + 8) + chunks)
