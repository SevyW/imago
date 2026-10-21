"""Publishes a GitHub release of the VPK in build/ and points the README at it.

Usage: python tools/release.py <version> <notes.md>
  version: e.g. 1.1.0 or 1.2.0-beta.1 (must match APP_VERSION in src/app.h);
           versions with a suffix are published as pre-releases
  notes:   Markdown release notes; a checksum section is appended.

Requires the gh CLI. Run it after a clean release build (IMAGO_AUTOPILOT off).
"""
import hashlib
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
REPO = "SevyW/imago"


def main():
    version, notes_path = sys.argv[1], Path(sys.argv[2])
    app_h = (ROOT / "src" / "app.h").read_text()
    if f'#define APP_VERSION "{version}"' not in app_h:
        sys.exit(f"APP_VERSION in src/app.h is not {version}")
    cache = (ROOT / "build" / "CMakeCache.txt").read_text()
    if "IMAGO_AUTOPILOT:BOOL=OFF" not in cache:
        sys.exit("build/ must be a release build (IMAGO_AUTOPILOT=OFF)")

    tag = f"v{version}"
    prerelease = "-" in version
    name = f"imago-{tag}.vpk"
    dist = ROOT / "dist"
    dist.mkdir(exist_ok=True)
    vpk = dist / name
    shutil.copy(ROOT / "build" / "Imago.vpk", vpk)
    data = vpk.read_bytes()
    md5, sha = hashlib.md5(data).hexdigest(), hashlib.sha256(data).hexdigest()
    size = f"{len(data) / 1048576:.1f} MB"

    notes = notes_path.read_text(encoding="utf-8").rstrip() + f"""

## 🔒 Checksums

| File | Size | MD5 |
|---|---|---|
| `{name}` | {size} | `{md5}` |

```
SHA-256  {sha}
```
"""
    full_notes = dist / f"notes-{tag}.md"
    full_notes.write_text(notes, encoding="utf-8")

    readme = ROOT / "README.md"
    text = readme.read_text(encoding="utf-8")
    badge = version.replace("-", "--")
    text = re.sub(r"badge/version-[^)]*-([0-9a-f]{6})\)", lambda m: f"badge/version-{badge}-{m[1]})", text)
    start = text.index("## 📦 Download")
    end = text.index("\n## ", start + 1)
    section = f"""## 📦 Download

**[Imago {tag}](https://github.com/{REPO}/releases/tag/{tag})**: the latest {"beta" if prerelease else "release"}. ([All releases](https://github.com/{REPO}/releases))

| File | Size | MD5 |
|---|---|---|
| `{name}` | {size} | `{md5}` |

<details>
<summary>SHA-256 and how to check the file</summary>

```
SHA-256  {sha}
```

```bash
md5sum {name}                     # Linux / macOS / Git Bash
certutil -hashfile {name} MD5     # Windows
```

Each release's notes list what changed, what to test, and how to recover if something goes wrong.

</details>
"""
    text = text[:start] + section + text[end:]
    readme.write_text(text, encoding="utf-8", newline="\n")

    def run(*args):
        print("+", " ".join(args))
        subprocess.run(args, cwd=ROOT, check=True)

    run("git", "add", "README.md", "src/app.h")
    run("git", "commit", "-m", f"Release {version}\n\nCo-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>")
    run("git", "push")
    run("gh", "release", "create", tag, str(vpk), "--repo", REPO, "--target", "main",
        *(["--prerelease"] if prerelease else []), "--title", f"Imago {tag}", "--notes-file", str(full_notes))

    # Confirm what GitHub serves is byte-for-byte what was built.
    check = dist / "verify"
    shutil.rmtree(check, ignore_errors=True)
    run("gh", "release", "download", tag, "--repo", REPO, "-D", str(check))
    served = hashlib.sha256((check / name).read_bytes()).hexdigest()
    shutil.rmtree(check, ignore_errors=True)
    if served != sha:
        sys.exit("published asset does not match the build!")
    print(f"published {tag}: {name} {size} md5={md5}")


if __name__ == "__main__":
    main()
