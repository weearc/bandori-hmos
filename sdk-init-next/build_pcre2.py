"""Build the pinned PCRE2 UTF-16 C engine for the OHOS candidate (no JIT)."""
from pathlib import Path
import concurrent.futures
import hashlib
import json
import shutil
import subprocess

root = Path(__file__).resolve().parent
vendor = root / 'vendor/pcre2-10.45'
out = root / 'vendor/build-pcre2-16'
out.mkdir(exist_ok=True)
sdk = Path('C:/Program Files/Huawei/DevEco Studio/sdk/default/openharmony/native')
for original, name in [('config.h.generic', 'config.h'), ('pcre2.h.generic', 'pcre2.h'),
                       ('pcre2_chartables.c.dist', 'pcre2_chartables.c')]:
    shutil.copy2(vendor / 'src' / original, out / name)
cmake = (vendor / 'CMakeLists.txt').read_text()
section = cmake.split('  PCRE2_SOURCES\n', 1)[1].split('\n)', 1)[0]
sources = [vendor / line.strip() for line in section.splitlines()
           if line.strip().startswith('src/')]
sources.append(out / 'pcre2_chartables.c')
flags = [str(sdk / 'llvm/bin/clang.exe'), '--target=aarch64-linux-ohos',
         '--sysroot=' + str(sdk / 'sysroot'), '-std=gnu11', '-O2', '-fPIC',
         '-DHAVE_CONFIG_H', '-DPCRE2_CODE_UNIT_WIDTH=16', '-DSUPPORT_PCRE2_16',
         '-DSUPPORT_UNICODE', '-DHAVE_MEMMOVE', '-DHAVE_STDLIB_H', '-DHAVE_STDINT_H',
         '-DHAVE_STRING_H', '-DHAVE_LIMITS_H', '-DPCRE2_STATIC', '-I' + str(out),
         '-I' + str(vendor / 'src')]

def compile_one(source):
    obj = out / (source.stem + '.o')
    subprocess.run(flags + ['-c', str(source), '-o', str(obj)], check=True)
    return obj

with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
    objects = list(pool.map(compile_one, sources))
archive = out / 'libpcre2-16.a'
subprocess.run([str(sdk / 'llvm/bin/llvm-ar.exe'), 'rcs', str(archive),
                *map(str, objects)], check=True)
(out / 'build.json').write_text(json.dumps({'flags': flags,
    'source': 'pcre2-10.45', 'sha256': hashlib.sha256(archive.read_bytes()).hexdigest()},
    indent=2) + '\n')
print(archive)
