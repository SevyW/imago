"""Builds docs/images/hero.svg: a photo of a PS Vita whose screen cycles
through real Imago screenshots (requires Pillow).

Usage: python tools/make_hero.py
Slides are read from docs/images/*.png, so run it after updating screenshots.

Photo: "PlayStation-Vita-1101-FL" by Evan-Amos, public domain
(https://commons.wikimedia.org/wiki/File:PlayStation-Vita-1101-FL.png),
stored downsized in tools/assets/.
"""
import base64
import io
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
IMAGES = ROOT / "docs" / "images"
PHOTO = ROOT / "tools" / "assets" / "ps-vita-evan-amos.webp"
SLIDES = ["discover.png", "installing.png", "library.png", "apps.png", "app_detail.png", "online_icons.png"]
HOLD = 3.2  # seconds each slide stays on screen
FADE = 0.5
RES = 1.6  # raster density relative to the SVG's coordinate space (sharp on hi-DPI)

W, H = 960, 620
PHOTO_X, PHOTO_Y, PHOTO_W = 75, 78, 810
# Screen corners (TL, TR, BR, BL) as fractions of the photo, measured on the
# 5310x3240 original by flood-filling the screen glass.
SCREEN = [(1440 / 5310, 346 / 3240), (4366 / 5310, 994 / 3240), (3836 / 5310, 2668 / 3240),
          (890 / 5310, 1974 / 3240)]


def to_uri(img, fmt, **opts):
    buf = io.BytesIO()
    img.save(buf, fmt, **opts)
    mime = {"WEBP": "image/webp", "PNG": "image/png"}.get(fmt, "image/jpeg")
    return f"data:{mime};base64," + base64.b64encode(buf.getvalue()).decode()


def solve(a, b):
    """Gaussian elimination for the 8x8 perspective system."""
    n = len(b)
    m = [row[:] + [b[i]] for i, row in enumerate(a)]
    for col in range(n):
        pivot = max(range(col, n), key=lambda r: abs(m[r][col]))
        m[col], m[pivot] = m[pivot], m[col]
        for r in range(n):
            if r != col:
                f = m[r][col] / m[col][col]
                m[r] = [x - f * y for x, y in zip(m[r], m[col])]
    return [m[i][n] / m[i][i] for i in range(n)]


def perspective_coeffs(dst, src):
    """Coefficients mapping output points (dst) back to input points (src),
    as PIL's Image.PERSPECTIVE transform expects."""
    a, b = [], []
    for (x, y), (u, v) in zip(dst, src):
        a.append([x, y, 1, 0, 0, 0, -u * x, -u * y])
        a.append([0, 0, 0, x, y, 1, -v * x, -v * y])
        b += [u, v]
    return solve(a, b)


