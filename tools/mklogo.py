#!/usr/bin/env python3
"""mklogo.py — build the boot splash JPEG that U-Boot shows while the DVR starts.

The stock image is an 800x600 JPEG living in **mtd0 at offset 0x20000** (33 265 bytes;
U-Boot env: `jpeg_addr=0x80020000`, `jpeg_size=0x20000`, and `bootcmd` starts with
`showlogo`). Ours goes in the same place — see `tools/dvr.py logo`.

    py -3 tools/mklogo.py out.jpg
    py -3 tools/mklogo.py out.jpg --title "FPV GROUND STATION" \
        --subtitle "RR104P - analog recorder" --status "sistem baslatiliyor..."

Constraints that shaped the design:
  * 800x600, shown on a VGA monitor by the bootloader for ~2 s.
  * It must fit in ONE 64 KiB flash erase block together with nothing else, so the
    encoder targets <= 56 KiB (--budget). That keeps the write to a single block at
    0x20000 and never touches U-Boot's own code at 0x00000-0x1FFFF.
  * Big, high-contrast shapes only: the analog scaler softens thin strokes.
"""
from __future__ import annotations

import argparse
import math
import os
import sys

from PIL import Image, ImageDraw, ImageFilter, ImageFont

W, H = 800, 600
BG = (5, 8, 11)
ACCENT = (79, 209, 224)          # the project's cyan
DIM = (120, 140, 152)

FONT_DIRS = [r"C:\Windows\Fonts", "/usr/share/fonts/truetype/dejavu", "/Library/Fonts"]
FONT_CANDIDATES = {
    "bold": ["seguisb.ttf", "arialbd.ttf", "verdanab.ttf", "DejaVuSans-Bold.ttf"],
    "regular": ["segoeui.ttf", "arial.ttf", "verdana.ttf", "DejaVuSans.ttf"],
    "mono": ["consola.ttf", "cour.ttf", "DejaVuSansMono.ttf"],
}


def font(kind: str, size: int) -> ImageFont.FreeTypeFont:
    for name in FONT_CANDIDATES[kind]:
        for d in FONT_DIRS:
            p = os.path.join(d, name)
            if os.path.exists(p):
                return ImageFont.truetype(p, size)
    return ImageFont.load_default()


def centered(draw: ImageDraw.ImageDraw, y: int, text: str, f, fill, spacing: int = 0):
    """Draw text centred on W, optionally letter-spaced. Returns its height."""
    if not text:
        return 0
    if spacing:
        widths = [draw.textlength(c, font=f) for c in text]
        total = sum(widths) + spacing * (len(text) - 1)
        x = (W - total) / 2
        for c, cw in zip(text, widths):
            draw.text((x, y), c, font=f, fill=fill)
            x += cw + spacing
        box = f.getbbox(text)
        return box[3] - box[1]
    box = draw.textbbox((0, 0), text, font=f)
    draw.text(((W - (box[2] - box[0])) / 2 - box[0], y), text, font=f, fill=fill)
    return box[3] - box[1]


def glow_bar(img: Image.Image, y: int, height: int, colour, spread: int = 60) -> None:
    """The soft horizontal light streak the stock splash had — an easy visual anchor
    that survives an analog scaler far better than fine detail."""
    layer = Image.new("RGB", (W, H), (0, 0, 0))
    d = ImageDraw.Draw(layer)
    for i in range(height):
        t = 1.0 - abs(i - height / 2) / (height / 2)
        c = tuple(int(v * t) for v in colour)
        d.line([(60, y + i), (W - 60, y + i)], fill=c)
    layer = layer.filter(ImageFilter.GaussianBlur(spread / 4))
    img.paste(Image.blend(img, Image.blend(img, layer, 0.0), 0.0))     # keep img
    base = img.load()
    lay = layer.load()
    for yy in range(max(0, y - spread), min(H, y + height + spread)):
        for xx in range(W):
            r, g, b = base[xx, yy]
            lr, lg, lb = lay[xx, yy]
            base[xx, yy] = (min(255, r + lr), min(255, g + lg), min(255, b + lb))


def paper_plane(draw: ImageDraw.ImageDraw, cx: int, cy: int, size: int) -> None:
    """The same paper-dart silhouette as the on-screen cursor (device/dvr/ui.h),
    pointing up-right: a light upper wing, a darker folded lower wing."""
    def P(u, v):
        return (cx + (u - 0.5) * size, cy + (v - 0.5) * size * 0.78)
    tip, upper, notch, lower = P(1.00, 0.10), P(0.02, 0.52), P(0.38, 0.62), P(0.30, 0.98)
    draw.polygon([tip, upper, notch], fill=(226, 240, 244))          # top wing
    draw.polygon([tip, notch, lower], fill=(96, 122, 133))           # folded wing
    draw.line([tip, notch], fill=(255, 255, 255), width=2)           # crease


def build(title: str, subtitle: str, status: str, accent) -> Image.Image:
    img = Image.new("RGB", (W, H), BG)

    # vignette-ish glow behind the centre so the black doesn't band on cheap panels
    halo = Image.new("RGB", (W, H), (0, 0, 0))
    hd = ImageDraw.Draw(halo)
    for r in range(320, 0, -8):
        v = int(26 * (1 - r / 320) ** 2)
        hd.ellipse([W / 2 - r * 1.6, 250 - r * 0.62, W / 2 + r * 1.6, 250 + r * 0.62],
                   fill=(v // 3, v // 2, v))
    img = Image.blend(img, Image.blend(img, halo, 1.0), 0.55)
    img = img.filter(ImageFilter.GaussianBlur(1.2))

    # the streak sits directly under the plane so it reads as a reflection/horizon,
    # not as a smudge behind the title (which is where a centred bar wants to land)
    glow_bar(img, 274, 5, accent, spread=70)

    d = ImageDraw.Draw(img)
    paper_plane(d, W // 2, 190, 190)

    centered(d, 322, title, font("bold", 54), (240, 248, 250), spacing=4)
    centered(d, 396, subtitle, font("regular", 24), DIM, spacing=2)

    # thin accent rule + status line, echoing the stock splash's structure
    d.rectangle([W // 2 - 90, 452, W // 2 + 90, 454], fill=accent)
    centered(d, 480, status, font("mono", 20), accent)
    return img


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--title", default="FPV GROUND STATION")
    ap.add_argument("--subtitle", default="RR104P  \u00b7  analog video recorder")
    ap.add_argument("--status", default="sistem ba\u015flat\u0131l\u0131yor...")
    ap.add_argument("--accent", default="4fd1e0", help="hex accent colour")
    ap.add_argument("--budget", type=int, default=56 * 1024,
                    help="max JPEG bytes (one 64 KiB erase block, with headroom)")
    ap.add_argument("--png", help="also write a PNG preview here")
    a = ap.parse_args()

    accent = tuple(int(a.accent[i:i + 2], 16) for i in (0, 2, 4))
    img = build(a.title, a.subtitle, a.status, accent)
    if a.png:
        img.save(a.png)

    for q in (92, 88, 84, 78, 72, 66, 60):
        img.save(a.out, "JPEG", quality=q, optimize=True, progressive=False)
        n = os.path.getsize(a.out)
        if n <= a.budget:
            print(f"{a.out}  {W}x{H}  {n} bytes  q={q}  "
                  f"({100 * n // a.budget}% of the {a.budget}-byte budget)")
            return 0
    print(f"could not get under {a.budget} bytes", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
