#!/usr/bin/env python3
"""Writes ClipItTo.ico for Windows: the same artwork as the macOS icon (packaging/macos/make-icns.py), on a rounded square
that fills most of the canvas (Windows draws no mask of its own), at 16 to 256 px.

    python3 packaging/windows/make-ico.py [OUTPUT.ico]      needs Pillow
"""
import importlib.util
import pathlib
import sys

here = pathlib.Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("make_icns", here.parent / "macos" / "make-icns.py")
icns = importlib.util.module_from_spec(spec)
spec.loader.exec_module(icns)

SIZES = [16, 24, 32, 48, 64, 128, 256]


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else str(here / "ClipItTo.ico")
    images = [icns.artwork(s, margin_frac=0.03) for s in SIZES]
    # Pillow writes every size from the largest image; hand it each one so every size is drawn, not scaled.
    images[-1].save(out, format="ICO", sizes=[(s, s) for s in SIZES], append_images=images[:-1])
    print(f"wrote {out}: sizes {SIZES}")


if __name__ == "__main__":
    main()
