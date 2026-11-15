"""Read-only Dalvik disassembler for the original APK DEX files.

Decodes class definitions, fields, methods, instructions, branch targets,
switch payloads and try/catch handlers. Nothing is executed or modified.

Examples:
  python dex_disasm.py --class Lcom/http/lib/ssl/SSLUtils;
  python dex_disasm.py --class Lcom/http/lib/ssl/SSLUtils; --method getSslSocketFactoryBase
  python dex_disasm.py --find SSLUtils
  python dex_disasm.py --callers "Ljavax/net/ssl/TrustManagerFactory;->getDefaultAlgorithm"
"""
import argparse
import struct
import sys
from pathlib import Path

DEFAULT_DEX = [Path(__file__).resolve().parent / 'sdk-runtime-spike/dex' / name
               for name in ('classes.dex', 'classes2.dex', 'classes3.dex')]

# opcode -> (mnemonic, format); unused opcodes are absent.
OPS = {}


def _ops(start, fmt, names):
    for i, name in enumerate(names.split()):
        OPS[start + i] = (name, fmt)


_ops(0x00, '10x', 'nop')
_ops(0x01, '12x', 'move')
_ops(0x02, '22x', 'move/from16')
_ops(0x03, '32x', 'move/16')
_ops(0x04, '12x', 'move-wide')
_ops(0x05, '22x', 'move-wide/from16')
_ops(0x06, '32x', 'move-wide/16')
_ops(0x07, '12x', 'move-object')
_ops(0x08, '22x', 'move-object/from16')
_ops(0x09, '32x', 'move-object/16')
_ops(0x0a, '11x', 'move-result move-result-wide move-result-object move-exception')
_ops(0x0e, '10x', 'return-void')
_ops(0x0f, '11x', 'return return-wide return-object')
_ops(0x12, '11n', 'const/4')
_ops(0x13, '21s', 'const/16')
_ops(0x14, '31i', 'const')
_ops(0x15, '21h', 'const/high16')
_ops(0x16, '21s', 'const-wide/16')
_ops(0x17, '31i', 'const-wide/32')
_ops(0x18, '51l', 'const-wide')
_ops(0x19, '21h', 'const-wide/high16')
_ops(0x1a, '21c', 'const-string')
_ops(0x1b, '31c', 'const-string/jumbo')
_ops(0x1c, '21c', 'const-class')
_ops(0x1d, '11x', 'monitor-enter monitor-exit')
_ops(0x1f, '21c', 'check-cast')
_ops(0x20, '22c', 'instance-of')
_ops(0x21, '12x', 'array-length')
_ops(0x22, '21c', 'new-instance')
_ops(0x23, '22c', 'new-array')
_ops(0x24, '35c', 'filled-new-array')
_ops(0x25, '3rc', 'filled-new-array/range')
_ops(0x26, '31t', 'fill-array-data')
_ops(0x27, '11x', 'throw')
_ops(0x28, '10t', 'goto')
_ops(0x29, '20t', 'goto/16')
_ops(0x2a, '30t', 'goto/32')
_ops(0x2b, '31t', 'packed-switch sparse-switch')
_ops(0x2d, '23x', 'cmpl-float cmpg-float cmpl-double cmpg-double cmp-long')
_ops(0x32, '22t', 'if-eq if-ne if-lt if-ge if-gt if-le')
_ops(0x38, '21t', 'if-eqz if-nez if-ltz if-gez if-gtz if-lez')
_ops(0x44, '23x', 'aget aget-wide aget-object aget-boolean aget-byte aget-char aget-short '
                  'aput aput-wide aput-object aput-boolean aput-byte aput-char aput-short')
_ops(0x52, '22c', 'iget iget-wide iget-object iget-boolean iget-byte iget-char iget-short '
                  'iput iput-wide iput-object iput-boolean iput-byte iput-char iput-short')
_ops(0x60, '21c', 'sget sget-wide sget-object sget-boolean sget-byte sget-char sget-short '
                  'sput sput-wide sput-object sput-boolean sput-byte sput-char sput-short')
