"""Generate the minimal real resource table for platform_runtime.c from the
original APK's resources.arsc.

The APK and its resources.arsc are read, never executed or modified.  The
generated include file is placed in sdk-init-next/apk_resource_table.inc and
contains:

  * platform_apk_resource_names[]    - every real (id, type, entry-name) row
  * platform_apk_resource_colors[]   - every resolvable color ARGB value
  * platform_apk_resource_reasons[]  - real fail-closed reason for ids that
                                        exist but cannot be resolved to ARGB
  * digests                          - APK/resources.arsc SHA-256 and ZIP CRC

Only dense, non-compact entry arrays are accepted; anything else fails the
generation instead of guessing.  Layout reference:
AOSP androidfw/include/androidfw/ResourceTypes.h.

Usage:
  python resource-table-next/gen_apk_resource_table.py \
      --apk BanGDream_....apk --out sdk-init-next/apk_resource_table.inc
"""
import argparse
import hashlib
import json
import struct
import zipfile
from pathlib import Path

RES_STRING_POOL_TYPE = 0x0001
RES_TABLE_TYPE = 0x0002
RES_TABLE_PACKAGE_TYPE = 0x0200
RES_TABLE_TYPE_TYPE = 0x0201
RES_TABLE_TYPE_SPEC_TYPE = 0x0202

TYPE_NULL = 0x00
TYPE_REFERENCE = 0x01
TYPE_ATTRIBUTE = 0x02
TYPE_STRING = 0x03
TYPE_FLOAT = 0x04
TYPE_DIMENSION = 0x05
TYPE_FRACTION = 0x06
TYPE_DYNAMIC_REFERENCE = 0x07
TYPE_DYNAMIC_ATTRIBUTE = 0x08
TYPE_INT_DEC = 0x10
TYPE_INT_HEX = 0x11
TYPE_INT_BOOLEAN = 0x12
TYPE_INT_COLOR_ARGB8 = 0x1c
TYPE_INT_COLOR_RGB8 = 0x1d
TYPE_INT_COLOR_ARGB4 = 0x1e
TYPE_INT_COLOR_RGB4 = 0x1f

TYPE_NAMES = {
    TYPE_NULL: 'null', TYPE_REFERENCE: 'reference', TYPE_ATTRIBUTE: 'attribute',
    TYPE_STRING: 'string', TYPE_FLOAT: 'float', TYPE_DIMENSION: 'dimension',
    TYPE_FRACTION: 'fraction', TYPE_DYNAMIC_REFERENCE: 'dynamic_reference',
    TYPE_DYNAMIC_ATTRIBUTE: 'dynamic_attribute', TYPE_INT_DEC: 'int_dec',
    TYPE_INT_HEX: 'int_hex', TYPE_INT_BOOLEAN: 'boolean',
    TYPE_INT_COLOR_ARGB8: 'argb8', TYPE_INT_COLOR_RGB8: 'rgb8',
    TYPE_INT_COLOR_ARGB4: 'argb4', TYPE_INT_COLOR_RGB4: 'rgb4',
}

ENTRY_FLAG_COMPLEX = 0x0001
ENTRY_FLAG_COMPACT = 0x0008
TYPE_FLAG_SPARSE = 0x01
TYPE_FLAG_OFFSET16 = 0x02


class ResourceError(Exception):
    pass


def u16(data, pos):
    return struct.unpack_from('<H', data, pos)[0]


def u32(data, pos):
    return struct.unpack_from('<I', data, pos)[0]


