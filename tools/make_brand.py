"""Builds every brand asset from the two source artworks (requires Pillow).

Sources (assets/brand/):
  imago-icon.png     square app icon: the ghost browsing floating frames
  imago-key-art.png  16:9 key art with the IMAGO wordmark

Outputs:
  sce_sys/icon0.png                      home-screen bubble (128x128)
  sce_sys/livearea/contents/bg.png       LiveArea background (840x500)
  sce_sys/livearea/contents/startup.png  LiveArea start gate (280x158)
  assets/logo.png                        in-app header logo
  assets/splash.jpg                      in-app opening splash (960x544)

The Vita only accepts 8-bit palettised PNGs for the sce_sys images, so those
are quantised before saving.

Usage: python tools/make_brand.py
"""
from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent.parent
BRAND = ROOT / "assets" / "brand"
ICON = BRAND / "imago-icon.png"
KEY_ART = BRAND / "imago-key-art.png"

# The ghost and its ring of frames all sit within this circle of the icon art,
# well inside the rounded square (whose faint outline we leave out).
ICON_CENTER = (627, 627)
ICON_RADIUS = 520
# A tighter crop on the ghost alone, which still reads at header size.
LOGO_CENTER = (660, 560)
LOGO_RADIUS = 280

# Where the LiveArea draws the start gate over the 840x500 background in the
# "a5" style (template.xml), which puts the gate on the right. The background
# sits at (60, 44) on screen and the gate at (580, 149), so the gate image is
# cut from the background at the same spot and blends into it.
GATE_BOX = (520, 105, 800, 263)


def save_indexed(img, path):
    """Saves img as an 8-bit palette PNG and returns the palettised image."""
    path.parent.mkdir(parents=True, exist_ok=True)
    if img.mode != "P":
        img = img.convert("RGB").quantize(colors=256, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.FLOYDSTEINBERG)
    img.save(path, optimize=True)
    print("wrote", path.relative_to(ROOT))
    return img


def cover(img, size):
    """Scales and centre-crops img to fill size exactly."""
    w, h = size
    scale = max(w / img.width, h / img.height)
    resized = img.resize((round(img.width * scale), round(img.height * scale)), Image.LANCZOS)
    x, y = (resized.width - w) // 2, (resized.height - h) // 2
    return resized.crop((x, y, x + w, y + h))


def icon_crop(icon):
    cx, cy = ICON_CENTER
    r = ICON_RADIUS
    return icon.crop((cx - r, cy - r, cx + r, cy + r))


def main():
    icon = Image.open(ICON).convert("RGB")
    art = Image.open(KEY_ART).convert("RGB")
    ghost = icon_crop(icon)

    # Bubble: the home screen crops icons to a circle, so the ghost fills it.
    save_indexed(ghost.resize((128, 128), Image.LANCZOS), ROOT / "sce_sys" / "icon0.png")

    # LiveArea: the key art behind, and the start gate cut from it.
    # Cutting the palettised background keeps the gate pixel-identical to it.
    bg = save_indexed(cover(art, (840, 500)), ROOT / "sce_sys" / "livearea" / "contents" / "bg.png")
    save_indexed(bg.crop(GATE_BOX), ROOT / "sce_sys" / "livearea" / "contents" / "startup.png")

    # In-app header logo: the ghost close up, with rounded corners baked in.
    lx, ly = LOGO_CENTER
    logo = icon.crop((lx - LOGO_RADIUS, ly - LOGO_RADIUS, lx + LOGO_RADIUS, ly + LOGO_RADIUS))
    logo = logo.resize((96, 96), Image.LANCZOS).convert("RGBA")
    mask = Image.new("L", (384, 384), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, 383, 383), radius=96, fill=255)
    logo.putalpha(mask.resize((96, 96), Image.LANCZOS))
    logo.save(ROOT / "assets" / "logo.png", optimize=True)
    print("wrote assets/logo.png")

    # Opening splash, shown full screen while the first themes load.
    cover(art, (960, 544)).save(ROOT / "assets" / "splash.jpg", quality=88, optimize=True, progressive=False)
    print("wrote assets/splash.jpg")


if __name__ == "__main__":
    main()