_ops(0x6e, '35c', 'invoke-virtual invoke-super invoke-direct invoke-static invoke-interface')
_ops(0x74, '3rc', 'invoke-virtual/range invoke-super/range invoke-direct/range '
                  'invoke-static/range invoke-interface/range')
_ops(0x7b, '12x', 'neg-int not-int neg-long not-long neg-float neg-double int-to-long '
                  'int-to-float int-to-double long-to-int long-to-float long-to-double '
                  'float-to-int float-to-long float-to-double double-to-int double-to-long '
                  'double-to-float int-to-byte int-to-char int-to-short')
_ops(0x90, '23x', 'add-int sub-int mul-int div-int rem-int and-int or-int xor-int shl-int '
                  'shr-int ushr-int add-long sub-long mul-long div-long rem-long and-long '
                  'or-long xor-long shl-long shr-long ushr-long add-float sub-float mul-float '
                  'div-float rem-float add-double sub-double mul-double div-double rem-double')
_ops(0xb0, '12x', ' '.join(n + '/2addr' for n in (
    'add-int sub-int mul-int div-int rem-int and-int or-int xor-int shl-int shr-int ushr-int '
    'add-long sub-long mul-long div-long rem-long and-long or-long xor-long shl-long shr-long '
    'ushr-long add-float sub-float mul-float div-float rem-float add-double sub-double '
    'mul-double div-double rem-double').split()))
_ops(0xd0, '22s', 'add-int/lit16 rsub-int mul-int/lit16 div-int/lit16 rem-int/lit16 '
                  'and-int/lit16 or-int/lit16 xor-int/lit16')
_ops(0xd8, '22b', 'add-int/lit8 rsub-int/lit8 mul-int/lit8 div-int/lit8 rem-int/lit8 '
                  'and-int/lit8 or-int/lit8 xor-int/lit8 shl-int/lit8 shr-int/lit8 ushr-int/lit8')
_ops(0xfa, '45cc', 'invoke-polymorphic')
_ops(0xfb, '4rcc', 'invoke-polymorphic/range')
_ops(0xfc, '35c', 'invoke-custom')
_ops(0xfd, '3rc', 'invoke-custom/range')
_ops(0xfe, '21c', 'const-method-handle const-method-type')

WIDTH = {'10x': 1, '12x': 1, '11n': 1, '11x': 1, '10t': 1, '20t': 2, '22x': 2, '21t': 2,
         '21s': 2, '21h': 2, '21c': 2, '23x': 2, '22b': 2, '22t': 2, '22s': 2, '22c': 2,
         '32x': 3, '30t': 3, '31i': 3, '31t': 3, '31c': 3, '35c': 3, '3rc': 3,
         '45cc': 4, '4rcc': 4, '51l': 5}


def s4(value):
    return value - 16 if value & 8 else value


def s8(value):
    return value - 256 if value & 0x80 else value


def s16(value):
    return value - 0x10000 if value & 0x8000 else value


def s32(value):
    return value - 0x100000000 if value & 0x80000000 else value