def parse_string_pool(data, base, limit):
    kind, header, size = struct.unpack_from('<HHI', data, base)
    if kind != RES_STRING_POOL_TYPE:
        raise ResourceError(f'expected string pool at {base:#x}, got {kind:#x}')
    if size < header or base + size > limit:
        raise ResourceError(f'string pool at {base:#x} exceeds its parent')
    count, style_count, flags, strings_start, styles_start = struct.unpack_from(
        '<IIIII', data, base + 8)
    utf8 = bool(flags & 0x100)
    offsets = [u32(data, base + header + i * 4) for i in range(count)]
    result = []
    for offset in offsets:
        pos = base + strings_start + offset
        if utf8:
            first = data[pos]
            pos += 2 if first & 0x80 else 1
            second = data[pos]
            pos += 2 if second & 0x80 else 1
            end = data.index(b'\0', pos)
            result.append(data[pos:end].decode('utf-8', 'replace'))
        else:
            first = u16(data, pos)
            if first & 0x8000:
                length = ((first & 0x7fff) << 16) | u16(data, pos + 2)
                pos += 4
            else:
                length = first
                pos += 2
            result.append(data[pos:pos + length * 2].decode('utf-16-le', 'replace'))
    return result


def decode_value(data, data_type, value, string_pool):
    if data_type == TYPE_STRING:
        if value >= len(string_pool):
            raise ResourceError(f'string index {value} outside pool')
        return string_pool[value]
    return None


def value_to_argb(data_type, value):
    if data_type == TYPE_INT_COLOR_ARGB8:
        return value & 0xffffffff
    if data_type == TYPE_INT_COLOR_RGB8:
        return 0xff000000 | (value & 0xffffff)
    if data_type == TYPE_INT_COLOR_ARGB4:
        a = (value >> 12) & 0xf
        r = (value >> 8) & 0xf
        g = (value >> 4) & 0xf
        b = value & 0xf
        return ((a << 28) | (a << 24) | (r << 20) | (r << 16) |
                (g << 12) | (g << 8) | (b << 4) | b)
    if data_type == TYPE_INT_COLOR_RGB4:
        r = (value >> 8) & 0xf
        g = (value >> 4) & 0xf
        b = value & 0xf
        return 0xff000000 | (r << 20) | (r << 16) | (g << 12) | (g << 8) | (b << 4) | b
    if data_type in (TYPE_INT_DEC, TYPE_INT_HEX):
        # Android compiles color resources to ARGB8; an integer here is not a
        # color declaration.  Keep it unresolved rather than guess.
        return None
    return None


def parse_color_string(text):
    """Only accept Android hex color syntax; names are not guessed."""
    if not text or not text.startswith('#'):
        return None
    digits = text[1:]
    try:
        if len(digits) == 6:
            return 0xff000000 | int(digits, 16)
        if len(digits) == 8:
            return int(digits, 16) & 0xffffffff
        if len(digits) == 3:
            r, g, b = (int(c * 2, 16) for c in digits)
            return 0xff000000 | (r << 16) | (g << 8) | b
        if len(digits) == 4:
            a, r, g, b = (int(c * 2, 16) for c in digits)
            return (a << 24) | (r << 16) | (g << 8) | b
    except ValueError:
        return None
    return None


def default_config_score(data, base, config_size):
    # A configuration whose bytes after its size field are all zero is the
    # default (unqualified) configuration.  Lower is preferred.
    if config_size < 4 or config_size > 256:
        raise ResourceError(f'implausible ResTable_config size {config_size}')
    end = base + config_size
    return sum(byte != 0 for byte in data[base + 4:end])


