"""Generates the procedural test media in scenes/media/ (checked in).

Deterministic: the same script always writes the same pixels, so golden
images don't depend on anything outside this repo.

    pip install pillow && python3 scenes/make_media.py
"""
import colorsys
import pathlib
import random

from PIL import Image, ImageDraw, ImageFilter

OUT = pathlib.Path(__file__).resolve().parent / "media"
W, H = 1920, 1080


def dusk():
    """A dusk sky gradient with soft out-of-focus lights."""
    img = Image.new("RGB", (W, H))
    top, mid, bottom = (18, 22, 58), (92, 46, 110), (238, 128, 84)
    px = img.load()
    for y in range(H):
        t = y / (H - 1)
        a, b, u = (top, mid, t / 0.6) if t < 0.6 else (mid, bottom, (t - 0.6) / 0.4)
        row = tuple(int(a[i] + (b[i] - a[i]) * u) for i in range(3))
        for x in range(W):
            px[x, y] = row
    glow = Image.new("RGB", (W, H), (0, 0, 0))
    d = ImageDraw.Draw(glow)
    rng = random.Random(7)
    for _ in range(46):
        r = rng.randint(18, 90)
        x, y = rng.randint(0, W), rng.randint(int(H * 0.35), H)
        c = rng.choice([(255, 196, 120), (255, 150, 170), (190, 170, 255)])
        d.ellipse((x - r, y - r, x + r, y + r), fill=tuple(int(v * 0.55) for v in c))
    glow = glow.filter(ImageFilter.GaussianBlur(14))
    out = Image.new("RGB", (W, H))
    a, b = img.load(), glow.load()
    o = out.load()
    for y in range(H):
        for x in range(W):
            p, q = a[x, y], b[x, y]
            o[x, y] = tuple(min(255, p[i] + q[i]) for i in range(3))  # additive glow
    out.save(OUT / "dusk.jpg", quality=88)


def spectrum():
    """Hue across, light to dark down: a backdrop that shows every blend mode."""
    img = Image.new("RGB", (W, H))
    px = img.load()
    for y in range(H):
        v = 1.0 - 0.75 * (y / (H - 1))
        for x in range(W):
            r, g, b = colorsys.hsv_to_rgb(x / W, 0.65, v)
            stripe = 0.85 if (x // 40 + y // 40) % 2 else 1.0
            px[x, y] = (int(r * 255 * stripe), int(g * 255 * stripe), int(b * 255 * stripe))
    img.save(OUT / "spectrum.jpg", quality=90)


if __name__ == "__main__":
    OUT.mkdir(exist_ok=True)
    dusk()
    spectrum()
    print("wrote", ", ".join(p.name for p in sorted(OUT.iterdir())))
