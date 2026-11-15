# Android looper shim change

Updated `android_looper_shim.c/.h` to preserve negative timeout as an indefinite
eventfd wait, return wake only after consuming a real eventfd counter, and keep
finite timeout duration against one monotonic deadline through `EINTR`.

Looper handles are opaque monotonically assigned integer tokens. The shim
compares tokens while holding its mutex and never dereferences caller supplied
handles, so foreign and malformed handles are rejected and a released stale
handle cannot alias a later slot occupant. The token counter stops allocation
before `uintptr_t` exhaustion rather than wrapping and reusing an identity.
Thread ownership is a separate reference released by a pthread TLS destructor;
explicit acquire/release references keep the eventfd alive after the owning
thread exits. Repeated prepare on the same thread returns the same handle and
does not add another reference. Wake lookup/write and final close are serialized
under the same mutex.

Host regression source: `tests/android_looper_test.c`. It covers repeated
prepare, empty-thread lookup behavior through worker setup, indefinite poll and
wake, finite timeout, invalid handles, more than 64 prepare/exit cycles, and an
external reference surviving owner-thread exit. The parent compiled it with
strict warnings using Windows SDK clang and the ArchLinux glibc sysroot, then
executed it under WSL: `android_looper_test: PASS`. The actual log is
`batch-validation/codex-looper-audit-unit-linux.txt`. This is a controlled host
test, not proof of Unity's Swappy/Choreographer runtime behavior. Parent review
also added pthread_setspecific failure cleanup and EINTR-safe wake writes.
