#!/usr/bin/env python3
"""Writes the logo PNGs of the MSIX package (packaging/windows/msix/Assets), from the same artwork as the .ico and .icns.
The PNGs are committed; run this only when the artwork changes.

    python3 packaging/windows/msix/make-assets.py [OUTPUT_DIR]      needs Pillow
"""
import importlib.util
import pathlib
import sys

from PIL import Image

here = pathlib.Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("make_icns", here.parent.parent / "macos" / "make-icns.py")
icns = importlib.util.module_from_spec(spec)
spec.loader.exec_module(icns)

# file name -> edge in px (the artwork is a rounded plate; tiles leave a margin, the small icons fill their square)
SQUARE = {
    "StoreLogo.png": (50, 1.0),
    "Square44x44Logo.png": (44, 1.0),
    "Square71x71Logo.png": (71, 0.8),
    "Square150x150Logo.png": (150, 0.8),
    "Square310x310Logo.png": (310, 0.8),
}
# Taskbar / Start list / title bar sizes: the shell picks the one it needs. "altform-unplated" = used without a coloured plate.
TARGET_SIZES = [16, 24, 32, 48, 256]


def tile(width, height, fill):
    """The icon, `fill` of the shorter side, centred on a transparent canvas."""
    side = round(min(width, height) * fill)
    art = icns.artwork(max(side, 16), margin_frac=0.03).resize((side, side), Image.LANCZOS)
    canvas = Image.new("RGBA", (width, height), (0, 0, 0, 0))
    canvas.paste(art, ((width - side) // 2, (height - side) // 2), art)
    return canvas


def main():
    out = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else here / "Assets"
    out.mkdir(parents=True, exist_ok=True)
    for name, (size, fill) in SQUARE.items():
        tile(size, size, fill).save(out / name)
    tile(310, 150, 0.8).save(out / "Wide310x150Logo.png")
    for size in TARGET_SIZES:
        img = tile(size, size, 1.0)
        img.save(out / f"Square44x44Logo.targetsize-{size}.png")
        img.save(out / f"Square44x44Logo.targetsize-{size}_altform-unplated.png")
    print(f"wrote {len(list(out.glob('*.png')))} files to {out}")


if __name__ == "__main__":
    main()
