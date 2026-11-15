# Android property shim audit

## AOSP contract checked

The public Bionic header defines the deprecated `__system_property_read(pi,
name, value)` entry point alongside `PROP_NAME_MAX` (32) and
`PROP_VALUE_MAX` (92). The function has no buffer-length parameters, so its
legacy ABI requires a 32-byte name destination and a separate 92-byte value
destination. They are not interchangeable. The AOSP implementation copies a
property name with a 32-byte bound and returns the value length; current Bionic
also logs that long names/values should use
`__system_property_read_callback()` instead. See the [Bionic public header](https://android.googlesource.com/platform/bionic/+/refs/heads/main/libc/include/sys/system_properties.h)
and [Bionic implementation](https://android.googlesource.com/platform/bionic/+/refs/heads/main/libc/system_properties/system_properties.cpp).

## Changes and semantics

- `find` still consults only `getenv`; it does not invent build properties.
- A property name/value longer than this shim's 91-byte stored payload is
  rejected. It is not silently shortened into a different property key/value.
- `read` accepts only exact slot-start addresses in the static table. It checks
  integer address bounds and slot alignment before any slot dereference, then
  checks `used` under the mutex. This rejects arbitrary, interior, and
  in-range unpublished slot addresses.
- Published entries never move or get reused. The mutex covers publication and
  the entire read copy, so callers see a coherent value and a stable
  process-lifetime object.
- The legacy name output is at most 31 bytes plus NUL (matching Bionic's
  `PROP_NAME_MAX` behavior); the value output is at most 91 bytes plus NUL. A
  longer supported stored name stays distinct during `find`, while legacy
  `read` returns its Bionic-style shortened name. `name == NULL` is supported.

## Validation and limits

Parent validation: strict clang host compilation and actual WSL execution
passed (`android_property_test: PASS`), recorded in
`batch-validation/codex-property-audit-unit-linux.txt`. AOSP reference bodies
and hashes are saved under
`batch-validation/unity-bootstrap-20261109/aosp-runtime-audit/`.

`tests/android_property_test.c` is a host C test with a `sdk_trace_fprintf`
stub. It covers real environment-backed find/read, missing properties,
arbitrary/interior/unpublished pointers, separate name/value canaries, long
inputs, and concurrent find/read.

This is a bounded compatibility shim, not Bionic's property area: only 16
entries can be published, the environment is sampled once at first find, and
the deprecated read ABI necessarily cannot verify caller buffer sizes. The
caller must provide the AOSP-required 32-byte name and 92-byte value buffers.
Environment mutation concurrent with `getenv` is outside this shim's
synchronization contract. The test's unpublished-slot pointer construction
tracks the current private slot layout and should be updated if that layout is
changed.