def parse_arsc(data):
    kind, header_size, total_size = struct.unpack_from('<HHI', data, 0)
    if kind != RES_TABLE_TYPE or header_size < 12 or total_size != len(data):
        raise ResourceError('not a complete resources.arsc table')
    strings = None
    packages = []
    pos = header_size
    while pos < total_size:
        chunk_kind, chunk_header, chunk_size = struct.unpack_from('<HHI', data, pos)
        if chunk_size < chunk_header or pos + chunk_size > total_size:
            raise ResourceError(f'invalid chunk {chunk_kind:#x} at {pos:#x}')
        if chunk_kind == RES_STRING_POOL_TYPE:
            strings = parse_string_pool(data, pos, total_size)
        elif chunk_kind == RES_TABLE_PACKAGE_TYPE:
            packages.append(pos)
        pos += chunk_size
    if strings is None:
        raise ResourceError('global string pool is missing')

    resources = {}  # id -> record; best configuration wins
    for package_base in packages:
        package_kind, package_header, package_size = struct.unpack_from(
            '<HHI', data, package_base)
        package_id = u32(data, package_base + 8)
        package_name = data[package_base + 12:package_base + 12 + 256].decode(
            'utf-16-le').split('\0')[0]
        type_strings_offset = u32(data, package_base + 268)
        key_strings_offset = u32(data, package_base + 276)
        type_id_offset = (u32(data, package_base + 284)
                          if package_header >= 288 else 0)
        type_names = parse_string_pool(data, package_base + type_strings_offset,
                                       package_base + package_size)
        key_names = parse_string_pool(data, package_base + key_strings_offset,
                                      package_base + package_size)
        pos = package_base + package_header
        while pos < package_base + package_size:
            chunk_kind, chunk_header, chunk_size = struct.unpack_from(
                '<HHI', data, pos)
            if chunk_size < chunk_header or pos + chunk_size > package_base + package_size:
                raise ResourceError(f'invalid package chunk at {pos:#x}')
            if chunk_kind != RES_TABLE_TYPE_TYPE:
                pos += chunk_size
                continue
            type_id = data[pos + 8]
            flags = data[pos + 9]
            entry_count = u32(data, pos + 12)
            entries_start = u32(data, pos + 16)
            config_size = u32(data, pos + 20)
            score = default_config_score(data, pos + 20, config_size)
            if flags & TYPE_FLAG_SPARSE:
                raise ResourceError('sparse resource entries are not supported')
            if flags & TYPE_FLAG_OFFSET16:
                raise ResourceError('16-bit resource offsets are not supported')
            offsets_pos = pos + chunk_header
            for index in range(entry_count):
                entry_offset = u32(data, offsets_pos + index * 4)
                if entry_offset == 0xffffffff:
                    continue
                entry = pos + entries_start + entry_offset
                entry_size, entry_flags, key_index = struct.unpack_from('<HHI', data, entry)
                if entry_flags & ENTRY_FLAG_COMPACT:
                    raise ResourceError('compact resource entries are not supported')
                if entry_flags & ENTRY_FLAG_COMPLEX:
                    # Map (style/array) entries have no scalar value.  Record
                    # the existence so metadata still answers, but no color.
                    record = {
                        'id': (package_id << 24) | ((type_id + type_id_offset) << 16) | index,
                        'type': type_names[type_id - 1],
                        'name': key_names[key_index],
                        'data_type': None, 'data': None, 'complex': True,
                        'config_score': score, 'config_size': config_size,
                        'package': package_name,
                    }
                else:
                    value_type = data[entry + entry_size + 3]
                    value_data = u32(data, entry + entry_size + 4)
                    record = {
                        'id': (package_id << 24) | ((type_id + type_id_offset) << 16) | index,
                        'type': type_names[type_id - 1],
                        'name': key_names[key_index],
                        'data_type': value_type, 'data': value_data, 'complex': False,
                        'config_score': score, 'config_size': config_size,
                        'package': package_name,
                    }
                old = resources.get(record['id'])
                if old is None or (record['config_score'], record['config_size']) < (
                        old['config_score'], old['config_size']):
                    resources[record['id']] = record
            pos += chunk_size
    return strings, resources


