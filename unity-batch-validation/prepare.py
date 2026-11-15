"""Extract actual ELF undefined symbols and compile a platform visibility probe."""
import json
import struct
import subprocess
import zipfile
from pathlib import Path

folder = Path(__file__).resolve().parent
root = folder.parent
rows = []
libraries = []
with zipfile.ZipFile(next(root.glob('BanGDream*.apk'))) as apk:
    for path in sorted(apk.namelist()):
        if not (path.startswith('lib/arm64-v8a/') and path.endswith('.so')):
            continue
        lib = path.rsplit('/', 1)[1]
        libraries.append(lib)
        data = apk.read(path)
        off = struct.unpack_from('<Q', data, 40)[0]
        stride, count = struct.unpack_from('<HH', data, 58)
        sections = [struct.unpack_from('<IIQQQQIIQQ', data, off + i * stride) for i in range(count)]
        for section in sections:
            if section[1] != 11:
                continue
            strings = sections[section[6]]
            for pos in range(section[4], section[4] + section[5], section[9] or 24):
                name, info, _, index, _, _ = struct.unpack_from('<IBBHQQ', data, pos)
                if name and index == 0 and info >> 4 in (1, 2):
                    start = strings[4] + name
                    symbol = data[start:data.index(b'\0', start)].decode('utf-8')
                    rows.append({'lib': lib, 'name': symbol, 'weak': info >> 4 == 2})
text = 'struct Symbol {const char* lib; const char* name; int weak;};\nstatic const struct Symbol symbols[]={\n'
text += ',\n'.join('{' + json.dumps(r['lib']) + ',' + json.dumps(r['name']) + ',' + str(int(r['weak'])) + '}' for r in rows)
text += '\n};\nstatic const size_t symbol_count=sizeof(symbols)/sizeof(symbols[0]);\n'
text += 'static const char* game_libs[]={' + ','.join(map(json.dumps, libraries)) + '};\n'
text += 'static const size_t game_lib_count=sizeof(game_libs)/sizeof(game_libs[0]);\n'
(folder / 'symbols.inc').write_text(text, encoding='utf-8')
(folder / 'imports.json').write_text(json.dumps(rows, indent=2), encoding='utf-8')
sdk = Path('C:/Program Files/Huawei/DevEco Studio/sdk/default/openharmony/native')
subprocess.run([str(sdk/'llvm/bin/clang.exe'), '--target=aarch64-linux-ohos', '--sysroot='+str(sdk/'sysroot'),
                '-fPIC', '-shared', '-O2', '-Wall', '-Wextra', '-Wl,--no-undefined',
                str(folder/'unity_batch.c'), '-ldl', '-o', str(folder/'libunitybatch.so')], check=True)
print('Compiled platform visibility probe:', len(libraries), 'game libraries,', len(rows), 'undefined symbols')
