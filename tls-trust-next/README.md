# API 26 system certificate verifier

`trust.c` dynamically loads the exact API 26 NetStack chain verifier and its paired free function. It requires a real peer certificate chain and the requested hostname. `ca == NULL` asks NetStack to use system anchors; passing a CA tests explicit trust. A positive return is the unchanged NetStack error code, a negative return is a local error, and zero includes a copied nonempty verified chain. The caller owns that copy and must call `tls_trust_free_chain`. No runtime symbol or malformed platform output fails closed.

ABI evidence: installed DevEco SDK `native/sysroot/usr/include/network/netstack/net_ssl/net_ssl_c.h` declares `OH_NetStack_CreateAndVerifySortedCertChain(const NetStack_CertBlob *, size_t, const NetStack_CertBlob *, const char *, NetStack_CertBlob **, size_t *)` and `OH_NetStack_FreeCertChain(NetStack_CertBlob *, size_t)` since API 26, Stage model only. `net_ssl_c_type.h` declares `NetStack_CertBlob` as enum type, `uint32_t size`, `uint8_t *data`; PEM=0, DER=1. The code uses only the type header so a target SDK 20 build has no direct API 26 symbol reference.

Run `python make_fixtures.py` to make disposable local certificates. It needs the OpenSSL CLI. Compile `selftest.c` with `trust.c` and `-ldl`, then supply `PEER.der CA.der|- HOSTNAME` on the API 26 device. Expected explicit CA tests:

| Peer | CA | Name | Outcome |
| --- | --- | --- | --- |
| valid | test-root | local.test | success, nonempty sorted chain |
| wrong-host | test-root | local.test | hostname error, usually 2305062 |
| expired | test-root | local.test | validity error, usually 2305010 |
| wrong-ca | test-root | local.test | trust error |
| valid | system (`-`) | local.test | trust error |

The generated root is never a system anchor. A system trust positive test requires a real peer chain from a trusted live server. This module authenticates certificates only: TLS transport, OkHttp peer exposure, and the app's `CertificatePinner` remain separate work.
