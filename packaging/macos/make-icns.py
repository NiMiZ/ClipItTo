#!/usr/bin/env python3
"""Draws ClipItTo's macOS icon and writes ClipItTo.icns (PNG entries, 16 px to 1024 px).

The artwork is the one src/ui/AppIcon.cpp draws (on a 32x32 grid), placed on the rounded
square macOS expects, in the 824 px safe area of a 1024 px canvas.

    python3 packaging/macos/make-icns.py [OUTPUT.icns]      needs Pillow
"""
import io
import struct
import sys

from PIL import Image, ImageDraw

SS = 4  # draw at 4x, scale down: smooth edges


def artwork(size):
    """The icon at `size` px: a light rounded square with the clipboard on it."""
    big = size * SS
    img = Image.new("RGBA", (big, big), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    # macOS Big Sur+ icons: the rounded square fills 824/1024 of the canvas.
    margin = big * 100 // 1024
    d.rounded_rectangle([margin, margin, big - margin, big - margin], radius=big * 185 // 1024, fill=(226, 234, 247, 255))
    # The clipboard on a 32x32 grid, drawn in the middle 60% of the square.
    side = (big - 2 * margin) * 0.62
    ox = (big - side) / 2
    oy = (big - side) / 2 + side * 0.02
    k = side / 32.0

    def box(x, y, w, h, r, fill):
        d.rounded_rectangle([ox + x * k, oy + y * k, ox + (x + w) * k, oy + (y + h) * k], radius=r * k, fill=fill)

    box(5, 4, 22, 26, 4, (61, 126, 214, 255))      # board
    box(8, 8, 16, 19, 2, (247, 249, 252, 255))     # paper
    box(11, 2, 10, 6, 2, (43, 47, 54, 255))        # clip
    for i in range(3):                             # lines of text
        box(11, 12 + i * 4.5, 7 if i == 2 else 10, 2, 1, (154, 165, 181, 255))
    return img.resize((size, size), Image.LANCZOS)


def png(size):
    out = io.BytesIO()
    artwork(size).save(out, "PNG")
    return out.getvalue()


# ICNS type -> pixel size (PNG data; the @2x types are the doubled size).
TYPES = [("icp4", 16), ("icp5", 32), ("icp6", 64), ("ic07", 128), ("ic08", 256), ("ic09", 512), ("ic10", 1024),
         ("ic11", 32), ("ic12", 64), ("ic13", 256), ("ic14", 512)]


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "ClipItTo.icns"
    cache = {}
    body = b""
    for code, size in TYPES:
        data = cache.setdefault(size, png(size))
        body += code.encode("ascii") + struct.pack(">I", 8 + len(data)) + data
    with open(out, "wb") as f:
        f.write(b"icns" + struct.pack(">I", 8 + len(body)) + body)
    print(f"wrote {out}: {len(body) + 8} bytes, sizes {sorted(cache)}")


if __name__ == "__main__":
    main()
