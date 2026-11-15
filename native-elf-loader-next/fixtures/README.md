# Nested ELF loader fixtures

Build `leaf.c` as `libnested_leaf.so` and `caller.c` as `libnested_caller.so`, targeting `aarch64-linux-ohos`. Package both in the HAP library directory. Load the caller through the adapter and invoke the exported `nested_caller_run` with the full path to `libnested_leaf.so`.

Both libraries import `__errno` directly and preserve its prior value. This intentionally exercises the adapter's existing `__errno` shim; neither fixture defines or aliases the missing system symbol. The caller performs nested `dlopen`/`dlsym`/`dlclose` calls only when `nested_caller_run` is called.

As of 72db the leaf also imports the Bionic `_ctype_` pointer object and 15 Android media-format key pointer objects. `leaf_ctype_check` independently verifies the EOF slot and all 256 character classifications; `leaf_media_keys_check` reads and compares all key strings. This checks actual ELF data relocations and pointer indirection, without implying that media decoding works. Compile with the candidate `source` include path (contains `bionic_media_keys.inc`) as done by `build_tls_candidate.py`. The production ctype table derives from AOSP/OpenBSD; its copyright and license are preserved in `../bionic_ctype_data.h`, with pinned provenance in the Unity research directory.

The run checks local `RTLD_NOW` and `RTLD_LAZY` opens, reuse of the same leaf mapping, one constructor execution, exported function lookup and return value, one-shot `dlerror` consumption, and rejection of lookup through a closed handle. `nested_caller_run` returns zero on success:

| Code | Meaning |
| --- | --- |
| 0 | All checks passed |
| 1 | Null leaf path |
| 2 | Either nested open failed |
| 3 | A required leaf symbol was not found |
| 4 | The leaf function returned the wrong value |
| 5 | Leaf constructor did not run exactly once |
| 6 | Missing-symbol lookup did not produce a nonempty error |
| 7 | `dlerror` did not consume the error |
| 8 | A nested close failed |
| 9 | Lookup through the closed handle was accepted |
| 10 | Bionic ctype pointer/table check failed |
| 11 | Media-format key pointer/string checks failed |