def resolve_color(resource_id, resources, strings, seen):
    if resource_id in seen:
        return None, f'cyclic color reference through 0x{resource_id:08x}'
    seen = seen | {resource_id}
    record = resources.get(resource_id)
    if record is None:
        return None, f'resource 0x{resource_id:08x} is not present in resources.arsc'
    if record['complex']:
        return None, f'{record["type"]}/{record["name"]} is a complex (map) resource, not a scalar color'
    argb = value_to_argb(record['data_type'], record['data'])
    if argb is not None:
        return argb, None
    if record['data_type'] == TYPE_REFERENCE:
        target = record['data'] & 0xffffffff
        target_record = resources.get(target)
        if target_record is None:
            return None, (f'{record["type"]}/{record["name"]} references '
                          f'0x{target:08x}, which is outside resources.arsc')
        if target_record['type'] == 'attr':
            return None, (f'{record["type"]}/{record["name"]} references theme '
                          f'attribute 0x{target:08x}; a Theme is required')
        return resolve_color(target, resources, strings, seen)
    if record['data_type'] == TYPE_ATTRIBUTE:
        return None, (f'{record["type"]}/{record["name"]} is a theme attribute '
                      f'value and requires a Theme')
    if record['data_type'] == TYPE_STRING:
        text = strings[record['data']] if record['data'] < len(strings) else None
        argb = parse_color_string(text)
        if argb is not None:
            return argb, None
        return None, (f'{record["type"]}/{record["name"]} holds string '
                      f'{text!r}, which is not an Android hex color')
    return None, (f'{record["type"]}/{record["name"]} has value type '
                  f'{TYPE_NAMES.get(record["data_type"], hex(record["data_type"]))}, '
                  f'which is not a color')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--apk', required=True)
    parser.add_argument('--out', required=True)
    parser.add_argument('--dex-uses', default='resource-table-next/dex-resource-uses.json')
    parser.add_argument('--report', default='resource-table-next/apk-resource-report.json')
    args = parser.parse_args()

    apk_path = Path(args.apk)
    apk_bytes = apk_path.read_bytes()
    apk_sha256 = hashlib.sha256(apk_bytes).hexdigest()
    with zipfile.ZipFile(apk_path) as archive:
        info = archive.getinfo('resources.arsc')
        arsc = archive.read('resources.arsc')
    arsc_sha256 = hashlib.sha256(arsc).hexdigest()
    strings, resources = parse_arsc(arsc)

    colors, reasons = {}, {}
    for resource_id, record in sorted(resources.items()):
        if record['type'] in ('color', 'drawable'):
            argb, reason = resolve_color(resource_id, resources, strings, frozenset())
            if argb is not None:
                colors[resource_id] = argb
            else:
                reasons[resource_id] = reason

    dex_ids = {}
    dex_scan = {}
    dex_path = Path(args.dex_uses)
    if dex_path.exists():
        dex_scan = json.loads(dex_path.read_text(encoding='utf-8'))
        dex_ids = dex_scan.get('const_resource_ids', {})

    # Only ids whose package/type/entry coordinates are in range count as
    # APK resources.  Masks such as 0x7fffffff are not resource ids.
    dex_matches, dex_missing = [], []
    for key, entry in sorted(dex_ids.items(), key=lambda item: int(item[0])):
        value = int(key)
        record = resources.get(value)
        if record is not None:
            dex_matches.append({'id': value, 'type': record['type'],
                                'name': record['name'],
                                'uses': entry.get('uses', [])})
        else:
            dex_missing.append({'id': value, 'uses': entry.get('uses', [])})

    # Direct constants passed to a concrete Resources call site are stronger
    # evidence than a whole-method constant scan: this includes platform
    # package ids (0x01xxxxxx) that cannot appear in the APK's own table.
    call_site_ids = {}
    for call in dex_scan.get('call_sites', []):
        value = call.get('id')
        if value is None:
            continue
        entry = call_site_ids.setdefault(str(value), {'value': value, 'uses': []})
        if len(entry['uses']) < 40:
            entry['uses'].append(
                f'{call["dex"]}:{call["class"]}->{call["method"]}+{call["offset"]:#x} '
                f'{call["invoke"]}{call["signature"]}')
    call_matches, call_missing = [], []
    for key, entry in sorted(call_site_ids.items(), key=lambda item: int(item[0])):
        value = int(key)
        record = resources.get(value)
        row = {'id': value, 'uses': entry['uses']}
        if record is not None:
            row['type'] = record['type']
            row['name'] = record['name']
            call_matches.append(row)
        else:
            call_missing.append(row)

    report = {
        'apk': {'path': apk_path.name, 'sha256': apk_sha256},
        'resources_arsc': {'sha256': arsc_sha256, 'zip_crc32': f'{info.CRC:08x}',
                           'bytes': len(arsc)},
        'package_count': len({r['package'] for r in resources.values()}),
        'resource_count': len(resources),
        'type_counts': {},
        'color_value_count': len(colors),
        'color_unresolved': len(reasons),
        'color_unresolved_reasons': {f'0x{k:08x}': v for k, v in sorted(reasons.items())},
        'dex_const_ids_in_apk': dex_matches,
        'dex_const_ids_not_in_apk': dex_missing,
        'dex_call_site_ids_in_apk': call_matches,
        'dex_call_site_ids_not_in_apk': call_missing,
    }
    for record in resources.values():
        report['type_counts'][record['type']] = report['type_counts'].get(record['type'], 0) + 1
    Path(args.report).write_text(json.dumps(report, indent=1), encoding='utf-8')

    lines = []
    lines.append('/* Generated by resource-table-next/gen_apk_resource_table.py.')
    lines.append(' * Source: original APK resources.arsc, read-only; nothing executed.')
    lines.append(f' * APK file: {apk_path.name}')
    lines.append(f' * APK SHA-256: {apk_sha256}')
    lines.append(f' * resources.arsc SHA-256: {arsc_sha256}')
    lines.append(f' * resources.arsc ZIP CRC32: {info.CRC:08x}')
    lines.append(f' * Unique resource ids: {len(resources)}; resolvable colors: {len(colors)}.')
    lines.append(' * Do not edit by hand; regenerate from the APK. */')
    lines.append('#define PLATFORM_APK_RESOURCE_TABLE 1')
    lines.append(f'#define PLATFORM_APK_RESOURCE_COUNT {len(resources)}')
    lines.append(f'#define PLATFORM_APK_COLOR_COUNT {len(colors)}')
    lines.append(f'#define PLATFORM_APK_REASON_COUNT {len(reasons)}')
    lines.append('static const platform_apk_resource platform_apk_resource_names[] = {')
    for resource_id, record in sorted(resources.items()):
        name = record['name'].replace('\\', '\\\\').replace('"', '\\"')
        rtype = record['type'].replace('\\', '\\\\').replace('"', '\\"')
        lines.append(f' {{{resource_id:#010x},"{rtype}","{name}"}},')
    lines.append('};')
    lines.append('static const platform_apk_color platform_apk_resource_colors[] = {')
    for resource_id, argb in sorted(colors.items()):
        lines.append(f' {{{resource_id:#010x},{argb:#010x}u}},')
    lines.append('};')
    lines.append('static const platform_apk_reason platform_apk_resource_reasons[] = {')
    for resource_id, reason in sorted(reasons.items()):
        escaped = reason.replace('\\', '\\\\').replace('"', '\\"')
        lines.append(f' {{{resource_id:#010x},"{escaped}"}},')
    lines.append('};')
    out_path = Path(args.out)
    out_path.write_text('\n'.join(lines) + '\n', encoding='utf-8')
    print(f'wrote {out_path} ({out_path.stat().st_size} bytes)')
    print(json.dumps({k: report[k] for k in (
        'resource_count', 'type_counts', 'color_value_count', 'color_unresolved')},
        indent=1))
    print(f'dex const ids in APK: {len(dex_matches)}, not in APK: {len(dex_missing)}')


if __name__ == '__main__':
    main()
