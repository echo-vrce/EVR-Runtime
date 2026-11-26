"""EchoCombat's content pack for an EVR-Runtime release (runtime src/combat).

    python -I runtime/tools/make_combat_pack.py <EchoCombat release dir> --zip echocombat-pack.zip
    python -I runtime/tools/make_combat_pack.py <EchoCombat release dir> --install <plugins\\versions\\<ver>>

<EchoCombat release dir> is what EchoCombat's nevr-pack branch builds with `tools/make_release.py`
(build/release/EchoCombat: content/echocombat/ from `evrtool pack`, plugins/PowerBoost.ini). Its plugin DLLs are
not used: the runtime has them built in.

--zip writes the zip CI unpacks into the release's pc/ folder (the fixed GitHub release "combat-pc" holds it as
echocombat-pack.zip): content/echocombat/... and PowerBoost.ini, beside evr-runtime.dll.
--install copies the same files into an installed version folder (a local test).
Every file content.json lists is checked against its sha256 first, as the runtime will.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import sys
import zipfile
from pathlib import Path


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def pack_files(release: Path) -> list[tuple[Path, str]]:
    """(source, path in the version folder) for every file the runtime needs."""
    pack = release / "content" / "echocombat"
    meta = json.loads((pack / "content.json").read_text(encoding="utf-8"))
    if meta.get("format") != 1 or meta.get("id") != "echocombat":
        sys.exit("content.json is not a format 1 echocombat pack")
    out = [(pack / "content.json", "content/echocombat/content.json")]
    for rel, want in sorted(meta["files"].items()):
        src = pack / rel
        if not src.is_file() or sha256(src) != want.lower():
            sys.exit(f"pack file {rel} is missing or changed: rebuild the EchoCombat release")
        out.append((src, f"content/echocombat/{rel}"))
    ini = release / "plugins" / "PowerBoost.ini"
    if not ini.is_file():
        sys.exit(f"no {ini}")
    out.append((ini, "PowerBoost.ini"))
    print(f"EchoCombat {meta.get('version', '?')}: {len(meta['files'])} pack files + PowerBoost.ini")
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("release", type=Path)
    ap.add_argument("--zip", type=Path)
    ap.add_argument("--install", type=Path)
    args = ap.parse_args()
    if not args.zip and not args.install:
        ap.error("--zip and/or --install")
    files = pack_files(args.release)
    if args.zip:
        with zipfile.ZipFile(args.zip, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
            for src, rel in files:
                z.write(src, rel)
        print(f"wrote {args.zip} ({args.zip.stat().st_size // 1024} KB)")
    if args.install:
        if not (args.install / "evr-runtime.dll").is_file():
            sys.exit(f"{args.install} has no evr-runtime.dll: not a version folder")
        old = args.install / "content" / "echocombat"
        if old.exists():
            shutil.rmtree(old)
        for src, rel in files:
            dst = args.install / rel
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src, dst)
        print(f"installed into {args.install}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
