"""Talks to a PS Vita running the vitacompanion plugin (FTP on 1337, commands on 1338).

    python tools/vita.py [--ip IP] wait                     wait until the Vita answers
    python tools/vita.py [--ip IP] cmd "launch PICT00001"   run commands (chain with ';')
    python tools/vita.py [--ip IP] ls ux0:/data/Pictura
    python tools/vita.py [--ip IP] get ux0:/data/Pictura/log.txt [local]
    python tools/vita.py [--ip IP] getdir ux0:/data/PicturaProbe local_dir
    python tools/vita.py [--ip IP] put local_file ux0:/data/x.png
    python tools/vita.py [--ip IP] install build/Pictura.vpk
    python tools/vita.py [--ip IP] shot screen.png          screenshot (PS + START) and download it
    python tools/vita.py [--ip IP] keys down down cross     tap buttons in order

`install` unpacks the VPK here, uploads it to ux0:/data/vita-install/<TITLEID>,
and asks the plugin to promote (install) that folder.

Give the Vita's address with --ip or $VITA_IP.
"""
import argparse
import ftplib
import io
import os
import socket
import struct
import sys
import time
import zipfile

CMD_PORT = 1338
FTP_PORT = 1337
STAGING = "ux0:/data/vita-install"
SHOTS = "ux0:/picture/SCREENSHOT"


def command(ip, text, wait=3.0):
    """Sends one command line and returns everything the plugin answers."""
    with socket.create_connection((ip, CMD_PORT), timeout=5) as s:
        s.sendall(text.encode() + b"\n")
        s.settimeout(wait)
        data = b""
        try:
            while True:
                chunk = s.recv(4096)
                if not chunk:
                    break
                data += chunk
        except socket.timeout:
            pass
    return data.decode(errors="replace")


def keys(ip, names, hold_ms=120, gap_ms=280):
    """Taps buttons in order (e.g. ["down", "down", "cross"]). The plugin limits
    how many commands one line may chain, so taps are sent a few at a time."""
    steps = []
    for name in names:
        steps += [f"press {name}", f"wait {hold_ms}ms", f"release {name}", f"wait {gap_ms}ms"]
    for k in range(0, len(steps), 8):
        reply = command(ip, "; ".join(steps[k:k + 8]), wait=2.0)
        if "Error" in reply:
            raise RuntimeError(reply.strip())


def ftp(ip):
    f = ftplib.FTP()
    f.connect(ip, FTP_PORT, timeout=15)
    f.login()
    return f


def wait_online(ip, timeout=300):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            reply = command(ip, "version", wait=2)
            if reply.strip():
                return reply.strip()
        except OSError:
            pass
        time.sleep(2)
    return None


def mkdirs(f, path):
    device, _, rest = path.partition(":/")
    current = device + ":"
    for part in rest.split("/"):
        if not part:
            continue
        current += "/" + part
        try:
            f.mkd(current)
        except ftplib.error_perm:
            pass  # already there


def get_dir(f, remote, local):
    os.makedirs(local, exist_ok=True)
    entries = []
    f.retrlines("LIST " + remote, entries.append)
    for line in entries:
        name = line.split(None, 8)[-1]
        if name in (".", ".."):
            continue
        r, l = remote.rstrip("/") + "/" + name, os.path.join(local, name)
        if line.startswith("d"):
            get_dir(f, r, l)
        else:
            with open(l, "wb") as out:
                f.retrbinary("RETR " + r, out.write)


def title_id(sfo):
    keys, values, count = struct.unpack("<III", sfo[8:20])
    for i in range(count):
        key_at, _, length, _, value_at = struct.unpack("<HHIII", sfo[20 + i * 16:36 + i * 16])
        key = sfo[keys + key_at:sfo.index(b"\0", keys + key_at)]
        if key == b"TITLE_ID":
            return sfo[values + value_at:values + value_at + length].rstrip(b"\0").decode()
    raise ValueError("no TITLE_ID in param.sfo")


