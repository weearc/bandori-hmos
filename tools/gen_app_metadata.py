#!/usr/bin/env python3
r"""Generate application meta-data values from your own APK.

The game declares SDK keys (for example Mob-AppKey / Mob-AppSecret) as
<meta-data> entries in AndroidManifest.xml. Those values belong to the game
publisher, so they are not distributed with this repository: this script reads
them from your APK and writes the two generated files the runtime uses.

Usage:
    python tools/gen_app_metadata.py --apk D:\path\to\your.apk

Outputs (both git-ignored):
    login-milestone-next/application-metadata-typed.json
    package-metadata-next/metadata_values.inc
"""
import argparse
import hashlib
import json
import pathlib
import struct
import sys
import zipfile

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(HERE))
from apk_metadata import apk_path  # noqa: E402

# Android resource value types used by the .inc reader.
TYPE_STRING = 3
TYPE_INT_DEC = 16
TYPE_INT_BOOLEAN = 18


def _parse_strings(blob, p, header):
    count = struct.unpack_from('<I', blob, p + 8)[0]
    flags = struct.unpack_from('<I', blob, p + 16)[0]
    start = struct.unpack_from('<I', blob, p + 20)[0]
    utf8 = bool(flags & 256)
    out = []
    for i in range(count):
        q = p + start + struct.unpack_from('<I', blob, p + header + 4 * i)[0]
        if utf8:
            n = blob[q]; q += 1
            if n & 0x80:
                n = ((n & 0x7F) << 8) | blob[q]; q += 1
            n2 = blob[q]; q += 1
            if n2 & 0x80:
                q += 1
            out.append(blob[q:q + n].decode('utf-8', 'replace'))
        else:
            n = struct.unpack_from('<H', blob, q)[0]; q += 2
            if n & 0x8000:
                n = ((n & 0x7FFF) << 16) | struct.unpack_from('<H', blob, q)[0]; q += 2
            out.append(blob[q:q + n * 2].decode('utf-16le', 'replace'))
    return out


def read_meta_data(apk):
    with zipfile.ZipFile(apk) as z:
        blob = z.read('AndroidManifest.xml')
    strings = []
    results = []
    p = struct.unpack_from('<H', blob, 2)[0]
    while p + 8 <= len(blob):
        ctype, header, size = struct.unpack_from('<HHI', blob, p)
        if size < header or size < 8 or p + size > len(blob):
            break
        if ctype == 1:
            strings = _parse_strings(blob, p, header)
        elif ctype == 0x0102:  # start element
            name_idx = struct.unpack_from('<I', blob, p + 20)[0]
            attr_start = struct.unpack_from('<H', blob, p + 24)[0]
            attr_size = struct.unpack_from('<H', blob, p + 26)[0]
            attr_count = struct.unpack_from('<H', blob, p + 28)[0]
            if strings and strings[name_idx] == 'meta-data':
                record = {}
                base = p + 16 + attr_start
                for i in range(attr_count):
                    q = base + i * attr_size
                    aname = strings[struct.unpack_from('<I', blob, q + 4)[0]]
                    raw = struct.unpack_from('<I', blob, q + 8)[0]
                    vtype = blob[q + 15]
                    vdata = struct.unpack_from('<I', blob, q + 16)[0]
                    if aname in ('name', 'value'):
                        if raw != 0xFFFFFFFF:
                            record[aname] = strings[raw]
                        elif vtype == TYPE_STRING:
                            record[aname] = strings[vdata]
                        else:
                            record[aname] = vdata
                        record.setdefault('_types', {})[aname] = vtype
                if 'name' in record:
                    results.append(record)
        p += size
    return results


def main():
    ap = argparse.ArgumentParser(description='Generate app metadata from an APK')
    ap.add_argument('--apk', help='your APK (default: $BG_APK or the bundled rawfile copy)')
    args = ap.parse_args()
    apk = apk_path(args.apk)
    if not apk.is_file():
        print('APK not found: %s' % apk)
        return 2
    records = read_meta_data(apk)
    rows, typed = [], []
    for rec in sorted(records, key=lambda r: r['name']):
        name = rec['name']
        types = rec.get('_types', {})
        value = rec.get('value')
        vtype = types.get('value', TYPE_STRING)
        if isinstance(value, str) and not value.isdigit():
            rows.append('{%s,%d,%s,0u},' % (json.dumps(name), TYPE_STRING, json.dumps(value)))
            typed.append({'attributes': {'name': name, 'value': value},
                          'android_value_types': {'name': TYPE_STRING, 'value': TYPE_STRING}})
        elif isinstance(value, int) or (isinstance(value, str) and value.isdigit()):
            num = int(value)
            rows.append('{%s,%d,NULL,%du},' % (json.dumps(name), vtype, num))
            typed.append({'attributes': {'name': name, 'value': num},
                          'android_value_types': {'name': TYPE_STRING, 'value': vtype}})
        else:
            rows.append('{%s,%d,NULL,0u},' % (json.dumps(name), vtype))
            typed.append({'attributes': {'name': name, 'value': None},
                          'android_value_types': {'name': TYPE_STRING, 'value': vtype}})
    inc = ROOT / 'package-metadata-next/metadata_values.inc'
    inc.parent.mkdir(parents=True, exist_ok=True)
    inc.write_text('\n'.join(rows) + '\n', encoding='utf-8')
    js = ROOT / 'login-milestone-next/application-metadata-typed.json'
    js.parent.mkdir(parents=True, exist_ok=True)
    js.write_text(json.dumps({
        'source_apk_sha256': hashlib.sha256(apk.read_bytes()).hexdigest(),
        'metadata': typed,
    }, indent=2) + '\n', encoding='utf-8')
    print('meta-data entries: %d' % len(rows))
    print('wrote %s' % inc)
    print('wrote %s' % js)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
