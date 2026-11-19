#!/usr/bin/env python3
r"""Fail if a produced runtime ELF still carries game asset payload bytes.

Manifest-only asset tables legitimately contain APK paths and NULL data
pointers, and ELF padding can match trivially repetitive asset regions, so
both are filtered out. A failure means one of:

  1) a distinctive 64-byte slice of an APK asset appears verbatim in the ELF
     (embedded arrays reproduce file content exactly), or
  2) the ELF contains an asset-container signature (PNG header, ACB header,
     UnityFS bundle header).

Usage:
    python tools/verify_no_embedded_assets.py --apk yours.apk libsdktrace.so
    python tools/verify_no_embedded_assets.py libsdktrace.so        # structural only

Exit 0 clean, 1 payload found, 2 argument/IO error.
"""
import argparse
import pathlib
import sys
import zipfile

# Container signatures that only embedded asset files would introduce.
MAGICS = [
    (b'\x89PNG\r\n\x1a\n', 'PNG header'),
    (b'UnityFS', 'UnityFS bundle header'),
    (b'\x40\x00\x00\x00ACB', 'ACB header'),
]
MIN_DISTINCT = 8  # a 64-byte probe with fewer distinct byte values is padding


def structural_hits(blob: bytes):
    return ['%s present' % label for magic, label in MAGICS if magic in blob]


def payload_hits(blob: bytes, apk: pathlib.Path):
    hits = []
    with zipfile.ZipFile(apk) as z:
        for name in z.namelist():
            if not name.startswith('assets/') or name.endswith('/'):
                continue
            data = z.read(name)
            if len(data) < 256:
                continue
            probe = data[len(data) // 2:len(data) // 2 + 64]
            if len(probe) < 64 or len(set(probe)) < MIN_DISTINCT:
                continue  # padding-like regions match by coincidence
            if probe in blob:
                hits.append('%s (%d bytes) payload present' % (name, len(data)))
    return hits


def main(argv):
    ap = argparse.ArgumentParser(description='Detect embedded game assets in a runtime ELF')
    ap.add_argument('targets', nargs='+', type=pathlib.Path)
    ap.add_argument('--apk', type=pathlib.Path, help='your APK; enables payload-byte verification')
    args = ap.parse_args(argv)
    bad = 0
    for target in args.targets:
        if not target.is_file():
            print('ERROR: not a file: %s' % target)
            return 2
        blob = target.read_bytes()
        problems = structural_hits(blob)
        if args.apk:
            if not args.apk.is_file():
                print('ERROR: APK not found: %s' % args.apk)
                return 2
            problems += payload_hits(blob, args.apk)
        if problems:
            bad += 1
            print('FAIL %s' % target)
            for item in problems[:10]:
                print('  - ' + item)
        else:
            print('OK   %s (no embedded asset payload detected)' % target)
    if bad:
        print('\n%d file(s) carry game asset payload; rebuild with generate.py --no-embed' % bad)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
