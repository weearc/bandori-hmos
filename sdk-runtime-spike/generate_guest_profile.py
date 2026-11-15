"""Bind an explicit experimental Android profile to the actual input APK.

Only public APK metadata/certificates are read. No authentication or whole-APK
signature verification is performed. These facts must not be presented as such.
"""
from pathlib import Path
import hashlib
import json
import struct
import sys
import zipfile

ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT.parent))
from inspect_apk import manifest_inventory
from inspect_apk_signers import lp

PROFILES = {
    'android-api24-experimental': {'api': 24, 'release': '7.0'},
    'android-api28-experimental': {'api': 28, 'release': '9'},
    'android-api33-experimental': {'api': 33, 'release': '13'},
}
REASON = ('API28 is inside APK min24/target33 and is the first public '
          'Application.getProcessName baseline. This exercises the SDK public '
          'API path; it is not a claim of complete Android9/API28 support.')

def normalize_process(package, declared):
    if declared is not None and not isinstance(declared, str):
        raise ValueError('Resource-valued process requires resource resolution')
    return package if not declared else package + declared if declared.startswith(':') else declared

def generate():
    apk = next(ROOT.parent.glob('BanGDream*.apk'))
    with apk.open('rb') as f:
        apk_sha = hashlib.file_digest(f, 'sha256').hexdigest()
        f.seek(-min(apk.stat().st_size, 65557), 2)
        tail = f.read()
        eocd = tail.rfind(b'PK\x05\x06')
        if eocd < 0:
            raise ValueError('APK ZIP EOCD not found')
        cd = struct.unpack_from('<I', tail, eocd + 16)[0]
        f.seek(cd - 24)
        footer = f.read(24)
        if footer[8:] != b'APK Sig Block 42':
            raise ValueError('APK signing block missing')
        size = struct.unpack_from('<Q', footer)[0]
        f.seek(cd - size - 8)
        block = f.read(size + 8)
    certs = {}
    pos = 8
    while pos < len(block) - 24:
        size, bid = struct.unpack_from('<QI', block, pos)
        if size < 4 or pos + size + 8 > len(block) - 24:
            raise ValueError('Invalid signing block field')
        value = block[pos + 12:pos + size + 8]
        pos += size + 8
        if bid not in (0x7109871a, 0xf05368c0, 0x1b93ad61):
            continue
        signers, _ = lp(value, 0)
        off = 0
        while off < len(signers):
            signer, off = lp(signers, off)
            signed, _ = lp(signer, 0)
            _, field = lp(signed, 0)
            encoded, _ = lp(signed, field)
            off_cert = 0
            while off_cert < len(encoded):
                cert, off_cert = lp(encoded, off_cert)
                digest = hashlib.sha256(cert).hexdigest()
                certs[digest] = {'sha256': digest, 'md5': hashlib.md5(cert).hexdigest(),
                                 'der_bytes': len(cert), 'source_block_id': hex(bid)}
                directory = ROOT / 'public-certificates'
                directory.mkdir(exist_ok=True)
                (directory / (digest + '.der')).write_bytes(cert)
    if len(certs) != 1:
        raise ValueError('This experimental profile currently requires one unique certificate')
    cert = next(iter(certs.values()))
    with zipfile.ZipFile(apk) as z:
        manifest_data = z.read('AndroidManifest.xml')
        tags = manifest_inventory(manifest_data)
        dex_hashes = {n: hashlib.sha256(z.read(n)).hexdigest() for n in z.namelist()
                      if n.startswith('classes') and n.endswith('.dex') and '/' not in n}
    manifest = next(t['attributes'] for t in tags if t['tag'] == 'manifest')
    application = next(t['attributes'] for t in tags if t['tag'] == 'application')
    sdk = next(t['attributes'] for t in tags if t['tag'] == 'uses-sdk')
    package = manifest['package']
    declared = application.get('process')
    process = normalize_process(package, declared)
    identity = {'package': package, 'process': process,
                'apk_sha256': apk_sha, 'manifest_sha256': hashlib.sha256(manifest_data).hexdigest(),
                'cert_md5': cert['md5'], 'cert_sha256': cert['sha256'],
                'min_sdk': sdk['minSdkVersion'], 'target_sdk': sdk['targetSdkVersion'],
                'compile_sdk': manifest['compileSdkVersion'], 'version_code': manifest['versionCode'],
                'version_name': manifest['versionName']}
    doc = {'schema': 1, 'selected_profile': 'android-api28-experimental',
           'selection_reason': REASON, 'profiles': PROFILES, 'identity': identity,
           'provenance': {'apk': apk.name, 'manifest_application_process_attribute': declared,
                          'process_rule': 'Absent process defaults to manifest package; :name expands to package:name.',
                          'component_process_declarations': [
                              {'tag': t['tag'], 'name': t['attributes'].get('name'),
                               'declared_process': t['attributes']['process'],
                               'normalized_process': normalize_process(package, t['attributes']['process'])}
                              for t in tags if 'process' in t['attributes']],
                          'public_signing_certificates': list(certs.values()), 'dex_sha256': dex_hashes},
           'limitations': ['HarmonyOS API26 is not used as Android SDK_INT.',
                          'Experimental guest API declaration; full Android compatibility not implemented.',
                          'Public certificate inventory only; complete APK signature verification not performed.',
                          'No SDK authentication, login, risk-control or signature-check success is synthesized.',
                          'Unknown framework/business APIs fail explicitly.']}
    (ROOT / 'guest-environment.json').write_text(json.dumps(doc, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    values = {'schema': 1, 'profile': doc['selected_profile'], 'android_api': 28, **identity}
    cfg = '# Explicit experimental Android guest profile; not HarmonyOS API26.\n'
    cfg += '\n'.join(f'{k}={v}' for k, v in values.items()) + '\n'
    (ROOT / 'dex' / 'guest-profile.cfg').write_text(cfg, encoding='ascii')
    header = '/* Generated from original APK public metadata; review guest-environment.json. */\n#pragma once\n'
    for k, v in identity.items():
        header += f'#define GUEST_EXPECTED_{k.upper()} {json.dumps(v)}\n'
    header += '#define GUEST_PROFILE_CANONICAL_CFG ' + json.dumps(cfg) + '\n'
    (ROOT / 'generated' / 'guest_identity.h').write_text(header, encoding='ascii')
    print('Guest profile:', doc['selected_profile'], 'package/process:', package, process)

if __name__ == '__main__':
    generate()
