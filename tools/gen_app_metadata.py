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
TYPE_REFERENCE = 1
TYPE_STRING = 3
TYPE_INT_DEC = 16
TYPE_INT_HEX = 17
TYPE_INT_BOOLEAN = 18
TYPE_INT_COLOR_ARGB8 = 28
NO_ENTRY = 0xFFFFFFFF


def _parse_strings(blob, p, header):
    """Decode a ResStringPool chunk (UTF-8 or UTF-16, per the pool flags)."""
    count = struct.unpack_from('<I', blob, p + 8)[0]
    flags = struct.unpack_from('<I', blob, p + 16)[0]
    start = struct.unpack_from('<I', blob, p + 20)[0]
    utf8 = bool(flags & 0x100)
    out = []
    for i in range(count):
        off = struct.unpack_from('<I', blob, p + header + 4 * i)[0]
        q = p + start + off
        if utf8:
            # UTF-8 pool layout: UTF-16 length first (u16, 0x8000 extends it),
            # then the UTF-8 byte length (u8, 0x80 extends it), then the bytes.
            c = struct.unpack_from('<H', blob, q)[0]; q += 2
            if c & 0x8000:
                c = ((c & 0x7FFF) << 16) | struct.unpack_from('<H', blob, q)[0]; q += 2
            n = blob[q]; q += 1
            if n & 0x80:
                n = ((n & 0x7F) << 8) | blob[q]; q += 1
            out.append(blob[q:q + n].decode('utf-8', 'replace'))
        else:
            # UTF-16 pool: character length, 1 or 2 little-endian u16.
            n = struct.unpack_from('<H', blob, q)[0]; q += 2
            if n & 0x8000:
                n = ((n & 0x7FFF) << 16) | struct.unpack_from('<H', blob, q)[0]; q += 2
            out.append(blob[q:q + n * 2].decode('utf-16le', 'replace'))
    return out


def read_meta_data(apk):
    """Return the <application> <meta-data> entries of AndroidManifest.xml.

    Only direct children of <application> are collected (provider/service deps
    declare their own meta-data that the runtime does not read), duplicate names
    keep their first occurrence, and each value keeps its declared Android type:
    strings resolve through the pool, booleans normalise to true/false, and every
    other numeric type keeps its 32-bit value.
    """
    with zipfile.ZipFile(apk) as z:
        blob = z.read('AndroidManifest.xml')
    strings = []
    results = []
    seen = set()
    tag_stack = []
    p = struct.unpack_from('<H', blob, 2)[0]
    while p + 8 <= len(blob):
        ctype, header, size = struct.unpack_from('<HHI', blob, p)
        if size < header or size < 8 or p + size > len(blob):
            break
        if ctype == 0x0001:  # string pool
            strings = _parse_strings(blob, p, header)
        elif ctype == 0x0102:  # start element
            name_idx = struct.unpack_from('<I', blob, p + 20)[0]
            tag = strings[name_idx] if strings and name_idx < len(strings) else ''
            if tag == 'meta-data' and tag_stack and tag_stack[-1] == 'application':
                record, types = {}, {}
                attr_start = struct.unpack_from('<H', blob, p + 24)[0]
                attr_size = struct.unpack_from('<H', blob, p + 26)[0]
                attr_count = struct.unpack_from('<H', blob, p + 28)[0]
                base = p + 16 + attr_start
                for i in range(attr_count):
                    q = base + i * attr_size
                    aname = strings[struct.unpack_from('<I', blob, q + 4)[0]]
                    raw = struct.unpack_from('<I', blob, q + 8)[0]
                    vtype = blob[q + 15]
                    vdata = struct.unpack_from('<I', blob, q + 16)[0]
                    if aname not in ('name', 'value'):
                        continue
                    types[aname] = vtype
                    if vtype == TYPE_STRING:
                        idx = raw if raw != NO_ENTRY else vdata
                        record[aname] = strings[idx] if idx < len(strings) else None
                    elif vtype == TYPE_INT_BOOLEAN:
                        record[aname] = bool(vdata)
                    else:
                        record[aname] = vdata
                name = record.get('name')
                if name and name not in seen:
                    seen.add(name)
                    record['_types'] = types
                    results.append(record)
            tag_stack.append(tag)
        elif ctype == 0x0103:  # end element
            if tag_stack:
                tag_stack.pop()
        p += size
    return results

def render(records):
    """Render records exactly as Android declared them.

    The value type decides the representation: a string stays a string even when
    it looks numeric (unity.builder="00123" must not become the integer 123),
    while int/boolean types are emitted as numbers with a NULL string pointer.
    """
    rows, typed = [], []
    for rec in sorted(records, key=lambda r: r['name']):
        name = rec['name']
        vtype = rec.get('_types', {}).get('value', TYPE_STRING)
        value = rec.get('value')
        if vtype == TYPE_STRING:
            text = value if isinstance(value, str) else (str(value) if value is not None else None)
            if text is None:
                rows.append('{%s,%d,NULL,0u},' % (json.dumps(name, ensure_ascii=False), TYPE_STRING))
                typed.append({'attributes': {'name': name, 'value': None},
                              'android_value_types': {'name': TYPE_STRING, 'value': TYPE_STRING}})
            else:
                rows.append('{%s,%d,%s,0u},' % (json.dumps(name, ensure_ascii=False), TYPE_STRING, json.dumps(text, ensure_ascii=False)))
                typed.append({'attributes': {'name': name, 'value': text},
                              'android_value_types': {'name': TYPE_STRING, 'value': TYPE_STRING}})
        elif vtype == TYPE_INT_BOOLEAN:
            flag = 1 if (value is True or (isinstance(value, int) and value != 0)) else 0
            rows.append('{%s,%d,NULL,%du},' % (json.dumps(name, ensure_ascii=False), TYPE_INT_BOOLEAN, flag))
            typed.append({'attributes': {'name': name, 'value': bool(flag)},
                          'android_value_types': {'name': TYPE_STRING, 'value': TYPE_INT_BOOLEAN}})
        else:
            num = int(value) if isinstance(value, (int, bool)) else (int(value) if isinstance(value, str) and value.lstrip('-').isdigit() else 0)
            rows.append('{%s,%d,NULL,%du},' % (json.dumps(name, ensure_ascii=False), vtype, num))
            typed.append({'attributes': {'name': name, 'value': num},
                          'android_value_types': {'name': TYPE_STRING, 'value': vtype}})
    return rows, typed


def main():
    ap = argparse.ArgumentParser(description='Generate app metadata from an APK')
    ap.add_argument('--apk', help='your APK (default: $BG_APK or the bundled rawfile copy)')
    args = ap.parse_args()
    apk = apk_path(args.apk)
    if not apk.is_file():
        print('APK not found: %s' % apk)
        return 2
    records = read_meta_data(apk)
    rows, typed = render(records)
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
