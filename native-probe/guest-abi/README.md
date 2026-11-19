# Android-target freestanding ELF probe

`guest.c` is entirely project-owned code. `build.ps1` uses the LLVM toolchain bundled with DevEco with `--target=aarch64-linux-android26 -nostdlib` and outputs the prebuilt library under `entry/libs/arm64-v8a`, which Hvigor's local native-library processing reads. It has no libc or other DT_NEEDED dependency. ELF metadata is System V AArch64 DYN and does not itself prove Android origin; the compiler command records the target. Constructor, integer and floating-point calls are checked via dlopen/dlsym from the HarmonyOS host.

A passing result establishes only this small ELF's loading and calls on the device, not Android Bionic, pthread, JNI, or game compatibility. No Android NDK was found in the common local SDK paths, so no Bionic-linked second layer is included. Rebuild the HAP and inspect the archive for the guest library before device tests.
