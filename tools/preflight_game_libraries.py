"""Compare APK dependency names against packaged libraries and SDK link stubs.

This performs no dynamic loading. Symbol-name matches do not establish ABI,
symbol-version, namespace, weak-import, or behavioral compatibility.
"""
import csv
import io
import json
import struct
import zipfile
from pathlib import Path
from inspect_apk import elf_inventory
from inspect_layer import exports_of


SDK_LIBS = Path('C:/Program Files/Huawei/DevEco Studio/sdk/default/openharmony/native/sysroot/usr/lib/aarch64-linux-ohos')


def version_requirements(data):
    offset = struct.unpack_from('<Q', data, 40)[0]
    stride, count = struct.unpack_from('<HH', data, 58)
    sections = [struct.unpack_from('<IIQQQQIIQQ', data, offset + i * stride) for i in range(count)]
    result = []
    for section in sections:
        if section[1] != 0x6ffffffe:
            continue
        strings = sections[section[6]]

        def name_at(relative):
            start = strings[4] + relative
            return data[start:data.index(b'\0', start)].decode('utf-8', 'replace')

        pos = section[4]
        while pos < section[4] + section[5]:
            _, number, file, aux, following = struct.unpack_from('<HHIII', data, pos)
            a = pos + aux
            names = []
            for _ in range(number):
                _, _, _, name, next_aux = struct.unpack_from('<IHHII', data, a)
                names.append(name_at(name))
                if not next_aux:
                    break
                a += next_aux
            result.append({'library': name_at(file), 'versions': names})
            if not following:
                break
            pos += following
    return result


def main():
    game = {}
    with zipfile.ZipFile(next(Path('.').glob('*.apk'))) as apk:
        for name in apk.namelist():
            if name.startswith('lib/arm64-v8a/') and name.endswith('.so'):
                data = apk.read(name)
                game[Path(name).name] = {**elf_inventory(data),
                    'exports': set(exports_of(data)), 'version_requirements': version_requirements(data)}
    system = {}
    needed = {name for lib in game.values() for name in lib['needed'] if name not in game}
    for name in sorted(needed):
        path = SDK_LIBS / name
        item = {'present_in_sdk': path.is_file(), 'exports': set(), 'note': ''}
        if path.is_file():
            data = path.read_bytes()
            if data[:4] == b'\x7fELF':
                item['exports'] = set(exports_of(data))
                item['needed'] = elf_inventory(data)['needed']
            else:
                item['note'] = 'SDK linker script or non-ELF; not resolved by this inventory'
        system[name] = item

    def closure(name, visited):
        if name in visited:
            return set()
        visited.add(name)
        lib = game.get(name) or system.get(name)
        if not lib:
            return set()
        symbols = set(lib['exports'])
        for dep in lib.get('needed', []):
            symbols |= closure(dep, visited)
        return symbols

    result = {'sdk_lib_directory': str(SDK_LIBS),
              'system_dependency_names': {k: {'present_in_sdk': v['present_in_sdk'],
                 'export_count': len(v['exports']), 'note': v['note']} for k,v in system.items()},
              'libraries': {},
              'limitations': [
                  'Static name-only preflight, not dlopen, JNI execution, or a runtime link test.',
                  'SDK link stubs do not prove device namespace visibility or behavior.',
                  'Matching symbol names do not prove Bionic/musl ABI, structure, TLS, or symbol-version compatibility.',
                  'Unmatched imports may include optional weak symbols; dlsym/JNI dependencies are not covered.']}
    rows = []
    for name, lib in game.items():
        provided = set()
        for dep in lib['needed']:
            provided |= closure(dep, set())
        unmatched = [s for s in lib['imports'] if s not in provided]
        result['libraries'][name] = {'needed': lib['needed'],
            'dependency_names_absent_from_apk_and_sdk': [n for n in lib['needed']
                 if n not in game and not system[n]['present_in_sdk']],
            'imports': len(lib['imports']), 'name_only_matches': len(lib['imports']) - len(unmatched),
            'name_only_unmatched': unmatched, 'version_requirements': lib['version_requirements']}
        for symbol in lib['imports']:
            rows.append([name, symbol, 'name match only' if symbol in provided else 'no candidate in examined dependency closure'])
    out = Path('compat-spec')
    out.mkdir(exist_ok=True)
    (out/'game-library-preflight.json').write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
    with (out/'game-library-symbols.csv').open('w', encoding='utf-8-sig', newline='') as stream:
        writer = csv.writer(stream)
        writer.writerow(['game_library', 'import', 'static_observation'])
        writer.writerows(rows)
    for name in ['libmain.so', 'libunity.so', 'libil2cpp.so', 'libcri_ware_unity.so', 'libbili.so']:
        r = result['libraries'][name]
        print(name, 'absent dependency names:', r['dependency_names_absent_from_apk_and_sdk'],
              'unmatched symbol names:', len(r['name_only_unmatched']), 'of', r['imports'],
              'required versions:', r['version_requirements'])
    print('Wrote compat-spec/game-library-preflight.json and game-library-symbols.csv')


if __name__ == '__main__':
    main()
