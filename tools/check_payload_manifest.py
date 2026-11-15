#!/usr/bin/env python3
"""Check whether host/entry/src/main/cpp/apk_payload_manifest.h matches your APK.

The manifest is bound to one APK build: it records the APK size/CRC and, for
every extracted asset, its size and CRC. Switching APK versions therefore
requires regenerating it; this script tells you whether the tracked header
still describes the APK you supplied.

Usage:
    python tools/check_payload_manifest.py --apk D:\path\to\your.apk
Exit codes: 0 match, 1 mismatch, 2 usage/IO error.
"""
import argparse
import pathlib
import re
import sys
import zlib

HEADER = pathlib.Path(__file__).resolve().parent.parent / "host/entry/src/main/cpp/apk_payload_manifest.h"


def read_manifest(path: pathlib.Path):
    text = path.read_text(encoding="utf-8", errors="replace")

    def take(pattern, cast=int):
        m = re.search(pattern, text)
        if not m:
            raise SystemExit("manifest field not found: %s" % pattern)
        return cast(m.group(1))

    size = take(r"APK_SOURCE_SIZE=(\d+)ULL")
    crc = take(r"APK_SOURCE_CRC=(\d+)U")
    entries = re.findall(r'\{"([^"]+)","([^"]+)",(\d+)ULL,(\d+)U\}', text)
    return size, crc, entries


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--apk", type=pathlib.Path, required=True)
    parser.add_argument("--header", type=pathlib.Path, default=HEADER)
    args = parser.parse_args()

    apk = args.apk.resolve()
    if not apk.is_file():
        print("APK not found: %s" % apk, file=sys.stderr)
        return 2
    if not args.header.is_file():
        print("manifest not found: %s" % args.header, file=sys.stderr)
        return 2

    data = apk.read_bytes()
    size, crc = len(data), zlib.crc32(data) & 0xFFFFFFFF
    msize, mcrc, entries = read_manifest(args.header)

    print("APK      size=%d crc=%d" % (size, crc))
    print("manifest size=%d crc=%d entries=%d" % (msize, mcrc, len(entries)))
    if size == msize and crc == mcrc:
        print("RESULT: MATCH — the tracked manifest describes this APK")
        return 0

    print("RESULT: MISMATCH — the tracked manifest belongs to a different APK build")
    print("Action: regenerate apk_payload_manifest.h for your APK before building,")
    print("        or use the APK build the manifest was generated from.")
    print("Note: the source-side size/CRC check is necessary but not sufficient;")
    print("      per-entry CRC comparison is done at extraction time by the runtime.")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
