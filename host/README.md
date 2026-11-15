# HarmonyOS compatibility probes

> Audit note (2026-11-02): The text below contains historical module states and build instructions. Read the [current audit requirements](../README.md) and [handoff](../handoff.md) before using it; old pending/current claims do not supersede later device reports. Original game JNI_OnLoad and Unity execution remain unverified.

This is a diagnostic HAP, not a working BanG Dream port. The original APK is unchanged. Build and Run in DevEco using legitimate automatic signing; do not disclose private keys or passwords.

Verified on MatePad mini, HDC API26/AArch64:
- Signed installation and application launch.
- Blue GLESv3 surface, real HUAWEI/Maleoon 910 renderer and pixel readback 20/89/128/255.
- Project-owned OHOS payload and freestanding Android-target ELF constructor/integer/double tests.
- Android-target libc-import guest: malloc/free/memcpy/strlen/snprintf, expected result 0xb478961e475221f0. This uses the OH SDK libc as a link-time symbol library, not an NDK-built Bionic library.
- Audible pulse confirmed by user; OHAudio fast stream observed. Neither buffer duration nor requested fast mode measures actual latency.
- Event-driven touch diagnostics and thread-safe UI callbacks. Counts only cover delivered events; logs may throttle and UI renders may coalesce. Diagnostic logging is not the final game hot path.

Trace original SDK copies the three unchanged APK DEX files into filesDir/sdkdex and invokes libsdktrace.so on an async worker. Each isolated original applicationAttach/getInstance stage has a 100000-instruction limit and 256-frame limit. Unsupported APIs explicitly fail; no guest native loader, network login, signature bypass or success callback is provided. Full output is filesDir/sdk-trace.txt. Read the active bundle name from the built HAP because the user may change it.

The first actual SDK report is ../sdk-runtime-spike/device-report-first.txt. Original bytecode loaded and executed but stopped at Void.TYPE and CopyOnWriteArrayList construction; SDK initialization and login are NOT verified. See ../device-validation.md for device evidence.

The interpreter derives from pinned MPL-2.0 Lunaria sources; provenance, source modifications and reproducible build are in ../sdk-runtime-spike/. License/provenance are also packaged under rawfile/sdktrace-license. Original DEX files come from the user-provided APK.

The IDE builds the complete HAP. Full Hvigor subprocess tasks in the agent environment have encountered EPERM, while direct SDK clang++ ARM64 compilation/linking succeeds. Direct C++ compilation is not whole-HAP verification. No SDK/system libc is packaged. guest-abi/ and ../abi-runtime-probe/ document the two guest tests.
