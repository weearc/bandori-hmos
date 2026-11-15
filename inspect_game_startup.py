"""Read DEX class definitions and selected method call sites; no APK execution."""
import json
import re
import struct
import zipfile
from pathlib import Path


class Dex:
    def __init__(self, data):
        self.data = data
        assert data[:4] == b'dex\n'
        count, offset = self.pair(56)
        self.strings = []
        for i in range(count):
            pos = self.u32(offset + 4 * i)
            _, pos = self.uleb(pos)
            end = data.index(b'\0', pos)
            self.strings.append(data[pos:end].decode('utf-8', 'replace'))
        count, offset = self.pair(64)
        self.types = [self.strings[self.u32(offset + 4 * i)] for i in range(count)]
        count, offset = self.pair(72)
        self.protos = []
        for i in range(count):
            _, ret, params = struct.unpack_from('<III', data, offset + i * 12)
            args = [] if not params else [self.types[struct.unpack_from('<H', data, params + 4 + j * 2)[0]]
                                         for j in range(self.u32(params))]
            self.protos.append('(' + ''.join(args) + ')' + self.types[ret])
        count, offset = self.pair(88)
        self.methods = []
        for i in range(count):
            klass, proto, name = struct.unpack_from('<HHI', data, offset + i * 8)
            self.methods.append(self.types[klass] + '->' + self.strings[name] + self.protos[proto])
        self.classes = {}
        count, offset = self.pair(96)
        for i in range(count):
            klass, flags, parent, _, _, _, classdata, _ = struct.unpack_from('<8I', data, offset + i * 32)
            rec = {'super': self.types[parent] if parent != 0xffffffff else None,
                   'access': flags, 'methods': []}
            if classdata:
                counts = []
                pos = classdata
                for _ in range(4):
                    value, pos = self.uleb(pos)
                    counts.append(value)
                for _ in range(counts[0] + counts[1]):
                    _, pos = self.uleb(pos)
                    _, pos = self.uleb(pos)
                for group in counts[2:]:
                    methodid = 0
                    for _ in range(group):
                        delta, pos = self.uleb(pos)
                        access, pos = self.uleb(pos)
                        code, pos = self.uleb(pos)
                        methodid += delta
                        rec['methods'].append({'name': self.methods[methodid], 'access': access, 'code': code})
            self.classes[self.types[klass]] = rec

    def u32(self, pos):
        return struct.unpack_from('<I', self.data, pos)[0]

    def pair(self, pos):
        return struct.unpack_from('<II', self.data, pos)

    def uleb(self, pos):
        result, shift = 0, 0
        while True:
            value = self.data[pos]
            pos += 1
            result |= (value & 127) << shift
            if not value & 128:
                return result, pos
            shift += 7
            if shift > 35:
                raise ValueError('Invalid ULEB128')

    def calls(self, offset):
        if not offset:
            return {'calls': [], 'strings': []}
        count = self.u32(offset + 12)
        units = struct.unpack_from('<' + 'H' * count, self.data, offset + 16)
        calls, strings, pos = [], [], 0
        widths = [0] * 256
        for a, b, width in [(0, 1, 1), (1, 2, 1), (2, 3, 2), (3, 4, 3),
                            (4, 5, 1), (5, 6, 2), (6, 7, 3), (7, 8, 1), (8, 9, 2), (9, 10, 3),
                            (10, 19, 1), (19, 20, 2), (20, 21, 3), (21, 23, 2), (23, 24, 3),
                            (24, 25, 5), (25, 27, 2), (27, 28, 3), (28, 29, 2), (29, 31, 1),
                            (31, 33, 2), (33, 34, 1), (34, 36, 2), (36, 39, 3), (39, 41, 1),
                            (41, 42, 2), (42, 45, 3), (45, 62, 2), (68, 110, 2),
                            (110, 115, 3), (116, 121, 3), (123, 144, 1), (144, 176, 2),
                            (176, 208, 1), (208, 227, 2), (250, 252, 4), (252, 254, 3), (254, 256, 2)]:
            widths[a:b] = [width] * (b - a)
        while pos < count:
            word = units[pos]
            op = word & 255
            width = widths[op]
            if op == 0 and word:
                if word == 0x100:
                    width = 4 + 2 * units[pos + 1]
                elif word == 0x200:
                    width = 2 + 4 * units[pos + 1]
                elif word == 0x300:
                    size = units[pos + 2] | units[pos + 3] << 16
                    width = 4 + (units[pos + 1] * size + 1) // 2
                else:
                    raise ValueError(f'Unknown payload {word:x}')
            if not width or pos + width > count:
                raise ValueError(f'Invalid opcode/width {op:x} at {pos}')
            if op in (*range(0x6e, 0x73), *range(0x74, 0x79), 0xfa, 0xfb):
                calls.append({'unit_offset': pos, 'opcode': hex(op), 'target': self.methods[units[pos + 1]]})
            if op in (0x1a, 0x1b):
                index = units[pos + 1] | (units[pos + 2] << 16 if op == 0x1b else 0)
                strings.append(self.strings[index])
            pos += width
        return {'calls': calls, 'strings': strings}


def main():
    apk = next(Path('.').glob('*.apk'))
    result = {'apk': apk.name, 'dex': {}, 'classes': {}}
    prefixes = ('Lcom/bilibili/star/bili/', 'Lcom/unity3d/player/', 'Ljp/co/craftegg/band/lib/',
                'Lcom/gsc/pub/GSCPubCommon;', 'Lcom/gsc/pub/utils/b;')
    with zipfile.ZipFile(apk) as archive:
        for name in archive.namelist():
            if not re.fullmatch(r'classes\d*\.dex', name):
                continue
            dex = Dex(archive.read(name))
            result['dex'][name] = {'classes': len(dex.classes), 'methods': len(dex.methods),
                                   'sdk_class_samples': [x for x in dex.classes if any(s in x.lower() for s in
                                      ['bili', 'criware', 'criatom', 'unityplayer', 'gscloud'])][:180]}
            for klass, rec in dex.classes.items():
                if not klass.startswith(prefixes):
                    continue
                item = {'dex': name, 'super': rec['super'], 'access': rec['access'], 'methods': []}
                for method in rec['methods']:
                    entry = dict(method)
                    try:
                        entry.update(dex.calls(method['code']))
                    except (ValueError, IndexError, struct.error) as error:
                        entry['decode_error'] = str(error)
                    item['methods'].append(entry)
                result['classes'][klass] = item
    Path('game-startup-analysis.json').write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
    print('Wrote game-startup-analysis.json;', len(result['classes']), 'selected classes')
    for klass, rec in result['classes'].items():
        if klass.startswith('Lcom/bilibili/star/bili/') and '$' not in klass:
            print('\nCLASS', klass, 'SUPER', rec['super'])
            for method in rec['methods']:
                print(method['name'], 'NATIVE' if method['access'] & 256 else '')
                print('  CALLS', [x['target'] for x in method.get('calls', [])])
                print('  STRINGS', method.get('strings', []))
                if 'decode_error' in method:
                    print('  ERROR', method['decode_error'])
    print('Decoder errors:', [(k, m['name'], m['decode_error']) for k, c in result['classes'].items()
                              for m in c['methods'] if 'decode_error' in m])


if __name__ == '__main__':
    main()
