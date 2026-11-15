"""Statically scan the original APK DEX files for Resources resource-id uses.

Read-only: the DEX files are loaded by dex_disasm.py, never executed.  For each
call site the script reports the caller, the invoke instruction and the
constant resource id when a simple backwards data-flow scan can prove one.
Ids that arrive through fields, method results or XML inflation stay marked
"dynamic"; they are not guessed.

Usage:
  python resource-table-next/scan_dex_resource_uses.py [--json out.json]
"""
import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
import dex_disasm  # noqa: E402

TARGET_METHODS = {
    'getColor', 'getColorStateList', 'getResourceEntryName', 'getResourceTypeName',
    'getResourceName', 'getResourcePackageName', 'getDrawable', 'getDimension',
    'getDimensionPixelSize', 'getDimensionPixelOffset', 'getInteger', 'getBoolean',
    'getString', 'getText', 'getXml', 'getLayout', 'openRawResource',
}
INVOKE_RX = re.compile(r'^(invoke-[a-z/]+) \{([^}]*)\}, '
                       r'L(android/content/res/Resources);->([A-Za-z0-9_$<>]+)(\([^)]*\).*)$')
CONST_RX = re.compile(r'^(const(?:/4|/16|/high16|\s|/wide(?:/16|/32|/high16)?)?) '
                      r'v(\d+), #(-?(?:0x[0-9a-fA-F]+|\d+))$')
WRITE_RX = re.compile(r'^[a-z][a-z0-9/_-]* v(\d+)(?:,|\s|$)')
# Instructions whose first vN operand is written.
WRITE_PREFIXES = (
    'const', 'move', 'move-result', 'move-exception', 'move-object', 'move-wide',
    'iget', 'sget', 'new-instance', 'new-array', 'array-length', 'instance-of',
    'check-cast', 'add-int', 'sub-int', 'mul-int', 'div-int', 'rem-int', 'and-int',
    'or-int', 'xor-int', 'shl-int', 'shr-int', 'ushr-int', 'neg-int', 'not-int',
    'add-long', 'sub-long', 'mul-long', 'div-long', 'rem-long', 'and-long',
    'or-long', 'xor-long', 'shl-long', 'shr-long', 'ushr-long', 'add-float',
    'sub-float', 'mul-float', 'div-float', 'rem-float', 'int-to-long',
    'int-to-float', 'long-to-int', 'float-to-int', 'cmp-long', 'cmpl-float',
    'cmpg-float', 'aget', 'aget-object', 'aget-wide', 'aget-boolean', 'aget-byte',
    'aget-char', 'aget-short', 'add-int/lit', 'sub-int/lit', 'mul-int/lit',
    'div-int/lit', 'rem-int/lit', 'and-int/lit', 'or-int/lit', 'xor-int/lit',
    'shl-int/lit', 'shr-int/lit', 'ushr-int/lit', 'add-int/lit8', 'sub-int/lit8',
    'mul-int/lit8', 'div-int/lit8', 'rem-int/lit8', 'and-int/lit8', 'or-int/lit8',
    'xor-int/lit8', 'shl-int/lit8', 'shr-int/lit8', 'ushr-int/lit8',
)


def decode_const(op, literal):
    """Return the 32-bit value a const instruction writes, or None."""
    try:
        value = int(literal, 0)
    except ValueError:
        return None
    if op == 'const/4':
        value &= 0xf
        if value & 0x8:
            value -= 0x10
    elif op == 'const/16':
        value &= 0xffff
        if value & 0x8000:
            value -= 0x10000
    elif op == 'const/high16':
        value &= 0xffff
        if value & 0x8000:
            value -= 0x10000
        value <<= 16
    elif op in ('const-wide', 'const-wide/16', 'const-wide/32', 'const-wide/high16'):
        return None  # not a resource id argument
    value &= 0xffffffff
    return value - (1 << 32) if value & 0x80000000 else value


def written_register(text):
    if text.startswith(WRITE_PREFIXES):
        match = WRITE_RX.match(text)
        if match:
            return int(match.group(1))
    return None


