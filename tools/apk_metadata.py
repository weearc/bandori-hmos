"""Small Android binary XML version reader; no aapt/Android SDK dependency."""
from pathlib import Path
import hashlib
import os
import struct
import zipfile

ROOT = Path(__file__).resolve().parents[1]

def apk_path(explicit=None):
    """Resolve the APK: explicit argument, then BG_APK, then the bundled rawfile copy."""
    if explicit:
        return Path(explicit).resolve()
    return Path(os.environ.get('BG_APK',
        str(ROOT / 'native-probe/entry/src/main/resources/rawfile/sdkdex/base.apk'))).resolve()

def sampled_key(path):
    size = path.stat().st_size
    with path.open('rb') as f:
        first = f.read(min(size, 1024 * 1024))
        f.seek(max(0, size - 65536))
        last = f.read(min(size, 65536))
    return hashlib.sha256(first + last + struct.pack('<Q', size)).hexdigest()

def version(path):
    with zipfile.ZipFile(path) as z:
        b = z.read('AndroidManifest.xml')
    u16 = lambda p: struct.unpack_from('<H', b, p)[0]
    u32 = lambda p: struct.unpack_from('<I', b, p)[0]
    strings = []
    p = u16(2)
    while p + 8 <= len(b):
        kind, header, size = struct.unpack_from('<HHI', b, p)
        if size < header or size < 8 or p + size > len(b):
            raise ValueError('invalid AndroidManifest XML chunk')
        if kind == 1:
            count, flags, start = u32(p+8), u32(p+16), u32(p+20)
            def length(q, wide=False):
                n = u16(q) if wide else b[q]
                step, mask = (2, 0x8000) if wide else (1, 0x80)
                return (((n & (mask-1)) << (16 if wide else 8)) | (u16(q+step) if wide else b[q+step]), q+step*2) if n & mask else (n, q+step)
            for i in range(count):
                q = p + start + u32(p+header+4*i)
                if flags & 256:
                    _, q = length(q)
                    n, q = length(q)
                    strings.append(b[q:q+n].decode('utf-8'))
                else:
                    n, q = length(q, True)
                    strings.append(b[q:q+n*2].decode('utf-16le'))
        if kind == 0x102 and strings[u32(p+20)] == 'manifest':
            attrs = p+16+u16(p+24)
            result = {}
            for i in range(u16(p+28)):
                q = attrs + i*u16(p+26)
                name, raw, typ, value = strings[u32(q+4)], u32(q+8), b[q+15], u32(q+16)
                if name in ('versionName', 'versionCode'):
                    result[name] = strings[raw] if raw != 0xffffffff else strings[value] if typ == 3 else value
            if not isinstance(result.get('versionName'), str) or 'versionCode' not in result:
                raise ValueError('APK manifest version is absent or resource-referenced')
            result['versionCode'] = int(result['versionCode'])
            return result
        p += size
    raise ValueError('APK manifest element missing')

if __name__ == '__main__':
    import json
    print(json.dumps(dict(path=str(apk_path()), apk_key=sampled_key(apk_path()), **version(apk_path()))))
