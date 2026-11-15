"""Record ELF loading features and runtime audio symbol hints without execution."""
import json
import re
import struct
import zipfile
from collections import Counter
from pathlib import Path


def describe(data):
    phoff, shoff = struct.unpack_from('<QQ', data, 32)
    phsize, phcount, shsize, shcount, shstrings = struct.unpack_from('<5H', data, 54)
    segments = [struct.unpack_from('<IIQQQQQQ', data, phoff + i * phsize) for i in range(phcount)]
    sections = [struct.unpack_from('<IIQQQQIIQQ', data, shoff + i * shsize) for i in range(shcount)]
    table = sections[shstrings]
    relocations = Counter()
    features = []
    for section in sections:
        pos = table[4] + section[0]
        name = data[pos:data.index(b'\0', pos)].decode('ascii', 'replace')
        if name in ('.init_array', '.fini_array', '.tdata', '.tbss', '.android.rel.dyn',
                    '.android.rela.dyn', '.relr.dyn', '.gnu.version', '.gnu.version_r'):
            features.append({'section': name, 'size': section[5]})
        if section[1] == 4:
            for offset in range(section[4], section[4] + section[5], section[9] or 24):
                info = struct.unpack_from('<Q', data, offset + 8)[0]
                relocations[str(info & 0xffffffff)] += 1
    strings = set(x.decode('ascii') for x in re.findall(rb'[\x20-\x7e]{6,}', data))
    return {'load_segment_alignments': sorted({p[7] for p in segments if p[0] == 1}),
            'has_tls_segment': any(p[0] == 7 for p in segments),
            'gnu_stack_flags': [p[1] for p in segments if p[0] == 0x6474e551],
            'relocation_types_numeric': dict(relocations), 'sections': features,
            'aaudio_symbol_strings': sorted(x for x in strings if re.fullmatch(r'AAudio[A-Za-z0-9_]+', x)),
            'cri_version_strings': sorted(x for x in strings if x.startswith('CRI ') and 'Ver.' in x)}


def main():
    apk = next(Path('.').glob('*.apk'))
    with zipfile.ZipFile(apk) as archive:
        result = {'libraries': {Path(n).name: describe(archive.read(n)) for n in archive.namelist()
                               if n.startswith('lib/arm64-v8a/') and n.endswith('.so')}}
        data = archive.read('assets/bin/Data/Managed/Metadata/global-metadata.dat')
        magic, version = struct.unpack_from('<II', data)
        result['metadata_header'] = {'magic': hex(magic), 'version': version}
        result['asset_summary'] = {'acb_files': sum(n.endswith('.acb') for n in archive.namelist()),
                                   'awb_files': sum(n.endswith('.awb') for n in archive.namelist()),
                                   'has_obb_entry': any(n.endswith('.obb') for n in archive.namelist())}
    layer = json.loads(Path('layer-analysis.json').read_text(encoding='utf-8'))
    audioexports = set(layer['libraries']['libaaudio.so']['exports'])
    names = result['libraries']['libcri_ware_unity.so']['aaudio_symbol_strings']
    result['cri_aaudio_names_not_in_reference_exports'] = [x for x in names if x not in audioexports]
    result['limitations'] = 'String names are backend hints, not proof of runtime calls or configured backend. Numeric relocation inventory covers SHT_RELA only.'
    Path('native-requirements.json').write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
    print('Wrote native-requirements.json')
    print('Metadata', result['metadata_header'], 'Assets', result['asset_summary'])
    print('CRI AAudio candidate names', names)
    print('Not exported by reference AAudio', result['cri_aaudio_names_not_in_reference_exports'])
    for name in ['libunity.so', 'libil2cpp.so', 'libcri_ware_unity.so']:
        print(name, {k:v for k,v in result['libraries'][name].items()
                     if k not in ('aaudio_symbol_strings', 'cri_version_strings')})


if __name__ == '__main__':
    main()