class Dex:
    def __init__(self, path):
        self.path = Path(path)
        self.data = data = self.path.read_bytes()
        if data[:4] != b'dex\n':
            raise ValueError(f'{path}: not a DEX file')
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
            self.protos.append('(' + ''.join(self.type_list(params)) + ')' + self.types[ret])
        count, offset = self.pair(80)
        self.fields = []
        for i in range(count):
            klass, kind, name = struct.unpack_from('<HHI', data, offset + i * 8)
            self.fields.append(f'{self.types[klass]}->{self.strings[name]}:{self.types[kind]}')
        count, offset = self.pair(88)
        self.methods = []
        for i in range(count):
            klass, proto, name = struct.unpack_from('<HHI', data, offset + i * 8)
            self.methods.append(f'{self.types[klass]}->{self.strings[name]}{self.protos[proto]}')
        self.classes = {}
        count, offset = self.pair(96)
        for i in range(count):
            klass, flags, parent, ifaces, _, _, classdata, _ = struct.unpack_from(
                '<8I', data, offset + i * 32)
            rec = {'super': self.types[parent] if parent != 0xffffffff else None,
                   'access': flags, 'interfaces': self.type_list(ifaces),
                   'static_fields': [], 'instance_fields': [], 'methods': []}
            if classdata:
                pos = classdata
                counts = []
                for _ in range(4):
                    value, pos = self.uleb(pos)
                    counts.append(value)
                for group, key in ((counts[0], 'static_fields'), (counts[1], 'instance_fields')):
                    index = 0
                    for _ in range(group):
                        delta, pos = self.uleb(pos)
                        access, pos = self.uleb(pos)
                        index += delta
                        rec[key].append({'name': self.fields[index], 'access': access})
                for group in counts[2:]:
                    index = 0
                    for _ in range(group):
                        delta, pos = self.uleb(pos)
                        access, pos = self.uleb(pos)
                        code, pos = self.uleb(pos)
                        index += delta
                        rec['methods'].append({'name': self.methods[index], 'access': access,
                                               'code': code})
            self.classes[self.types[klass]] = rec

    def u16(self, pos):
        return struct.unpack_from('<H', self.data, pos)[0]

    def u32(self, pos):
        return struct.unpack_from('<I', self.data, pos)[0]

    def pair(self, pos):
        return struct.unpack_from('<II', self.data, pos)

    def type_list(self, offset):
        if not offset:
            return []
        return [self.types[self.u16(offset + 4 + 2 * j)] for j in range(self.u32(offset))]

    def uleb(self, pos):
        result, shift = 0, 0
        while True:
            value = self.data[pos]
            pos += 1
            result |= (value & 127) << shift
            if not value & 128:
                return result, pos
            shift += 7

    def sleb(self, pos):
        result, shift = 0, 0
        while True:
            value = self.data[pos]
            pos += 1
            result |= (value & 127) << shift
            shift += 7
            if not value & 128:
                if value & 0x40:
                    result -= 1 << shift
                return result, pos

    def code_item(self, offset):
        regs, ins, outs, tries, _, count = struct.unpack_from('<HHHHII', self.data, offset)
        units = struct.unpack_from('<' + 'H' * count, self.data, offset + 16)
        handlers = []
        if tries:
            pos = offset + 16 + 2 * count + (2 if count & 1 else 0)
            base = pos + 8 * tries
            for t in range(tries):
                start, length, handler = struct.unpack_from('<IHH', self.data, pos + 8 * t)
                hpos = base + handler
                size, hpos = self.sleb(hpos)
                catches = []
                for _ in range(abs(size)):
                    kind, hpos = self.uleb(hpos)
                    addr, hpos = self.uleb(hpos)
                    catches.append((self.types[kind], addr))
                if size <= 0:
                    addr, hpos = self.uleb(hpos)
                    catches.append(('<any>', addr))
                handlers.append((start, start + length, catches))
        return {'registers': regs, 'ins': ins, 'outs': outs, 'units': units, 'tries': handlers}

    def ref(self, op, index):
        name = OPS[op][0]
        if name.startswith(('const-string',)):
            return repr(self.strings[index])
        if name in ('const-class', 'check-cast', 'new-instance', 'instance-of', 'new-array',
                    'filled-new-array', 'filled-new-array/range'):
            return self.types[index]
        if name[1:4] == 'get' or name[1:4] == 'put':
            return self.fields[index]
        if name.startswith('invoke-custom'):
            return f'call_site@{index}'
        if name.startswith('const-method'):
            return f'{name}@{index}'
        return self.methods[index]

    def disassemble(self, offset):
        item = self.code_item(offset)
        units = item['units']
        lines = []
        pos = 0
        while pos < len(units):
            word = units[pos]
            op = word & 0xff
            if op == 0 and word in (0x100, 0x200, 0x300):
                if word == 0x100:
                    size = units[pos + 1]
                    first = units[pos + 2] | units[pos + 3] << 16
                    targets = [units[pos + 4 + 2 * i] | units[pos + 5 + 2 * i] << 16
                               for i in range(size)]
                    lines.append((pos, f'packed-switch-payload first={s32(first)} '
                                       f'rel={[s32(t) for t in targets]}'))
                    pos += 4 + 2 * size
                elif word == 0x200:
                    size = units[pos + 1]
                    keys = [s32(units[pos + 2 + 2 * i] | units[pos + 3 + 2 * i] << 16)
                            for i in range(size)]
                    base = pos + 2 + 2 * size
                    rel = [s32(units[base + 2 * i] | units[base + 1 + 2 * i] << 16)
                           for i in range(size)]
                    lines.append((pos, f'sparse-switch-payload {dict(zip(keys, rel))}'))
                    pos += 2 + 4 * size
                else:
                    width = units[pos + 1]
                    size = units[pos + 2] | units[pos + 3] << 16
                    total = 4 + (width * size + 1) // 2
                    raw = struct.pack('<' + 'H' * (total - 4), *units[pos + 4:pos + total])
                    lines.append((pos, f'array-payload width={width} count={size} '
                                       f'bytes={raw[:width * size].hex()}'))
                    pos += total
                continue
            if op not in OPS:
                lines.append((pos, f'<unused opcode {op:#04x}>'))
                pos += 1
                continue
            name, fmt = OPS[op]
            width = WIDTH[fmt]
            u = units[pos:pos + width]
            a = (word >> 8) & 0xff
            text = name
            if fmt == '10x':
                pass
            elif fmt == '12x':
                text += f' v{a & 15}, v{a >> 4}'
            elif fmt == '11n':
                text += f' v{a & 15}, #{s4(a >> 4)}'
            elif fmt == '11x':
                text += f' v{a}'
            elif fmt == '10t':
                text += f' -> {pos + s8(a):#x}'
            elif fmt == '20t':
                text += f' -> {pos + s16(u[1]):#x}'
            elif fmt == '22x':
                text += f' v{a}, v{u[1]}'
            elif fmt == '21t':
                text += f' v{a}, -> {pos + s16(u[1]):#x}'
            elif fmt == '21s':
                text += f' v{a}, #{s16(u[1])}'
            elif fmt == '21h':
                shift = 48 if 'wide' in name else 16
                text += f' v{a}, #{s16(u[1]) << shift:#x}'
            elif fmt == '21c':
                text += f' v{a}, {self.ref(op, u[1])}'
            elif fmt == '23x':
                text += f' v{a}, v{u[1] & 0xff}, v{u[1] >> 8}'
            elif fmt == '22b':
                text += f' v{a}, v{u[1] & 0xff}, #{s8(u[1] >> 8)}'
            elif fmt == '22t':
                text += f' v{a & 15}, v{a >> 4}, -> {pos + s16(u[2 - 1]):#x}'
            elif fmt == '22s':
                text += f' v{a & 15}, v{a >> 4}, #{s16(u[1])}'
            elif fmt == '22c':
                text += f' v{a & 15}, v{a >> 4}, {self.ref(op, u[1])}'
            elif fmt == '32x':
                text += f' v{u[1]}, v{u[2]}'
            elif fmt == '30t':
                text += f' -> {pos + s32(u[1] | u[2] << 16):#x}'
            elif fmt == '31i':
                text += f' v{a}, #{s32(u[1] | u[2] << 16)}'
            elif fmt == '31t':
                text += f' v{a}, payload@{pos + s32(u[1] | u[2] << 16):#x}'
            elif fmt == '31c':
                text += f' v{a}, {repr(self.strings[u[1] | u[2] << 16])}'
            elif fmt in ('35c', '45cc'):
                count = a >> 4
                regs = [u[2] & 15, (u[2] >> 4) & 15, (u[2] >> 8) & 15, u[2] >> 12, a & 15][:count]
                text += ' {' + ', '.join(f'v{r}' for r in regs) + '}, ' + self.ref(op, u[1])
                if fmt == '45cc':
                    text += f' proto={self.protos[u[3]]}'
            elif fmt in ('3rc', '4rcc'):
                text += f' {{v{u[2]} .. v{u[2] + a - 1}}}, ' + self.ref(op, u[1])
                if fmt == '4rcc':
                    text += f' proto={self.protos[u[3]]}'
            elif fmt == '51l':
                value = u[1] | u[2] << 16 | u[3] << 32 | u[4] << 48
                text += f' v{a}, #{value - (1 << 64) if value >> 63 else value}'
            lines.append((pos, text))
            pos += width
        return item, lines