def install(ip, vpk):
    z = zipfile.ZipFile(vpk)
    tid = title_id(z.read("sce_sys/param.sfo"))
    root = f"{STAGING}/{tid}"
    f = ftp(ip)
    mkdirs(f, root)
    for info in z.infolist():
        if info.is_dir():
            continue
        target = f"{root}/{info.filename}"
        mkdirs(f, target.rsplit("/", 1)[0])
        f.storbinary("STOR " + target, io.BytesIO(z.read(info)))
        print("  sent", info.filename)
    f.quit()
    print(command(ip, f"promote {root}", wait=30).strip())


def newest_screenshot(f):
    """Path of the newest PNG under ux0:/picture/SCREENSHOT (one folder per app)."""
    best = None
    folders = []
    f.retrlines("LIST " + SHOTS, folders.append)
    for line in folders:
        if not line.startswith("d"):
            continue
        folder = f"{SHOTS}/{line.split(None, 8)[-1]}"
        files = []
        f.retrlines("LIST " + folder, files.append)
        for entry in files:
            name = entry.split(None, 8)[-1]
            if name.lower().endswith(".png") and (best is None or name > best[0]):
                best = (name, f"{folder}/{name}")  # names are timestamps
    return best[1] if best else None


def remove_if_empty(f, folder):
    entries = []
    f.retrlines("LIST " + folder, entries.append)
    if not any(e.split(None, 8)[-1] not in (".", "..") for e in entries):
        f.rmd(folder)


def screenshot(ip, local):
    """Takes a screenshot with PS + START (needs screenshots enabled), downloads
    it, and deletes it from the Vita so test shots don't pile up in Photos."""
    f = ftp(ip)
    before = newest_screenshot(f)
    command(ip, "press ps; wait 600ms; press start; wait 600ms; release start; wait 200ms; release ps; release all")
    for _ in range(10):
        time.sleep(1)
        latest = newest_screenshot(f)
        if latest and latest != before:
            # The system writes the PNG in steps: download only once it is
            # complete, and delete it from the Vita only after that.
            for _ in range(20):
                time.sleep(0.5)
                data = bytearray()
                f.retrbinary("RETR " + latest, data.extend)
                if data.endswith(b"IEND\xaeB`\x82"):
                    with open(local, "wb") as out:
                        out.write(data)
                    f.delete(latest)
                    remove_if_empty(f, latest.rsplit("/", 1)[0])
                    f.quit()
                    return local
            break
    f.quit()
    return None


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--ip", default=os.environ.get("VITA_IP"))
    p.add_argument("action", choices=["wait", "cmd", "ls", "get", "getdir", "put", "install", "shot", "keys"])
    p.add_argument("args", nargs="*")
    a = p.parse_args()
    if a.action == "wait":
        reply = wait_online(a.ip)
        print(reply or "no answer")
        sys.exit(0 if reply else 1)
    if a.action == "cmd":
        print(command(a.ip, " ".join(a.args), wait=10).strip())
        return
    if a.action == "install":
        install(a.ip, a.args[0])
        return
    if a.action == "keys":
        keys(a.ip, a.args)
        return
    if a.action == "shot":
        saved = screenshot(a.ip, a.args[0] if a.args else "screen.png")
        print("saved", saved) if saved else print("no new screenshot (are screenshots enabled?)")
        sys.exit(0 if saved else 1)
    f = ftp(a.ip)
    if a.action == "ls":
        f.retrlines("LIST " + a.args[0])
    elif a.action == "get":
        local = a.args[1] if len(a.args) > 1 else os.path.basename(a.args[0])
        with open(local, "wb") as out:
            f.retrbinary("RETR " + a.args[0], out.write)
        print("saved", local)
    elif a.action == "getdir":
        get_dir(f, a.args[0], a.args[1])
        print("saved", a.args[1])
    elif a.action == "put":
        mkdirs(f, a.args[1].rsplit("/", 1)[0])
        with open(a.args[0], "rb") as src:
            f.storbinary("STOR " + a.args[1], src)
        print("sent", a.args[1])
    f.quit()


if __name__ == "__main__":
    main()
