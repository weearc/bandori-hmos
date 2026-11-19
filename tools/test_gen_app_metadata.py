#!/usr/bin/env python3
r"""Regression test for tools/gen_app_metadata.py.

Builds a synthetic AndroidManifest.xml covering the cases that broke before:
a numeric-looking string, a boolean, a hex integer, a Chinese value, and a
<meta-data> nested under <instrumentation> that must be ignored.
"""
import io
import pathlib
import struct
import sys
import zipfile

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[1] / 'tools'))
import gen_app_metadata as gen

CHINESE = '\u4e2d\u6587\u540d\u79f0'
TYPE_STRING, TYPE_INT_HEX, TYPE_INT_BOOLEAN = 3, 17, 18
NO_ENTRY = 0xFFFFFFFF


def pool_utf8(items):
    offsets, data = [], b''
    for item in items:
        offsets.append(len(data))
        raw = item.encode('utf-8')
        # AXML UTF-8 pool: UTF-16 length first, then the UTF-8 byte length.
        data += struct.pack('<H', len(item))
        if len(raw) < 0x80:
            data += bytes([len(raw)])
        else:
            data += bytes([(len(raw) >> 8) | 0x80, len(raw) & 0xFF])
        data += raw + b'\x00'
    header = 28
    start = header + 4 * len(items)
    body = b''.join(struct.pack('<I', o) for o in offsets) + data
    return (struct.pack('<HHI', 1, header, header + len(body))
            + struct.pack('<IIIII', len(items), 0, 0x100, start, 0) + body)


def start_element(name_idx, attrs):
    body = b''
    for name, raw, vtype, data in attrs:
        body += struct.pack('<IIIHBBI', 0xFFFFFFFF, name, raw, 8, 0, vtype, data)
    ext = (struct.pack('<II', 0xFFFFFFFF, name_idx)
           + struct.pack('<HHHHHH', 20, 20, len(attrs), 0, 0, 0) + body)
    return struct.pack('<HHIII', 0x0102, 36, 16 + len(ext), 0, 0) + ext


def end_element(name_idx):
    return (struct.pack('<HHIII', 0x0103, 16, 24, 0, 0)
            + struct.pack('<II', 0xFFFFFFFF, name_idx))


def build_manifest():
    items = ['manifest', 'application', 'instrumentation', 'meta-data', 'name', 'value',
             'unity.builder', '00123', 'FLAG_ON', 'HEX16', 'title', CHINESE]
    ix = {name: i for i, name in enumerate(items)}

    def meta(parent, attr_name_idx, raw, vtype, data):
        return (start_element(ix[parent], [])
                + start_element(ix['meta-data'],
                                [(ix['name'], attr_name_idx, TYPE_STRING, 0),
                                 (ix['value'], raw, vtype, data)])
                + end_element(ix['meta-data'])
                + end_element(ix[parent]))

    out = struct.pack('<HHI', 3, 8, 8) + pool_utf8(items)
    out += start_element(ix['manifest'], [])
    out += meta('application', ix['unity.builder'], ix['00123'], TYPE_STRING, 0)
    out += meta('application', ix['FLAG_ON'], NO_ENTRY, TYPE_INT_BOOLEAN, 0xFFFFFFFF)
    out += meta('application', ix['HEX16'], NO_ENTRY, TYPE_INT_HEX, 0x10)
    out += meta('application', ix['title'], ix[CHINESE], TYPE_STRING, 0)
    out += meta('instrumentation', ix['unity.builder'], ix['00123'], TYPE_STRING, 0)
    out += end_element(ix['manifest'])
    return out


def main():
    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, 'w') as archive:
        archive.writestr('AndroidManifest.xml', build_manifest())
    tmp = pathlib.Path(__file__).with_name('_synthetic.apk')
    tmp.write_bytes(buffer.getvalue())
    try:
        rows, _typed = gen.render(gen.read_meta_data(tmp))
    finally:
        tmp.unlink()
    text = '\n'.join(rows)
    checks = [
        ('numeric string keeps its form', ',3,"00123",0u}' in text),
        ('boolean true becomes 1', '{"FLAG_ON",18,NULL,1u},' in text),
        ('hex 0x10 becomes 16', '{"HEX16",17,NULL,16u},' in text),
        ('chinese value intact', CHINESE in text),
        ('instrumentation meta-data ignored', text.count('"00123"') == 1),
    ]
    failed = [name for name, ok in checks if not ok]
    for name, ok in checks:
        print(('PASS ' if ok else 'FAIL ') + name)
    if failed:
        print('\nrows:\n' + text)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
