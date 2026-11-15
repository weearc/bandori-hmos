"""Resolve the original SDK initializer's string resources without APK execution.

Only handles this APK's dense, non-compact resource entries. Keeps values local;
the console prints names/IDs and lengths, never SDK key values.
Layout reference: AOSP androidfw/include/androidfw/ResourceTypes.h.
"""
import hashlib
import json
import struct
import zipfile
from pathlib import Path
from inspect_game_startup import Dex


def string_pool(data, start):
    kind, header, size = struct.unpack_from('<HHI', data, start)
    if kind != 1:
        raise ValueError('Expected resource string pool')
    count, _, flags, strings, _ = struct.unpack_from('<IIIII', data, start + 8)
    utf8 = bool(flags & 256)

    def length(pos):
        if utf8:
            value = data[pos]
            return (((value & 127) << 8) | data[pos + 1], pos + 2) if value & 128 else (value, pos + 1)
        value = struct.unpack_from('<H', data, pos)[0]
        return (((value & 32767) << 16) | struct.unpack_from('<H', data, pos + 2)[0], pos + 4) if value & 32768 else (value, pos + 2)

    result = []
    for index in range(count):
        offset = struct.unpack_from('<I', data, start + header + index * 4)[0]
        count_chars, pos = length(start + strings + offset)
        count_bytes, pos = length(pos) if utf8 else (count_chars * 2, pos)
        if pos + count_bytes > start + size:
            raise ValueError('Resource string outside pool')
        result.append(data[pos:pos + count_bytes].decode('utf-8' if utf8 else 'utf-16-le', 'strict'))
    return result


def resource_values(data, required):
    strings = None
    packages = []
    pos = struct.unpack_from('<H', data, 2)[0]
    while pos < len(data):
        kind, header, size = struct.unpack_from('<HHI', data, pos)
        if size < header or pos + size > len(data):
            raise ValueError('Invalid resource chunk')
        if kind == 1:
            strings = string_pool(data, pos)
        if kind == 0x200:
            packages.append((pos, header, size))
        pos += size
    if strings is None:
        raise ValueError('Global string pool missing')
    found = {}
    for base, header, size in packages:
        package_id = struct.unpack_from('<I', data, base + 8)[0]
        type_offset = struct.unpack_from('<I', data, base + 268)[0]
        key_offset = struct.unpack_from('<I', data, base + 276)[0]
        type_id_offset = struct.unpack_from('<I', data, base + 284)[0] if header >= 288 else 0
        types = string_pool(data, base + type_offset)
        keys = string_pool(data, base + key_offset)
        pos = base + header
        while pos < base + size:
            kind, hsize, length = struct.unpack_from('<HHI', data, pos)
            if length < hsize or pos + length > base + size:
                raise ValueError('Invalid package resource chunk')
            if kind == 0x201:
                type_id, flags, _, count, entry_start = struct.unpack_from('<BBHII', data, pos + 8)
                if flags:
                    raise ValueError('Sparse/16-bit resource offsets are not implemented by this extractor')
                score = sum(byte != 0 for byte in data[pos + 24:pos + hsize])
                for entry_index in range(count):
                    resource_id = package_id << 24 | (type_id + type_id_offset) << 16 | entry_index
                    if resource_id not in required:
                        continue
                    relative = struct.unpack_from('<I', data, pos + hsize + entry_index * 4)[0]
                    if relative == 0xffffffff:
                        continue
                    entry = pos + entry_start + relative
                    entry_size, entry_flags, key = struct.unpack_from('<HHI', data, entry)
                    if entry_flags & (1 | 8):
                        raise ValueError('Complex or compact SDK resource entry is not supported')
                    _, _, datatype, value = struct.unpack_from('<HBBI', data, entry + entry_size)
                    if datatype != 3:
                        raise ValueError('Expected SDK string resource, got type ' + hex(datatype))
                    record = {'id': resource_id, 'hex_id': hex(resource_id), 'name': keys[key],
                              'type': types[type_id - 1], 'value': strings[value],
                              'configuration_nonzero_bytes': score}
                    if resource_id not in found or score < found[resource_id]['configuration_nonzero_bytes']:
                        found[resource_id] = record
            pos += length
    if set(found) != required:
        raise ValueError('Not all SDK initializer resources were resolved')
    return found


def main():
    apk = next(Path('.').glob('*.apk'))
    with zipfile.ZipFile(apk) as archive:
        dex = Dex(archive.read('classes.dex'))
        targets = {'sdk_merchant_id', 'sdk_app_id', 'sdk_server_id', 'sdk_app_key'}
        count, offset = dex.pair(80)
        fields = {}
        owner = 'Lcom/bilibili/star/bili/R$string;'
        for index in range(count):
            klass, _, name = struct.unpack_from('<HHI', dex.data, offset + index * 8)
            if dex.types[klass] == owner:
                fields[index] = dex.strings[name]
        count, offset = dex.pair(96)
        values = {}
        for index in range(count):
            klass, _, _, _, _, _, class_data, static_values = struct.unpack_from('<8I', dex.data, offset + index * 32)
            if dex.types[klass] != owner:
                continue
            number, pos = dex.uleb(class_data)
            for _ in range(3):
                _, pos = dex.uleb(pos)
            static_fields, field_id = [], 0
            for _ in range(number):
                delta, pos = dex.uleb(pos)
                _, pos = dex.uleb(pos)
                field_id += delta
                static_fields.append(field_id)
            number, pos = dex.uleb(static_values)
            for field in static_fields[:number]:
                head = dex.data[pos]
                pos += 1
                if head & 31 != 4:
                    raise ValueError('Expected int R.string encoded value')
                width = (head >> 5) + 1
                value = int.from_bytes(dex.data[pos:pos + width], 'little', signed=True)
                pos += width
                if fields[field] in targets:
                    values[fields[field]] = value
        if set(values) != targets:
            raise ValueError('Original R.string initializer arguments not all found')
        resources = resource_values(archive.read('resources.arsc'), set(values.values()))
    for name, resource_id in values.items():
        if resources[resource_id]['name'] != name:
            raise ValueError('DEX and resource table name mismatch')
    with apk.open('rb') as stream:
        digest = hashlib.file_digest(stream, 'sha256').hexdigest()
    output = {'source_apk_sha256': digest,
              'entry': 'PermissionActivity.initialize -> GSCPubCommon.init',
              'resources': {name: resources[value] for name, value in values.items()},
              'note': 'Original local SDK configuration values; do not print key values in device trace. No SDK init executed by this script.'}
    destination = Path('compat-spec/sdk-initializer-resources.json')
    destination.write_text(json.dumps(output, ensure_ascii=False, indent=2), encoding='utf-8')
    print('Resolved four original SDK initializer resources; values retained locally, not printed.')
    for name, value in output['resources'].items():
        print(name, value['hex_id'], 'characters', len(value['value']))


if __name__ == '__main__':
    main()