def main():
    photo = Image.open(PHOTO).convert("RGBA")
    scale = PHOTO_W / photo.width
    photo_h = photo.height * scale
    quad = [(PHOTO_X + fx * photo.width * scale, PHOTO_Y + fy * photo.height * scale) for fx, fy in SCREEN]
    x0, y0 = min(p[0] for p in quad), min(p[1] for p in quad)
    x1, y1 = max(p[0] for p in quad), max(p[1] for p in quad)
    box_w, box_h = round((x1 - x0) * RES), round((y1 - y0) * RES)
    local = [((x - x0) * RES, (y - y0) * RES) for x, y in quad]

    photo_uri = to_uri(photo.resize((round(PHOTO_W * RES), round(photo_h * RES)), Image.LANCZOS), "WEBP",
                       quality=88, method=6)

    logo_uri = to_uri(Image.open(ROOT / "assets" / "logo.png").convert("RGBA"), "PNG", optimize=True)

    slides = []
    for name in SLIDES:
        if not (IMAGES / name).exists():
            continue
        shot = Image.open(IMAGES / name).convert("RGB")
        src = [(0, 0), (shot.width, 0), (shot.width, shot.height), (0, shot.height)]
        warped = shot.transform((box_w, box_h), Image.PERSPECTIVE, perspective_coeffs(local, src), Image.BICUBIC)
        slides.append(to_uri(warped, "JPEG", quality=84, optimize=True, progressive=True))

    count = len(slides)
    total = HOLD * count
    fade, show = FADE / total * 100, HOLD / total * 100
    points = " ".join(f"{x:.1f},{y:.1f}" for x, y in quad)
    images = "\n      ".join(
        f'<image class="slide s{i}" href="{uri}" x="{x0:.1f}" y="{y0:.1f}" width="{x1 - x0:.1f}" '
        f'height="{y1 - y0:.1f}" preserveAspectRatio="none"/>' for i, uri in enumerate(slides))
    delays = "\n    ".join(f".s{i} {{ animation-delay: {i * HOLD - FADE:.1f}s; }}" for i in range(count))

    svg = f'''<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {H}" width="{W}" height="{H}" role="img"
     aria-label="A PS Vita running Imago: the theme grid, a theme being installed, a filter by author and the library">
  <defs>
    <radialGradient id="bg" cx="50%" cy="45%" r="75%">
      <stop offset="0" stop-color="#2a1517"/>
      <stop offset="1" stop-color="#0a0908"/>
    </radialGradient>
    <linearGradient id="glare" x1="0" y1="0" x2="1" y2="0">
      <stop offset="0" stop-color="#fff" stop-opacity="0"/>
      <stop offset="0.5" stop-color="#fff" stop-opacity="0.16"/>
      <stop offset="1" stop-color="#fff" stop-opacity="0"/>
    </linearGradient>
    <filter id="blur" x="-50%" y="-50%" width="200%" height="200%"><feGaussianBlur stdDeviation="60"/></filter>
    <filter id="shadow" x="-10%" y="-10%" width="120%" height="130%">
      <feDropShadow dx="0" dy="22" stdDeviation="24" flood-color="#000" flood-opacity="0.65"/>
    </filter>
    <clipPath id="logo"><rect width="44" height="44" rx="12"/></clipPath>
    <clipPath id="screen"><polygon points="{points}"/></clipPath>
  </defs>
  <style>
    @keyframes cycle {{ 0% {{ opacity: 0 }} {fade:.2f}% {{ opacity: 1 }} {show:.2f}% {{ opacity: 1 }} {show + fade:.2f}% {{ opacity: 0 }} 100% {{ opacity: 0 }} }}
    .slide {{ opacity: 0; animation: cycle {total:.1f}s infinite; }}
    {delays}
    @keyframes pulse {{ 0%, 100% {{ opacity: 0.5 }} 50% {{ opacity: 0.9 }} }}
    @keyframes sweep {{ 0% {{ transform: translateX(-380px) }} 60%, 100% {{ transform: translateX(760px) }} }}
    @keyframes float {{ 0%, 100% {{ transform: translateY(0) }} 50% {{ transform: translateY(-6px) }} }}
    .glow {{ animation: pulse 6s ease-in-out infinite; }}
    .glow2 {{ animation: pulse 6s ease-in-out -3s infinite; }}
    .glare {{ animation: sweep 7s ease-in-out infinite; }}
    .device {{ animation: float 6s ease-in-out infinite; }}
    text {{ font-family: Inter, "Segoe UI", -apple-system, "Helvetica Neue", Arial, sans-serif; }}
  </style>

  <rect width="{W}" height="{H}" rx="22" fill="url(#bg)"/>
  <circle class="glow" cx="330" cy="330" r="200" fill="#c2292f" filter="url(#blur)" opacity="0.55"/>
  <circle class="glow2" cx="660" cy="320" r="180" fill="#dfd1bd" filter="url(#blur)" opacity="0.22"/>

  <g transform="translate({W / 2 - 150} 26)">
    <image href="{logo_uri}" width="44" height="44" clip-path="url(#logo)"/>
    <g font-size="30" font-weight="500" fill="#f6eede" text-anchor="middle">
      <text x="72" y="34">I</text><text x="104" y="34">M</text><text x="172" y="34">G</text><text x="206" y="34">O</text>
    </g>
    <polygon points="127,33 138,12 149,33" fill="none" stroke="#d62e36" stroke-width="3" stroke-linejoin="miter"/>
    <text x="236" y="33" font-size="15" fill="#988879">for PS Vita</text>
  </g>

  <g class="device">
    <image href="{photo_uri}" x="{PHOTO_X}" y="{PHOTO_Y}" width="{PHOTO_W}" height="{photo_h:.1f}" filter="url(#shadow)"/>
    <g clip-path="url(#screen)">
      <polygon points="{points}" fill="#090807"/>
      {images}
      <rect class="glare" x="{x0:.1f}" y="{y0 - 20:.1f}" width="160" height="{y1 - y0 + 40:.1f}" fill="url(#glare)"/>
    </g>
  </g>
</svg>
'''
    out = IMAGES / "hero.svg"
    out.write_text(svg, encoding="utf-8", newline="\n")
    print(f"wrote {out.relative_to(ROOT)} ({out.stat().st_size / 1024:.0f} KB, {count} slides)")


if __name__ == "__main__":
    main()