def load(paths):
    return [Dex(p) for p in paths]


def access_text(flags):
    names = [(0x1, 'public'), (0x2, 'private'), (0x4, 'protected'), (0x8, 'static'),
             (0x10, 'final'), (0x20, 'synchronized'), (0x40, 'bridge'), (0x80, 'varargs'),
             (0x100, 'native'), (0x200, 'interface'), (0x400, 'abstract'), (0x1000, 'synthetic'),
             (0x4000, 'enum'), (0x10000, 'constructor')]
    return ' '.join(n for bit, n in names if flags & bit)


def print_class(dex, name, method_filter=None, out=sys.stdout):
    rec = dex.classes[name]
    print(f'# {dex.path.name}: class {name} [{access_text(rec["access"])}]', file=out)
    print(f'#   super {rec["super"]} implements {rec["interfaces"]}', file=out)
    for key in ('static_fields', 'instance_fields'):
        for field in rec[key]:
            print(f'#   {key[:-7]} field {field["name"]} [{access_text(field["access"])}]', file=out)
    for method in rec['methods']:
        if method_filter and method_filter not in method['name']:
            continue
        print(f'\n.method {method["name"]} [{access_text(method["access"])}] '
              f'code={method["code"]:#x}', file=out)
        if not method['code']:
            continue
        item, lines = dex.disassemble(method['code'])
        print(f'  registers={item["registers"]} ins={item["ins"]} outs={item["outs"]}', file=out)
        for start, end, catches in item['tries']:
            handlers = ', '.join(f'{kind}->{addr:#x}' for kind, addr in catches)
            print(f'  try [{start:#x}, {end:#x}) {handlers}', file=out)
        for pos, text in lines:
            print(f'  {pos:04x}: {text}', file=out)


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--dex', nargs='*', default=[str(p) for p in DEFAULT_DEX])
    parser.add_argument('--class', dest='klass', action='append', default=[])
    parser.add_argument('--method')
    parser.add_argument('--find', action='append', default=[])
    parser.add_argument('--callers', action='append', default=[])
    parser.add_argument('--fields-of', action='append', default=[])
    args = parser.parse_args()
    dexes = load(args.dex)
    for pattern in args.find:
        for dex in dexes:
            for name, rec in dex.classes.items():
                if pattern in name:
                    print(f'{dex.path.name}: {name} super={rec["super"]} '
                          f'ifaces={rec["interfaces"]} methods={len(rec["methods"])}')
    for klass in args.klass:
        found = False
        for dex in dexes:
            if klass in dex.classes:
                print_class(dex, klass, args.method)
                found = True
        if not found:
            print(f'# class {klass} not defined in given DEX files')
    for target in args.callers:
        for dex in dexes:
            for name, rec in dex.classes.items():
                for method in rec['methods']:
                    if not method['code']:
                        continue
                    try:
                        _, lines = dex.disassemble(method['code'])
                    except (IndexError, KeyError, struct.error) as error:
                        print(f'# decode error {dex.path.name} {method["name"]}: {error}')
                        continue
                    for pos, text in lines:
                        if text.startswith('invoke') and target in text:
                            print(f'{dex.path.name}: {method["name"]} +{pos:#x}: {text}')


if __name__ == '__main__':
    main()
