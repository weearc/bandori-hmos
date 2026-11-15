#!/usr/bin/env python3
r"""Generate asset-reader-next/assets.inc from a user-supplied game APK.

This file is NOT committed: it is derived from your own copy of the game.
Run it once (or let tools/build_native.ps1 / build_tls_candidate.py call it)
before compiling the runtime.

Usage:
    python asset-reader-next/generate.py --apk D:\path\to\your.apk
    python asset-reader-next/generate.py --apk your.apk --no-embed

By default, small text-like assets (<= 64 KiB, not under assets/bin/) are
embedded as C byte arrays; everything else is represented by a manifest entry
(path, size, sha256) and read from the APK payload at runtime.

--no-embed keeps only the manifest and embeds nothing.
"""
import argparse
import hashlib
import json
import pathlib
import sys
import zipfile

HERE = pathlib.Path(__file__).resolve().parent
EMBED_LIMIT = 65536


def build(apk: pathlib.Path, embed: bool) -> None:
    rows = []
    arrays = []
    entries = []
    with zipfile.ZipFile(apk) as archive:
        names = [n for n in archive.namelist() if n.startswith("assets/") and not n.endswith("/")]
        for index, name in enumerate(names):
            data = archive.read(name)
            small = len(data) <= EMBED_LIMIT and not name.startswith("assets/bin/") and not name.endswith(".bin")
            do_embed = embed and small
            rows.append({
                "path": name,
                "size": len(data),
                "sha256": hashlib.sha256(data).hexdigest(),
                "embedded": do_embed,
            })
            if do_embed:
                body = ",".join(str(b) for b in data) or "0"
                arrays.append("static const unsigned char b%d[]={%s};" % (index, body))
                pointer = "b%d" % index
            else:
                pointer = "NULL"
            entries.append('{"%s",%s,%d},' % (name[len("assets/"):], pointer, len(data)))

    out = arrays + ["static const asset assets[]={"] + entries + ["};"]
    (HERE / "assets.inc").write_text("\n".join(out) + "\n", encoding="utf-8")
    (HERE / "source-manifest.json").write_text(json.dumps(rows, indent=2), encoding="utf-8")
    embedded = sum(1 for r in rows if r["embedded"])
    print("assets.inc: %d entries (%d embedded, %d manifest-only)" % (len(rows), embedded, len(rows) - embedded))
    print("NOTE: this file is derived from your own APK; do not commit it.")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--apk", type=pathlib.Path, required=True, help="your own game APK")
    parser.add_argument("--no-embed", action="store_true", help="embed no bytes, keep manifest only")
    args = parser.parse_args()
    apk = args.apk.resolve()
    if not apk.is_file():
        print("APK not found: %s" % apk, file=sys.stderr)
        return 2
    build(apk, embed=not args.no_embed)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
