# window_runtime host fixture test report

## Scope

`window_runtime_test.c` is a host-only C fixture for the controlled `window_runtime` bridge. It does not use OHOS APIs, an ArkTS VM, a device, or any real native window. The original fixture covers host ABI validation, no-host/no-current behavior, dimensions and references, stale and arbitrary tokens, the 64-slot boundary, 200 allocation/release reuse cycles, stale host generations, and callback synchronization while a query and final release overlap. It also checks host replacement is rejected during acquire, closing, and host release callbacks.

## Attempted execution

WSL distribution discovery (`wsl -l -q`) returned `ArchLinux`. The requested strict host compile/run command was attempted:

```sh
cd <local-path>
cc -std=c11 -Wall -Wextra -Werror -pthread -Inative-window-next \
  native-window-next/window_runtime.c \
  native-window-next/tests/window_runtime_test.c \
  -o /tmp/window_runtime_test && /tmp/window_runtime_test
```

That first attempt failed because `cc` is not installed. Follow-up checks also found no `gcc` or `clang` in `ArchLinux`. This limitation was subsequently resolved without installing packages: the root agent used the Windows DevEco SDK clang targeting x86_64-linux-gnu with the installed ArchLinux glibc headers/libraries as its sysroot.

The successful strict compile used `-std=gnu11 -Wall -Wextra -Werror -pthread -fuse-ld=lld -nostdlib`, sysroot `\\wsl.localhost\ArchLinux`, its `usr/lib/crt1.o`, `crti.o`, `crtn.o`, `-lc -lpthread`, and dynamic linker `/lib64/ld-linux-x86-64.so.2`. The output `batch-validation/window_runtime_test-linux` was executed under WSL ArchLinux and returned `window_runtime_test: PASS`. The actual output is archived in `batch-validation/codex-window-runtime-unit-linux.txt`.

An OHOS executable build was also attempted: compilation passed, but direct execution from `/data/local/tmp` was denied by device policy. This is not counted as an OHOS fixture PASS; policy was not modified. The independently built HAP's real-window integration probe passed on the device as 72dc.

## Result and limits

The original controlled fixture passed its included cases, including query/final-release overlap and host replacement rejection while callbacks are in flight. This result predates the geometry test additions below.

## Geometry fixture extension

The fixture now adds a host `configure` callback that mutates real fixture dimensions and format state. It checks successful nonzero resize and readback; `0,0,format=0` restoration to fixture base dimensions/format; mixed-zero, negative dimensions, and negative format rejection; unsupported format failure without state mutation; stale host generation error propagation; missing configure callback returning `-ENOTSUP`; and that a blocked configure callback pins the OH reference until the concurrent final logical release can complete.

The root agent subsequently compiled the integrated host ABI 2 revision with strict warnings and ran it under WSL ArchLinux: `window_runtime_test: PASS`. This is a separate execution from the earlier four-function fixture. Binary: `batch-validation/window_runtime_geometry_test-linux`; actual output: `batch-validation/codex-window-runtime-geometry-unit-linux.txt`. It covers the added geometry cases and configure/final-release overlap.

This fixture remains separate from the real-window HAP/device evidence. Its callbacks are controlled fakes and cannot establish actual OHOS geometry behavior, full Android `ANativeWindow` compatibility, Unity initialization, or frame presentation. The geometry real-window evidence is recorded separately in the 72dd manifest and raw report; nonzero resize and explicit formats 1/2/4 have fixture coverage only in this phase.
