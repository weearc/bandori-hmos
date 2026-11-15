# TLS transport source preparation

Official Mbed TLS 3.6.7 LTS source: https://github.com/Mbed-TLS/mbedtls/releases/tag/mbedtls-3.6.7 ; source commit 068ff080b369adfac81509f9b57b2afabaf82dc5. Official maintenance statement: https://github.com/Mbed-TLS/mbedtls/blob/development/BRANCHES.md (3.6 supported until March 2027). The upstream source LICENSE offers Apache-2.0 OR GPL-2.0-or-later. Framework source https://github.com/Mbed-TLS/mbedtls-framework at gitlink dde0c4a0e448a0552f18817dcea633bb851fd288. Both clones are in vendor; framework was cloned separately because git submodule failed to create a signal pipe (Win32 error 5). Source was obtained by git clone, not a release archive; no archive SHA256 is available.

DevEco native SDK provides clang 15.0.4, CMake 3.28.2, Ninja, and ohos.toolchain.cmake. build.ps1 configures arm64-v8a static PIC libraries. Configuration identified the aarch64-linux-ohos compiler but hung at compiler ABI detection when Ninja was invoked; interrupted twice, including a retry setting CMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY. No libraries built, no hashes available. This environment prevented completion; do not treat this as a working transport. No trust, hostname, networking, or Java integration has been implemented. Peer chain, ALPN, socket and verification integration remain to be confirmed against actual build/configuration and tested on device.

## Direct build result

CMake configure remains blocked as above. `python tls-transport-next/build_direct.py` instead reads the three upstream CMake source lists and compiles each file using SDK clang with `--target=aarch64-linux-ohos`, SDK sysroot, `-fPIC`, `-O2`, and the upstream default configuration; llvm-ar makes the archives. Detailed commands and diagnostics are in `direct-build/build.log`. Source lists are compiled directly, not generated CMake commands.

- libmbedcrypto.a (81 objects): SHA-256 `41e23cf040d945af7c578115062112d474f3817dc57e0191ef96671c7bcda198`
- libmbedx509.a (9 objects): SHA-256 `79567dff19da1b81b00d6f2889aaddb180a46549e89081caf0a3e775f9f666c8`
- libmbedtls.a (18 objects): SHA-256 `0874073dcd4973d041eb3814aa49e368932b2f1f597fcd64232961a78f1f99d5`

SDK clang linked all archive objects into `whole.so` with `--whole-archive --no-undefined` successfully; SHA-256 `a41b07d52e68879929cda68143e0d4156cd0f51e8b30613a1d14a25bb8edf78d`. A separate SSL/X509 initialization smoke shared object linked successfully and readelf identified ELF64 AArch64. These are cross-link checks, not device execution or handshake validation. APIs in upstream headers include `mbedtls_ssl_get_peer_cert`, `mbedtls_ssl_set_hostname`, `mbedtls_ssl_conf_alpn_protocols`, and `mbedtls_ssl_set_bio`; complete Java integration and trust policy remain unresolved.
