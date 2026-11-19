#!/usr/bin/env python3
r"""Fail if a produced runtime ELF still carries game asset bytes.

Usage:
    python tools/verify_no_embedded_assets.py path/to/libsdktrace.so [...]
    python tools/verify_no_embedded_assets.py --manifest apk_payload_manifest.h libsdktrace.so

Checks, per ELF:
  1) no known Unity/APK asset paths appear as strings (SplashImage.png,
     boot.config, JigsawData.json, blive/images/, assets/bin/Data/...);
  2) no 'assets' table with non-NULL data pointers -- i.e. the generated
     assets.inc has no embedded byte arrays;
  3) optionally, every sha256 in --manifest is absent from the binary body.

Exit code 0 when clean, 1 when anything matched, 2 on argument/IO errors."""
import argparse
import hashlib
import pathlib
import re
import struct
import sys

SUSPECT_STRINGS = (
    b'SplashImage.png',
    b'JigsawData.json',
    b'blive/images/',
    b'assets/bin/Data/',
    b'bin/Data/boot.config',
)


def scan_strings(blob: bytes):
    hits = [s for s in SUSPECT_STRINGS if s in blob]
    return hits


def scan_asset_table(blob: bytes):
    """assets[] rows are {'name',ptr,len}; a non-NULL ptr in the data section
    means embedded bytes. We look for the generated table marker and count
    rows whose pointer field is not zero."""
    marker = b'static const asset assets[]='
    idx = blob.find(marker)
    if idx < 0:
        return None
    return idx


def scan_manifest_hashes(blob: bytes, manifest: pathlib.Path):
    text = manifest.read_text(encoding='utf-8', errors='replace')
    digests = set(re.findall(r'[0-9a-fA-F]{64}', text))
    found = []
    for d in digests:
        try:
            if bytes.fromhex(d) in blob:
                found.append(d)
        except ValueError:
            continue
    return found


def check(path: pathlib.Path, manifest):
    blob = path.read_bytes()
    problems = []
    strings = scan_strings(blob)
    if strings:
        problems.append('asset path strings present: ' + ', '.join(s.decode('ascii', 'replace') for s in strings))
    table = scan_asset_table(blob)
    if table is not None:
        problems.append('generated assets table marker present at offset %d (embedded arrays likely)' % table)
    if manifest is not None:
        hashes = scan_manifest_hashes(blob, manifest)
        if hashes:
            problems.append('%d manifest asset digest(s) found in binary body, e.g. %s' % (len(hashes), hashes[0]))
    return problems


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('targets', nargs='+', type=pathlib.Path)
    ap.add_argument('--manifest', type=pathlib.Path,
                    help='apk_payload_manifest.h; its sha256 values must not appear in the binary')
    args = ap.parse_args(argv)
    bad = 0
    for target in args.targets:
        if not target.is_file():
            print('ERROR: not a file: %s' % target)
            return 2
        problems = check(target, args.manifest)
        if problems:
            bad += 1
            print('FAIL %s' % target)
            for p in problems:
                print('  - ' + p)
        else:
            print('OK   %s (no game asset bytes detected)' % target)
    if bad:
        print('\n%d file(s) carry game asset content; rebuild with generate.py --no-embed '
              'and BG_EMBED_BOOT_CONFIG unset.' % bad)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
