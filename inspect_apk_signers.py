"""Read public signing certificates from APK v2/v3 blocks, without executing APK.

This inventories certificates only; it does not verify the APK signature.
"""
import hashlib
import json
import struct
from pathlib import Path


def lp(data, offset):
    size = struct.unpack_from('<I', data, offset)[0]
    start = offset + 4
    end = start + size
    if end > len(data):
        raise ValueError('Truncated length-prefixed field')
    return data[start:end], end


def main():
    path = next(Path('.').glob('*.apk'))
    data = path.read_bytes()
    eocd = data.rfind(b'PK\x05\x06', max(0, len(data) - 65557))
    if eocd < 0:
        raise ValueError('ZIP EOCD not found')
    cd = struct.unpack_from('<I', data, eocd + 16)[0]
    if data[cd - 16:cd] != b'APK Sig Block 42':
        raise ValueError('APK signing block not found')
    size = struct.unpack_from('<Q', data, cd - 24)[0]
    start = cd - size - 8
    if struct.unpack_from('<Q', data, start)[0] != size:
        raise ValueError('APK signing block size mismatch')
    pos = start + 8
    result = {'apk': path.name, 'blocks': [],
              'note': 'Public certificate inventory only. Whole APK signature and certificate trust NOT verified.'}
    while pos < cd - 24:
        pairsize = struct.unpack_from('<Q', data, pos)[0]
        if pairsize < 4 or pos + 8 + pairsize > cd - 24:
            raise ValueError('Invalid signing block pair')
        blockid = struct.unpack_from('<I', data, pos + 8)[0]
        value = data[pos + 12:pos + 8 + pairsize]
        pos += 8 + pairsize
        item = {'id': hex(blockid), 'bytes': len(value)}
        if blockid in (0x7109871a, 0xf05368c0, 0x1b93ad61):
            signers, _ = lp(value, 0)
            offset = 0
            item['signers'] = []
            while offset < len(signers):
                signer, offset = lp(signers, offset)
                signed, _ = lp(signer, 0)
                _, field = lp(signed, 0)  # digests
                certificates, _ = lp(signed, field)
                certpos, fingerprints = 0, []
                while certpos < len(certificates):
                    cert, certpos = lp(certificates, certpos)
                    fingerprints.append({'der_bytes': len(cert), 'md5': hashlib.md5(cert).hexdigest(),
                                         'sha256': hashlib.sha256(cert).hexdigest()})
                item['signers'].append({'public_certificates': fingerprints})
        result['blocks'].append(item)
    inventory = json.loads(Path('game-startup-analysis.json').read_text(encoding='utf-8'))
    candidate = inventory['classes'].get('Lcom/gsc/pub/utils/b;', {})
    constants = sorted({s.lower() for m in candidate.get('methods', []) for s in m.get('strings', [])
                        if len(s) == 32 and all(c in '0123456789abcdefABCDEF' for c in s)})
    md5s = {c['md5'] for b in result['blocks'] for s in b.get('signers', [])
            for c in s['public_certificates']}
    result['sdk_check_hex_constants'] = constants
    result['public_certificate_md5_matches_sdk_constant'] = sorted(md5s.intersection(constants))
    Path('compat-spec/apk-public-signers.json').write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
    print('Signing block IDs:', [b['id'] for b in result['blocks']])
    print('Certificate MD5 matches static SDK constant:', bool(result['public_certificate_md5_matches_sdk_constant']))
    print('Whole APK signature NOT verified; original SDK initialization NOT executed.')


if __name__ == '__main__':
    main()
