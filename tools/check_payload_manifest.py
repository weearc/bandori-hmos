#!/usr/bin/env python3
r"""Check that apk_payload_manifest.h matches the APK you are about to ship.

The manifest is bound to one APK build. It records the APK size, a sampled
identity hash, the app version, and one row per extracted payload entry
(size + zip CRC32). Switching APK versions requires regenerating it
(tools/gen_payload_manifest.py).

Usage:
    python tools/check_payload_manifest.py --apk D:\path\to\your.apk
    python tools/check_payload_manifest.py --apk your.apk --header path/to/apk_payload_manifest.h

Exit codes: 0 match, 1 mismatch, 2 usage/IO error.
"""
import argparse
import hashlib
import pathlib
import re
import sys
import zipfile

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(HERE))
from apk_metadata import sampled_key, version  # noqa: E402

DEFAULT_HEADER = ROOT / 'native-probe/entry/src/main/cpp/apk_payload_manifest.h'


def read_manifest(path: pathlib.Path):
    text = path.read_text(encoding='utf-8', errors='replace')

    def field(name, pattern, cast=str, default=None):
        m = re.search(pattern, text)
        if not m:
            if default is not None:
                return default
            raise SystemExit('manifest field not found: %s' % name)
        return cast(m.group(1))

    data = dict(
        size=field('APK_SOURCE_SIZE', r'APK_SOURCE_SIZE=(\d+)ULL', int),
        source_id=field('APK_SOURCE_ID', r'APK_SOURCE_ID="([0-9a-f]{64})"', str, ''),
        payload_hash=field('APK_PAYLOAD_HASH', r'APK_PAYLOAD_HASH="([0-9a-f]{64})"', str, ''),
        version_code=field('APK_APP_VERSION_CODE', r'APK_APP_VERSION_CODE=(\d+)U', int, 0),
        version_name=field('APK_APP_VERSION_NAME', r'APK_APP_VERSION_NAME\[\]="([^"]*)"', str, ''),
    )
    data['entries'] = re.findall(r'\{"([^"]+)","([^"]+)",(\d+)ULL,(\d+)U\}', text)
    return data


def canonical_hash(entries):
    text = ''.join('%s\t%s\t%d\t%08x\n' % (n, d, int(size), int(crc)) for n, d, size, crc in entries)
    return hashlib.sha256(text.encode()).hexdigest()


def main() -> int:
    ap = argparse.ArgumentParser(description='Compare apk_payload_manifest.h with an APK')
    ap.add_argument('--apk', type=pathlib.Path, required=True)
    ap.add_argument('--header', type=pathlib.Path, default=DEFAULT_HEADER)
    args = ap.parse_args()

    apk = args.apk.resolve()
    if not apk.is_file():
        print('APK not found: %s' % apk, file=sys.stderr)
        return 2
    if not args.header.is_file():
        print('manifest not found: %s' % args.header, file=sys.stderr)
        print('generate it first: python tools/gen_payload_manifest.py --apk <your.apk>', file=sys.stderr)
        return 2

    manifest = read_manifest(args.header)
    problems = []

    if apk.stat().st_size != manifest['size']:
        problems.append('size: apk=%d manifest=%d' % (apk.stat().st_size, manifest['size']))
    try:
        sid = sampled_key(apk)
        app = version(apk)
    except Exception as exc:  # not a readable APK
        print('RESULT: MISMATCH')
        print('  - cannot read APK metadata: %s' % exc)
        return 1
    if manifest['source_id'] and sid != manifest['source_id']:
        problems.append('sampled id: apk=%s manifest=%s' % (sid[:16], manifest['source_id'][:16]))
    if manifest['version_code'] and app['versionCode'] != manifest['version_code']:
        problems.append('versionCode: apk=%s manifest=%s' % (app['versionCode'], manifest['version_code']))
    if manifest['version_name'] and app['versionName'] != manifest['version_name']:
        problems.append('versionName: apk=%s manifest=%s' % (app['versionName'], manifest['version_name']))

    with zipfile.ZipFile(apk) as z:
        known = {info.filename: info for info in z.infolist() if not info.is_dir()}
    missing, mismatch = [], []
    for name, _dest, size, crc in manifest['entries']:
        info = known.get(name)
        if info is None:
            missing.append(name)
        elif info.file_size != int(size) or info.CRC != int(crc):
            mismatch.append('%s (apk %d/%08x, manifest %s/%s)' % (name, info.file_size, info.CRC, size, crc))
    if missing:
        problems.append('%d manifest entries are absent from this APK, e.g. %s' % (len(missing), missing[0]))
    if mismatch:
        problems.append('%d entries differ in size/CRC, e.g. %s' % (len(mismatch), mismatch[0]))
    if manifest['payload_hash']:
        actual = canonical_hash(manifest['entries'])
        if actual != manifest['payload_hash']:
            problems.append('payload hash is inconsistent with its own rows (regenerate the header)')

    print('APK      %s' % apk.name)
    print('         size=%d version=%s(%s) sampled=%s' % (
        apk.stat().st_size, app['versionName'], app['versionCode'], sid[:16]))
    print('manifest %s' % args.header)
    print('         size=%d version=%s(%s) entries=%d' % (
        manifest['size'], manifest['version_name'], manifest['version_code'], len(manifest['entries'])))
    if problems:
        print('RESULT: MISMATCH')
        for item in problems:
            print('  - ' + item)
        print('Action: regenerate with python tools/gen_payload_manifest.py --apk <your.apk>')
        return 1
    print('RESULT: MATCH - this manifest describes the supplied APK')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
