#!/usr/bin/env python3
"""Writes the MSIX (Microsoft Store) image assets to resources/msix/Assets from LYNX's icon (round 165n).

Usage: scripts/make-msix-assets.py   (needs Pillow; the PNGs are committed, so a release build does not run this)

Every logo comes from resources/lynx_badge.png (the app icon: a round dark badge, so it needs no plate) and the
project-file logo from the largest image in resources/LYNX_doc.ico. Scale and target-size variants follow
Microsoft's tile/icon asset guidelines; makepri (in the release workflow) indexes them into resources.pri.
"""
import os
import sys
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "resources", "msix", "Assets")
SCALES = (100, 125, 150, 200, 400)
TARGETS = (16, 20, 24, 30, 32, 36, 40, 48, 60, 64, 72, 80, 96, 256)


def badge():
    return Image.open(os.path.join(ROOT, "resources", "lynx_badge.png")).convert("RGBA")


def doc_icon():
    ico = Image.open(os.path.join(ROOT, "resources", "LYNX_doc.ico"))
    best = max(ico.info.get("sizes", {ico.size}), key=lambda s: s[0])
    ico.size = best
    return ico.convert("RGBA")


def placed(src, width, height, fraction):
    """src scaled to `fraction` of the shorter side, centred on a transparent canvas."""
    side = max(1, round(min(width, height) * fraction))
    canvas = Image.new("RGBA", (width, height), (0, 0, 0, 0))
    icon = src.resize((side, side), Image.LANCZOS)
    canvas.alpha_composite(icon, ((width - side) // 2, (height - side) // 2))
    return canvas


def save(img, name):
    img.save(os.path.join(OUT, name), optimize=True)


def main():
    os.makedirs(OUT, exist_ok=True)
    for f in os.listdir(OUT):
        if f.endswith(".png"):
            os.remove(os.path.join(OUT, f))
    b, d = badge(), doc_icon()
    for s in SCALES:
        k = s / 100.0
        save(placed(b, round(44 * k), round(44 * k), 1.0), f"Square44x44Logo.scale-{s}.png")
        save(placed(b, round(150 * k), round(150 * k), 0.66), f"Square150x150Logo.scale-{s}.png")
        save(placed(b, round(310 * k), round(150 * k), 0.66), f"Wide310x150Logo.scale-{s}.png")
        save(placed(b, round(71 * k), round(71 * k), 0.8), f"SmallTile.scale-{s}.png")
        save(placed(b, round(310 * k), round(310 * k), 0.6), f"LargeTile.scale-{s}.png")
        save(placed(b, round(50 * k), round(50 * k), 1.0), f"StoreLogo.scale-{s}.png")
        save(placed(d, round(44 * k), round(44 * k), 1.0), f"ProjectFile.scale-{s}.png")
    for t in TARGETS:
        icon = placed(b, t, t, 1.0)
        for suffix in ("", "_altform-unplated", "_altform-lightunplated"):
            save(icon, f"Square44x44Logo.targetsize-{t}{suffix}.png")
        save(placed(d, t, t, 1.0), f"ProjectFile.targetsize-{t}.png")
    print(f"{len(os.listdir(OUT))} files in {OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
