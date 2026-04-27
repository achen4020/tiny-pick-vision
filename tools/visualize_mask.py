#!/usr/bin/env python3
"""
Render a .mask file (LSB-first packed bitmap, 640*480/8 = 38400 bytes)
into a PNG where set bits are white and clear bits are black.

Usage: python3 tools/visualize_mask.py <mask_file> [-o out.png]
Default output: same path with .mask -> .png

No external dependencies beyond Pillow (standard install).
"""
import sys
from pathlib import Path

try:
    from PIL import Image
except ImportError:
    sys.exit("install Pillow:  pip install pillow")

WIDTH = 640
HEIGHT = 480
EXPECTED = WIDTH * HEIGHT // 8

def decode_mask(buf: bytes) -> bytes:
    if len(buf) != EXPECTED:
        raise ValueError(f"mask size {len(buf)} != {EXPECTED}")
    out = bytearray(WIDTH * HEIGHT)
    for i in range(WIDTH * HEIGHT):
        byte = buf[i >> 3]
        bit = (byte >> (i & 7)) & 1
        out[i] = 255 if bit else 0
    return bytes(out)

def main():
    if len(sys.argv) < 2:
        sys.exit("usage: visualize_mask.py <mask_file> [-o out.png]")
    inp = sys.argv[1]
    out = None
    i = 2
    while i < len(sys.argv):
        if sys.argv[i] == "-o" and i + 1 < len(sys.argv):
            out = sys.argv[i+1] ; i += 2
        else:
            sys.exit(f"unknown arg: {sys.argv[i]}")
    if out is None:
        out = str(Path(inp).with_suffix(".png"))

    with open(inp, "rb") as f:
        raw = f.read()
    pixels = decode_mask(raw)
    img = Image.frombytes("L", (WIDTH, HEIGHT), pixels)
    img.save(out)
    n_fg = pixels.count(255)
    print(f"{inp} -> {out}  ({n_fg} foreground px, {n_fg * 100.0 / (WIDTH * HEIGHT):.2f}%)")

if __name__ == "__main__":
    main()
