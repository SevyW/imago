"""Checks a VPK for the things that make the Vita's installer refuse it
("promoting" fails): sce_sys images must be 8-bit palette PNGs of the right
size, the LiveArea files must be present, and param.sfo must be sane.

Usage: python tools/check_vpk.py file.vpk [...]
"""
import re
import struct
import sys
import zipfile

SIZES = {
    "sce_sys/icon0.png": (128, 128),
    "sce_sys/livearea/contents/bg.png": (840, 500),
    "sce_sys/livearea/contents/startup.png": (280, 158),
}
REQUIRED = ["eboot.bin", "sce_sys/param.sfo", "sce_sys/livearea/contents/template.xml", *SIZES]


def read_sfo(data):
    keys, values, count = struct.unpack("<III", data[8:20])
    out = {}
    for i in range(count):
        key_at, fmt, length, _, value_at = struct.unpack("<HHIII", data[20 + i * 16:36 + i * 16])
        key = data[keys + key_at:data.index(b"\0", keys + key_at)].decode()
        value = data[values + value_at:values + value_at + length]
        out[key] = struct.unpack("<I", value)[0] if fmt == 0x0404 else value.rstrip(b"\0").decode()
    return out


def check(path):
    problems = []
    z = zipfile.ZipFile(path)
    names = set(z.namelist())
    for name in REQUIRED:
        if name not in names:
            problems.append(f"missing {name}")
    for name in sorted(n for n in names if n.startswith("sce_sys/") and n.endswith(".png")):
        head = z.read(name)[:26]
        if head[:8] != b"\x89PNG\r\n\x1a\n":
            problems.append(f"{name}: not a PNG")
            continue
        w, h, depth, ctype = struct.unpack(">IIBB", head[16:26])
        if depth != 8 or ctype != 3:
            problems.append(f"{name}: {depth}-bit, colour type {ctype} (needs 8-bit palette)")
        if name in SIZES and (w, h) != SIZES[name]:
            problems.append(f"{name}: {w}x{h} (needs {SIZES[name][0]}x{SIZES[name][1]})")
    if "eboot.bin" in names and z.read("eboot.bin")[:4] != b"SCE\0":
        problems.append("eboot.bin is not a SELF")
    if "sce_sys/param.sfo" in names:
        sfo = read_sfo(z.read("sce_sys/param.sfo"))
        if not re.fullmatch(r"[A-Z]{4}\d{5}", sfo.get("TITLE_ID", "")):
            problems.append(f"TITLE_ID {sfo.get('TITLE_ID')!r} is not like ABCD12345")
        if not re.fullmatch(r"\d{2}\.\d{2}", sfo.get("APP_VER", "")) or sfo.get("APP_VER", "00.00") < "01.00":
            problems.append(f"APP_VER {sfo.get('APP_VER')!r} (use 01.00 or later)")
        if not sfo.get("TITLE"):
            problems.append("TITLE is empty")
    return problems


def main():
    failed = False
    for path in sys.argv[1:]:
        problems = check(path)
        print(("OK   " if not problems else "FAIL ") + path)
        for p in problems:
            print("     -", p)
        failed |= bool(problems)
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
