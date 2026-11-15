# Unity initialization control flow near 0x9e3714

## Root verified entry and caller chain

Original ELF .eh_frame_hdr at VA/file0x2d2f9c has the enclosing sorted entry0x9e22b8, next0x9e385c, FDE0x402540. This unwind boundary matches the prologue at0x9e22b8. Evidence: batch-validation/codex-unity-init-unwind.txt and codex-unity-init-full.asm.txt.

Scanning aligned direct B/BL instructions in executable PT_LOAD found BL0x9e4304 ->0x9e22b8. Caller0x9e420c branches there when flag0. BL0x9fdbf8 calls0x9e420c; original R_AARCH64_RELATIVE JNI registration0x17fb818 identifies nativeRender()Z entry0x9fdba4. Thus first nativeRender can initialize the engine; flag0/nativeResume no-op does not establish an initializer stall. Indirect callers are not covered by this scan.

Actual72hz engine-probe candidate/PID segment confirms original S0.b -> S0.h ->nativeRender entering1, then Failed to read assets/bin/Data/unity_app_guid. This supersedes the old claim that nativeRender never ran. GUID reader0x9e20c8 uses0x8d8880 and virtual-filesystem open/read calls0x8f4884/0x8f490c; actual provider remains unresolved.

Original APK contains a36-byte GUID. Device base.apk was an11,978,999-byte5-entry subset without GUID. A complete byte-identical original APK was deployed and independently fetched/hash-verified BEFORE startup, but Index.ets then overwrote it from the old HAP rawfile subset. Consequently those runs do not establish behavior with the full APK. For72id the embedded rawfile has been replaced by the unchanged original APK; verify both HAP member and POST-startup device SHA before assessing resource-interface behavior.

Readonly static inspection of `rea-analysis-20261104/libunity.so` (AArch64) using the DevEco SDK `llvm-objdump`. No original bytes were changed and the library was not executed. This note is limited to the supplied init-prefix disassembly and direct decoded instructions around the target; caller enumeration is not complete.

## Address/function boundary

The requested store at VA `0x9e3714` is `strb w19, [x9,#2328]`, where `x9` is based at `0x1885000` and `w19` was set to 1 at `0x9e36e8`. Thus this is a byte flag at VA `0x1885918`. Disassembly labels this region under nearest symbol `UnitySwappy_injectTracer` at `0x9d3550`, but the target is well inside a large function, not its entry. The function's frame is visible by `0x9e3000` (large SP frame, saved registers); supplied dump truncation before its prologue means exact start was not established here.

## Pre-store paths and returns

The region `0x9e346c` onward contains preceding operations and conditionals, not a single JNI error return:

* `0x9e346c` calls `0x9f0194`; if bit 0 of `w0` is clear it calls `0x9e0a30` with `(0,0)`, then proceeds.
* At `0x9e34c0`, helper `0x850988` returns a bit-tested status. Clear bit takes a branch through `0xa00e1c`, then helpers `0x9d3584`/`0x9da18c`; both paths rejoin at `0x9e34f8`.
* `0x9e35ac` calls `0x9e0f90`; clear bit similarly takes `0xa00e1c` plus the two helpers, then rejoins at `0x9e35dc`.
* `0x9e35dc` calls `0x850c18(0)`; clear bit takes the same apparent reporting/helper sequence, then rejoins at `0x9e3610`.
* `0x9e361c` and `0x9e3620` call helpers, followed by `0x830d94` at `0x9e362c`; `tbz w0,#0` branches directly to `0x9e36e8`. Set bit takes additional work: initialize stack object, call `0x5e7988`, then `0x9e40a8`, and use `0x9cbfe0`. At `0x9e36d4`, `cbz w8` branches to `0x9e3788`; otherwise bit 0 of `w19` may branch to `0x9e36e8`.
* `0x9e36e8` sets `w19=1`, stores two global bytes (`0x1885920`, `0x1885921`), calls `0x9e795c`, sets another byte (`0x1885921`), calls `0x9e4100`, then reaches target flag store at `0x9e3714`. Therefore these instructions are on the path through this block, but the earlier `tbz` and branch at `0x9e36d4` converge here.
* `0x9e3718` and `0x9e3734` conditionally call `0x738a98` for cleanup/error state; they do not branch to a distinct failure exit. At `0x9e3754`, a stack guard comparison branches to `__stack_chk_fail` if unequal. Normal epilogue at `0x9e3764\xa8C0x9e3784` restores registers and returns.
* `0x9e3788` is a path that logs/cleans a stack object then either returns to `0x9e36dc` or goes to `0x9e36e8`, depending on bit 0 of `w19`.

## Interpretation limits

The nearby calls to `0xa00e1c` are consistent with exception/reporting handling based on adjacent JNI-focused work, but this disassembly alone does not identify them as a user-visible alert or prove they are triggered by JNI failure. Several status values are bit-tested and helper semantics are unresolved. No evidence here establishes that a JNI failure causes an early return before `0x9e3714`; the shown ordinary flow instead converges on the flag store, with a normal epilogue afterward. A stack-check failure is an explicit non-returning error path, distinct from JNI handling.

The broad name `UnitySwappy_injectTracer` is only the nearest symbol annotation and should not be taken as a semantic function name for this initialization logic. Exact entry, direct callers, string values passed to logging/reporting, and the behavior of callees remain to be established.
