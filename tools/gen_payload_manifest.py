#!/usr/bin/env python3
r"""Generate apk_payload_manifest.h (metadata only) from a user-supplied APK.

The header records the APK version, its size and sampled identity, and one row
per payload entry (APK path, destination, uncompressed size, zip CRC32). It
contains no game bytes: the payload stays inside base.apk.

Usage:
    python tools/gen_payload_manifest.py --apk D:\path\to\your.apk
    python tools/gen_payload_manifest.py --apk your.apk --out native-probe/entry/src/main/cpp/apk_payload_manifest.h

Then verify it matches the APK you will actually ship:
    python tools/check_payload_manifest.py --apk D:\path\to\your.apk
"""
import argparse
import hashlib
import json
import pathlib
import re
import sys
import zipfile

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(HERE))
from apk_metadata import apk_path, sampled_key, version  # noqa: E402


def collect(apk: pathlib.Path):
    entries = []
    with zipfile.ZipFile(apk) as z:
        for info in z.infolist():
            if info.is_dir():
                continue
            name = info.filename
            if name.startswith('assets/'):
                dest = 'native-assets/' + name[len('assets/'):]
            elif name.startswith('lib/arm64-v8a/'):
                continue  # installed as HAP native libraries, not extracted
            elif re.fullmatch(r'classes(?:[0-9]+)?\.dex', name):
                dest = name
            else:
                continue  # other resources stay reachable through base.apk
            if any(part in ('', '..', '.') for part in dest.split('/')):
                raise SystemExit('unsafe destination path: ' + dest)
            entries.append(dict(name=name, dest=dest, size=info.file_size, crc=info.CRC,
                                sha256=hashlib.sha256(z.read(info)).hexdigest() if dest.endswith('.dex') else ''))
    entries.sort(key=lambda e: e['name'])
    return entries


def render(apk: pathlib.Path, entries, out: pathlib.Path):
    canonical = ''.join('%s\t%s\t%d\t%08x\n' % (e['name'], e['dest'], e['size'], e['crc']) for e in entries)
    payload_hash = hashlib.sha256(canonical.encode()).hexdigest()
    app = version(apk)
    lines = ['// Generated metadata; payload stays exclusively inside base.apk.',
             'static const uint32_t APK_APP_VERSION_CODE=%dU;' % app['versionCode'],
             'static const char APK_APP_VERSION_NAME[]=%s;' % json.dumps(app['versionName']),
             'static const uint64_t APK_SOURCE_SIZE=%dULL;' % apk.stat().st_size,
             '// Whole APK CRC is diagnostic-only; identity uses sampled SHA256.',
             'static const char APK_SOURCE_ID[]="%s";' % sampled_key(apk),
             'static const char APK_PAYLOAD_HASH[]="%s";' % payload_hash,
             'static const ApkPayloadEntry APK_PAYLOAD[]={']
    lines += ['{%s,%s,%dULL,%dU},' % (json.dumps(e['name']), json.dumps(e['dest']), e['size'], e['crc']) for e in entries]
    lines += ['};', 'struct PayloadBootDigest {const char *name,*sha256;};',
              'static const PayloadBootDigest APK_BOOT_DIGESTS[]={']
    lines += ['{%s,%s},' % (json.dumps(e['name']), json.dumps(e['sha256'])) for e in entries if e['sha256']]
    lines += ['};', '']
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text('\n'.join(lines), encoding='utf-8')
    return payload_hash


def main(argv):
    ap = argparse.ArgumentParser(description='Generate apk_payload_manifest.h from an APK')
    ap.add_argument('--apk', help='path to your APK (default: $BG_APK or the bundled rawfile copy)')
    ap.add_argument('--out', default=str(ROOT / 'native-probe/entry/src/main/cpp/apk_payload_manifest.h'),
                    help='header to write (default: native-probe/entry/src/main/cpp/apk_payload_manifest.h)')
    ap.add_argument('--report', help='optional JSON report path')
    args = ap.parse_args(argv)
    apk = apk_path(args.apk)
    if not apk.is_file():
        print('APK not found: %s (pass --apk or set BG_APK)' % apk)
        return 2
    entries = collect(apk)
    out = pathlib.Path(args.out)
    payload_hash = render(apk, entries, out)
    assets = [e for e in entries if e['name'].startswith('assets/')]
    report = dict(apk=str(apk), apk_bytes=apk.stat().st_size, manifest_sha256=payload_hash,
                  entries=len(entries), expanded_bytes=sum(e['size'] for e in entries),
                  asset_files=len(assets), asset_bytes=sum(e['size'] for e in assets),
                  dex=[e['name'] for e in entries if e['dest'].endswith('.dex')])
    print(json.dumps(report, indent=2))
    if args.report:
        pathlib.Path(args.report).write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print('wrote %s' % out)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
