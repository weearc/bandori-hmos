from pathlib import Path
import hashlib
import subprocess

root = Path(__file__).resolve().parent
certificate = root.parent/'tls-trust-next/fixtures/valid.der'
public = subprocess.check_output(['openssl', 'x509', '-inform', 'DER', '-in', str(certificate), '-pubkey', '-noout'])
spki = subprocess.check_output(['openssl', 'pkey', '-pubin', '-outform', 'DER'], input=public)
digest = hashlib.sha256(spki).digest()
(root/'expected_spki.h').write_text('static const unsigned char expected_spki_hash[32]={' + ','.join(str(x) for x in digest) + '};\n')
print('Independent OpenSSL SPKI SHA256 fixture generated')
