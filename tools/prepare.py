"""Create a reviewable MPL-2.0 derivative of the pinned interpreter core."""
from pathlib import Path
import json, hashlib, zipfile
root = Path(__file__).resolve().parent
upstream = root.parent / 'research/lunaria-source'
out = root / 'generated'
manifest = {'upstream_commit': '8ed4b75368153f2b9d1174c986e50c8fe87aee59', 'license': 'MPL-2.0', 'files': {}}
for name in ['dex.h', 'dex.c', 'dvm.h', 'dvm_internal.h', 'dvm.c']:
    original = (upstream/name).read_bytes()
    src = original.decode('utf-8')
    if name == 'dvm.c':
        for header in ['dvm/platform_classes.h', 'luna_boot.h', 'luna_overlay.h', 'arm_exec.h', 'arm.h', 'lunaria_os.h']:
            src = src.replace('#include "'+header+'"', '')
        src = src.replace('#include <errno.h>', '#include "portable.h"\n#include <errno.h>')
        # Upstream silently returns zero for unsupported methods despite the
        # public hook comment promising NoSuchMethodError. Diagnosis must fail.
        old = '''   /* Returning zero is what the stub layer did before this module existed.
    * Throwing here would abort app code that only wanted a no-op logger. */
   return true;'''
        assert old in src
        src = src.replace(old, '''   dvm__throw(vm, "java/lang/NoSuchMethodError", "%s.%s%s", cls->name, name, sig);
   return false;''')
        src = src.replace('method not implemented, stub returns null/0:', 'unsupported method, strict runner throws:')
        needle = '         if (f && o->slots && f->slot < (o->cls ? o->cls->islots : 0))'
        assert src.count(needle) == 2
        src = src.replace(needle, '''         if (!f) {
            sdk_trace_unresolved_field(fc ? fc->name : NULL, fn, ft);
            dvm__throw(vm, "java/lang/NoSuchFieldError", "%s.%s:%s", fc ? fc->name : "?", fn ? fn : "?", ft ? ft : "?");
            goto exception;
         }
''' + needle)
        needle = '         if (vm->trace && trace_this && op == 0x69)'
        assert needle in src
        src = src.replace(needle, '''         if (!f) {
            sdk_trace_unresolved_field(fc ? fc->name : NULL, fn, ft);
            dvm__throw(vm, "java/lang/NoSuchFieldError", "%s.%s:%s", fc ? fc->name : "?", fn ? fn : "?", ft ? ft : "?");
            goto exception;
         }
''' + needle)
        src = src.replace('#include <time.h>', '#include <time.h>\n#define fprintf sdk_trace_fprintf\n#define fputc sdk_trace_fputc')
    (out/'dvm'/name).write_text(src, encoding='utf-8')
    manifest['files'][name] = hashlib.sha256(original).hexdigest()
(out/'LICENSE').write_bytes((upstream/'LICENSE').read_bytes())
(out/'upstream.json').write_text(json.dumps(manifest, indent=2)+'\n')
apk = next(root.parent.glob('BanGDream*.apk'))
with zipfile.ZipFile(apk) as z:
    dex_dir = root/'dex'
    dex_dir.mkdir(exist_ok=True)
    for n in z.namelist():
        if n.startswith('classes') and n.endswith('.dex') and '/' not in n:
            (dex_dir/n).write_bytes(z.read(n))
            print(n, (dex_dir/n).stat().st_size)
from generate_guest_profile import generate
generate()
from extract_collections import generate as extract_collections
extract_collections()