def trace_register(lines, invoke_index, register):
    """Walk backwards from an invoke to the most recent write of `register`."""
    for index in range(invoke_index - 1, max(-1, invoke_index - 80), -1):
        text = lines[index][1]
        match = CONST_RX.match(text)
        if match and int(match.group(2)) == register:
            return {'kind': 'const', 'op': match.group(1),
                    'value': decode_const(match.group(1), match.group(3)),
                    'value_text': match.group(3), 'offset': lines[index][0],
                    'instruction': text}
        other = written_register(text)
        if other == register:
            return {'kind': 'dynamic', 'offset': lines[index][0],
                    'instruction': text}
    return {'kind': 'unknown'}


def scan(dexes):
    calls, constants = [], {}
    for dex in dexes:
        for klass, rec in dex.classes.items():
            for method in rec['methods']:
                if not method['code']:
                    continue
                try:
                    _, lines = dex.disassemble(method['code'])
                except Exception as error:  # keep scanning the other methods
                    calls.append({'dex': dex.path.name, 'class': klass,
                                  'method': method['name'], 'decode_error': str(error)})
                    continue
                for index, (offset, text) in enumerate(lines):
                    if text.startswith('const'):
                        match = CONST_RX.match(text)
                        if match:
                            value = decode_const(match.group(1), match.group(3))
                            if value is not None and 0x7f000000 <= value <= 0x7fffffff:
                                key = str(value)
                                entry = constants.setdefault(key, {
                                    'value': value, 'uses': []})
                                if len(entry['uses']) < 40:
                                    entry['uses'].append(
                                        f'{dex.path.name}:{klass}->{method["name"]}'
                                        f'+{offset:#x}')
                    match = INVOKE_RX.match(text)
                    if not match:
                        continue
                    op, regs_text, owner, name, sig = match.groups()
                    if name not in TARGET_METHODS:
                        continue
                    regs = []
                    if '..' in regs_text:
                        left, right = regs_text.split('..')
                        start = int(left.strip().lstrip('v'))
                        end = int(right.strip().lstrip('v'))
                        regs = list(range(start, end + 1))
                    else:
                        regs = [int(r.strip().lstrip('v')) for r in regs_text.split(',') if r.strip()]
                    args = sig[1:sig.find(')')]
                    # Count wide arguments so the last register maps to the id.
                    wide = args.count('J') + args.count('D')
                    id_reg = regs[-1 - wide] if len(regs) > wide else None
                    source = trace_register(lines, index, id_reg) if id_reg is not None else {'kind': 'unknown'}
                    calls.append({
                        'dex': dex.path.name, 'class': klass, 'method': method['name'],
                        'offset': offset, 'instruction': text, 'owner': owner,
                        'invoke': name, 'signature': sig,
                        'id_register': id_reg,
                        'id_source': source,
                        'id': source.get('value') if source.get('kind') == 'const' else None,
                    })
    return calls, constants


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--json', help='write the complete scan to this path')
    parser.add_argument('--dex', nargs='*',
                        default=[str(p) for p in dex_disasm.DEFAULT_DEX])
    args = parser.parse_args()
    dexes = dex_disasm.load(args.dex)
    calls, constants = scan(dexes)
    report = {'dex_files': [str(Path(p).name) for p in args.dex],
              'call_sites': calls, 'const_resource_ids': constants}
    if args.json:
        Path(args.json).write_text(json.dumps(report, indent=1), encoding='utf-8')
    for call in calls:
        if 'decode_error' in call:
            print(f'# decode error {call["dex"]} {call["class"]}->{call["method"]}: '
                  f'{call["decode_error"]}')
            continue
        source = call['id_source']
        if source.get('kind') == 'const':
            detail = f'id=0x{call["id"] & 0xffffffff:08x} ({call["id"]}) op={source["op"]}'
        else:
            detail = f'id=dynamic source={source.get("kind")} ' \
                     f'last={source.get("instruction", "?")}'
        print(f'{call["dex"]}: {call["class"]}->{call["method"]}+{call["offset"]:#x} '
              f'{call["invoke"]}{call["signature"]} {detail}')
    print(f'\n# {len(calls)} call sites, {len(constants)} distinct const resource ids')
    for value in sorted(constants, key=int):
        entry = constants[value]
        print(f'  0x{entry["value"] & 0xffffffff:08x} {entry["value"]} '
              f'uses={len(entry["uses"])}')


if __name__ == '__main__':
    main()
