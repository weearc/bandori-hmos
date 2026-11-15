"""Separate measured platform misses from weak imports and APK dependencies."""
import json
import sys
import zipfile
from pathlib import Path
folder = Path(__file__).resolve().parent
root = folder.parent
sys.path.insert(0, str(root))
from inspect_apk import elf_inventory
from inspect_layer import exports_of

inventory = {}
with zipfile.ZipFile(next(root.glob('BanGDream*.apk'))) as apk:
    for name in apk.namelist():
        if name.startswith('lib/arm64-v8a/') and name.endswith('.so'):
            data = apk.read(name)
            inventory[name.rsplit('/', 1)[1]] = {
                'needed': elf_inventory(data)['needed'], 'exports': set(exports_of(data))}

def dependency_exports(lib, visited):
    if lib in visited or lib not in inventory:
        return set()
    visited.add(lib)
    item = inventory[lib]
    return item['exports'] | set().union(*(dependency_exports(d, visited) for d in item['needed']))

rows = json.loads((folder/'imports.json').read_text())
weak = sum(row['weak'] for row in rows)
misses = []
for line in (root/'batch-validation/unity-report.txt').read_text().splitlines():
    if not line.startswith('MISSING_SYMBOL '):
        continue
    _, lib, symbol = line.split(' ', 2)
    providers = [dep for dep in inventory[lib]['needed'] if symbol in dependency_exports(dep, set())]
    misses.append({'library': lib, 'symbol': symbol, 'apk_dependency_candidates': providers,
                   'classification': 'APK dependency name candidate' if providers else 'no APK dependency or probed platform name candidate'})
result = {'weak_imports_not_probed': weak, 'strong_not_visible_in_probed_platform_handles': len(misses),
          'apk_dependency_name_candidates': sum(bool(m['apk_dependency_candidates']) for m in misses),
          'remaining_strong_names': sum(not m['apk_dependency_candidates'] for m in misses),
          'limitations': 'Name candidates only, not symbol-version/ABI/namespace or actual game loading verification.',
          'details': misses}
(root/'batch-validation/unity-classified.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
print(json.dumps({k:v for k,v in result.items() if k != 'details'}, indent=2))
for lib in ['libunity.so', 'libil2cpp.so', 'libcri_ware_unity.so', 'libmain.so']:
    subset = [m for m in misses if m['library'] == lib and not m['apk_dependency_candidates']]
    print(lib, len(subset), 'remaining strong names')
