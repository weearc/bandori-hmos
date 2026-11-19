"""Read-only APK inventory using Python's standard library; does not execute code."""
import argparse
import hashlib
import json
import re
import struct
import zipfile
from pathlib import Path


def elf_inventory(data):
    if data[:6] != b'\x7fELF\x02\x01':
        raise ValueError('Expected little-endian ELF64')
    shoff = struct.unpack_from('<Q', data, 40)[0]
    entsize, count = struct.unpack_from('<HH', data, 58)
    sections = [struct.unpack_from('<IIQQQQIIQQ', data, shoff + i * entsize)
                for i in range(count)]

    def string_at(section, offset):
        start = section[4] + offset
        return data[start:data.index(b'\0', start)].decode('utf-8', 'replace')

    needed, imports, exports = [], [], []
    for section in sections:
        if section[1] == 6:  # SHT_DYNAMIC
            strings = sections[section[6]]
            for pos in range(section[4], section[4] + section[5], section[9] or 16):
                tag, value = struct.unpack_from('<qQ', data, pos)
                if tag == 1:
                    needed.append(string_at(strings, value))
        if section[1] == 11:  # SHT_DYNSYM
            strings = sections[section[6]]
            for pos in range(section[4], section[4] + section[5], section[9] or 24):
                name, info, _, index, _, _ = struct.unpack_from('<IBBHQQ', data, pos)
                if name and info >> 4 in (1, 2):
                    (imports if index == 0 else exports).append(string_at(strings, name))
    return {'machine': struct.unpack_from('<H', data, 18)[0],
            'needed': sorted(set(needed)), 'imports': sorted(set(imports)),
            'jni_exports': sorted(x for x in set(exports)
                                  if x.startswith(('Java_', 'JNI_')))}


def manifest_inventory(data):
    """Decode Android binary XML string pool and start-tag attributes."""
    strings, tags = [], []
    offset = struct.unpack_from('<H', data, 2)[0]
    while offset + 8 <= len(data):
        kind, header, size = struct.unpack_from('<HHI', data, offset)
        if size < 8 or offset + size > len(data):
            raise ValueError('Invalid binary XML chunk')
        if kind == 1:
            count, _, flags, start, _ = struct.unpack_from('<IIIII', data, offset + 8)
            utf8 = bool(flags & 256)

            def length(pos):
                if utf8:
                    value = data[pos]
                    return (((value & 127) << 8) | data[pos + 1], pos + 2) if value & 128 else (value, pos + 1)
                value = struct.unpack_from('<H', data, pos)[0]
                return (((value & 32767) << 16) | struct.unpack_from('<H', data, pos + 2)[0], pos + 4) if value & 32768 else (value, pos + 2)

            for i in range(count):
                relative = struct.unpack_from('<I', data, offset + header + i * 4)[0]
                n, pos = length(offset + start + relative)
                if utf8:
                    n, pos = length(pos)
                strings.append(data[pos:pos + n * (1 if utf8 else 2)].decode('utf-8' if utf8 else 'utf-16-le', 'replace'))
        elif kind == 0x102:
            ext = offset + header
            _, name, attrstart, attrsize, attrcount = struct.unpack_from('<IIHHH', data, ext)
            attrs = {}
            for i in range(attrcount):
                pos = ext + attrstart + i * attrsize
                _, key, raw, _, _, datatype, value = struct.unpack_from('<IIIHBBI', data, pos)
                attrs[strings[key]] = (strings[raw] if raw != 0xffffffff else
                                      strings[value] if datatype == 3 else
                                      bool(value) if datatype == 18 else value)
            tags.append({'tag': strings[name], 'attributes': attrs})
        offset += size
    return tags


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('apk', type=Path)
    parser.add_argument('--output', type=Path, default=Path('apk-analysis.json'))
    args = parser.parse_args()
    with args.apk.open('rb') as stream:
        digest = hashlib.file_digest(stream, 'sha256').hexdigest()
    with zipfile.ZipFile(args.apk) as archive:
        names = archive.namelist()
        libs = {}
        for item in archive.infolist():
            if item.filename.startswith('lib/arm64-v8a/') and item.filename.endswith('.so'):
                libs[item.filename.rsplit('/', 1)[1]] = {
                    'size': item.file_size, **elf_inventory(archive.read(item))}
        gm = archive.read('assets/bin/Data/globalgamemanagers')
        versions = sorted(set(x.decode() for x in re.findall(rb'20[12][0-9]\.[0-9]+\.[0-9]+[a-z][0-9]+(?:c[0-9]+)?', gm)))
        result = {'apk': args.apk.name, 'sha256': digest, 'unity_versions': versions,
                  'architectures': sorted({x.split('/')[1] for x in names if x.startswith('lib/')}),
                  'manifest': manifest_inventory(archive.read('AndroidManifest.xml')),
                  'dex_files': [x for x in names if re.fullmatch(r'classes\d*\.dex', x)],
                  'arm64_libraries': libs,
                  'note': 'Static inventory only. Imports do not cover dlsym, JNI method lookup, or runtime-loaded dependencies.'}
    args.output.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
    print(f'Wrote {args.output}; Unity {versions}; {len(libs)} ARM64 libraries')
    for name, lib in libs.items():
        print(f"{name}: {len(lib['imports'])} imports; needs {', '.join(lib['needed'])}")
    for tag in result['manifest']:
        if tag['tag'] in ('manifest', 'uses-sdk', 'application', 'activity'):
            print(tag)


if __name__ == '__main__':
    main()
