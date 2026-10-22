"""Sets up Vita3K for testing Pictura without a console.

Vita3K has no ur0:appmeta and its app.db has no bubble titles, so this makes
two fake apps (a homebrew with layout a5 and a retail-style one with a1), adds
a tbl_appinfo_icon table with their titles, and drops a picture into
ux0:data/Pictura/import. Then unpack a VPK into ux0/app/PICT00001 and run:

    Vita3K.exe --frontend sdl -r PICT00001

Usage: python tools/mock_vita3k.py [vita3k data dir] [picture]
Needs Pillow. Back up ur0/shell/db/app.db first if it matters to you.
"""
import os
import shutil
import sqlite3
import sys

from PIL import Image, ImageDraw

DEFAULT_DATA = os.path.join(os.environ.get("APPDATA", ""), "Vita3K", "Vita3K")

TEMPLATE = """<?xml version="1.0" encoding="utf-8"?>
<livearea style="{style}" format-ver="01.00" content-rev="1">
  <livearea-background>
    <image>{bg}</image>
  </livearea-background>
  <gate>
    <startup-image>startup.png</startup-image>
  </gate>
</livearea>
"""


def save_indexed(image, path):
    image.convert("P", palette=Image.ADAPTIVE, colors=256).save(path)


def make_app(data, title_id, style, bg_name, colors):
    contents = os.path.join(data, "ur0", "appmeta", title_id, "livearea", "contents")
    os.makedirs(contents, exist_ok=True)
    top, bottom, accent = colors

    icon = Image.new("RGB", (128, 128), top)
    ImageDraw.Draw(icon).ellipse((20, 20, 108, 108), fill=accent)
    save_indexed(icon, os.path.join(data, "ur0", "appmeta", title_id, "icon0.png"))

    bg = Image.new("RGB", (840, 500))
    draw = ImageDraw.Draw(bg)
    for y in range(500):
        t = y / 499
        draw.line((0, y, 840, y), fill=tuple(int(a + (b - a) * t) for a, b in zip(top, bottom)))
    save_indexed(bg, os.path.join(contents, bg_name))
    save_indexed(Image.new("RGB", (280, 158), accent), os.path.join(contents, "startup.png"))
    with open(os.path.join(contents, "template.xml"), "w", encoding="utf-8") as f:
        f.write(TEMPLATE.format(style=style, bg=bg_name))


def main():
    data = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_DATA
    picture = sys.argv[2] if len(sys.argv) > 2 else None
    make_app(data, "PCSB00245", "a1", "bg0.png", ((10, 30, 120), (160, 40, 210), (250, 200, 40)))
    make_app(data, "PCSE00001", "a5", "bg.png", ((20, 90, 60), (200, 230, 160), (220, 60, 60)))

    db_path = os.path.join(data, "ur0", "shell", "db", "app.db")
    os.makedirs(os.path.dirname(db_path), exist_ok=True)
    db = sqlite3.connect(db_path)
    db.execute("CREATE TABLE IF NOT EXISTS tbl_appinfo_icon (pageId INTEGER, pos INTEGER, iconPath TEXT,"
               " title TEXT, type INTEGER, titleId TEXT)")
    db.execute("DELETE FROM tbl_appinfo_icon WHERE titleId IN ('PCSB00245', 'PCSE00001')")
    db.executemany("INSERT INTO tbl_appinfo_icon (titleId, title) VALUES (?, ?)",
                   [("PCSB00245", "Gravity\nRush"), ("PCSE00001", "Sample Game")])
    db.commit()
    db.close()

    if picture:
        imports = os.path.join(data, "ux0", "data", "Pictura", "import")
        os.makedirs(imports, exist_ok=True)
        shutil.copy(picture, imports)
    print("Vita3K mock ready in", data)


if __name__ == "__main__":
    main()
