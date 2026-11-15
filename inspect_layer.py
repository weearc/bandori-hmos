"""Statically inventory the supplied nested HAP; never execute its contents."""
import hashlib
import io
import json
import struct
import zipfile
from pathlib import Path
from inspect_apk import elf_inventory


def exports_of(data):
    offset = struct.unpack_from('<Q', data, 40)[0]
    stride, count = struct.unpack_from('<HH', data, 58)
    sections = [struct.unpack_from('<IIQQQQIIQQ', data, offset + i * stride)
                for i in range(count)]
    result = {}
    for section in sections:
        if section[1] != 11:
            continue
        strings = sections[section[6]]
        for pos in range(section[4], section[4] + section[5], section[9] or 24):
            name, info, other, index, address, size = struct.unpack_from('<IBBHQQ', data, pos)
            if not name or not index or info >> 4 not in (1, 2) or other & 3 not in (0, 3):
                continue
            start = strings[4] + name
            symbol = data[start:data.index(b'\0', start)].decode('utf-8', 'replace')
            record = {'size': size, 'type': info & 15}
            if info & 15 == 2 and index < len(sections):
                target = sections[index]
                filepos = target[4] + address - target[3]
                if 0 <= filepos < len(data):
                    record['first_bytes'] = data[filepos:filepos + min(size, 16)].hex()
            result[symbol] = record
    return result


def main():
    path = next(Path('.').glob('*.zip'))
    with path.open('rb') as stream:
        digest = hashlib.file_digest(stream, 'sha256').hexdigest()
    with zipfile.ZipFile(path) as outer:
        hapname = next(n for n in outer.namelist() if n.endswith('.hap'))
        hapbytes = outer.read(hapname)
        with zipfile.ZipFile(io.BytesIO(hapbytes)) as hap:
            result = {'archive': path.name, 'archive_sha256': digest,
                      'hap': hapname, 'hap_sha256': hashlib.sha256(hapbytes).hexdigest(),
                      'module': json.loads(hap.read('module.json')),
                      'files': [{'name': x.filename, 'size': x.file_size} for x in hap.infolist()],
                      'libraries': {}}
            for item in hap.infolist():
                if item.filename.endswith('.so'):
                    data = hap.read(item)
                    result['libraries'][Path(item.filename).name] = {
                        'size': len(data), **elf_inventory(data), 'exports': exports_of(data)}
            result['embedded_library_hash_checks'] = {}
            for name in hap.namelist():
                if name.endswith('.sha256'):
                    target = name.split('.so.')[0] + '.so'
                    expected = hap.read(name).decode().strip().lower()
                    actual = hashlib.sha256(hap.read(target)).hexdigest()
                    result['embedded_library_hash_checks'][name] = {
                        'matches': expected == actual, 'expected': expected, 'actual': actual}
        scripts = [n for n in outer.namelist() if n.endswith('.ps1')]
        result['script_names'] = scripts
    game = json.loads(Path('apk-analysis.json').read_text(encoding='utf-8'))
    supplied = set().union(*(set(x['exports']) for x in result['libraries'].values()))
    result['selected_game_imports_absent_from_package_exports'] = {
        name: [s for s in lib['imports'] if s.startswith(
            ('AAsset', 'ANativeWindow', 'AHardwareBuffer', 'ALooper', 'ASensor', 'SL_', 'slCreate'))
            and s not in supplied]
        for name, lib in game['arm64_libraries'].items()
        if name in ('libunity.so', 'libcri_ware_unity.so')}
    result['limitations'] = [
        'Export names do not prove ABI or behavioral compatibility.',
        'Missing package exports may be provided dynamically or by the OS; this is not a full link check.',
        'No application execution, installation, signature verification, or network behavior audit performed.']
    Path('layer-analysis.json').write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
    print('Wrote layer-analysis.json')
    print('HAP SHA256:', result['hap_sha256'])
    print('Library hashes:', result['embedded_library_hash_checks'])
    for name, symbols in result['selected_game_imports_absent_from_package_exports'].items():
        print(name, 'selected absent symbols:', ', '.join(symbols))
    print('Media function sizes:', {n: x['size'] for n, x in
          result['libraries']['libmediandk.so']['exports'].items() if x['type'] == 2})


if __name__ == '__main__':
    main()
