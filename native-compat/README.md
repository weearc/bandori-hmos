# Native compatibility subset

`python native-compat/build.py` compiles `libgame_android_log.so` for OHOS ARM64.
Its SONAME is `liblog.so`. It is now packaged under that name in the probe HAP.
The original `libmain.so` successfully loaded on the device with this dependency
present; its `JNI_OnLoad` was resolved but not called. That verifies dependency
loading, not all logging functions. No Android game library is opened by the
bridge itself. Direct logging and assertion behavior remain untested on device.

Implemented exports: `__android_log_write`, `__android_log_buf_write`,
`__android_log_print`, `__android_log_vprint`, `__android_log_assert`.
These cover the four different Android logging names in the actual game-library
missing-symbol report, plus the assertion entry point. Main-buffer output uses HiLog with severity mapping;
non-main buffers are rejected with `-EPERM`. Verbose maps to debug. Unknown
priorities and null text/format return `-EINVAL`. Successful writes return 1;
failed writes return `-EPERM`. Assertion aborts the process after logging.
Formatted messages are truncated to 4095 bytes. Payloads use HiLog's private
string formatting; the original message is not treated as a HiLog format.

This subset does not implement Android logd, Android property-based log filters,
logger/aborter callbacks, structured log messages or the complete modern liblog
API. It must not be described as full liblog compatibility.

The ABI and return-value reference is the
[official Android logging API](https://developer.android.com/ndk/reference/group/logging).
HiLog signatures and success semantics were checked against the locally installed
Harmony API26 SDK `sysroot/usr/include/hilog/log.h`.

Build validation: `-Wall -Wextra -Werror`, `--no-undefined`, actual ARM64 clang
compile/link succeeded. No original game initialization or assertion execution
has occurred.

## Extracted-file asset backend

The same build script compiles `assets.c` into `libgame_assets.so`, independently
of the log bridge. The backend opens real files beneath a host-supplied directory
and implements the ARM64 AAsset read/seek/length/file-descriptor/mmap subset.
Every path component is opened beneath the directory with `O_NOFOLLOW`;
absolute paths, traversal and symlinks are rejected. Read state uses `pread`,
so handing a file descriptor to a caller does not alter AAsset's read position.
Each extracted asset occupies its complete file: returned FD start is zero.

This is not an APK/ZIP decoder. Asset extraction and resource provisioning are
not yet integrated. `AAssetManager_fromJava` and directory enumeration are not
implemented; the real JNI object-to-manager association remains pending.
Android ARM64 uses 64-bit file offsets; the compile checks this assumption.
Individual AAsset handles require caller serialization. Input files must remain
immutable while open; the host must not overwrite/truncate mapped files.

Compile/link passed with the same strict flags. Actual resource reading has not
yet been tested on device. ABI reference:
[Android asset API](https://developer.android.com/ndk/reference/group/asset).
