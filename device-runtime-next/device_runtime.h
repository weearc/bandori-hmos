#pragma once
#include <stdbool.h>
struct dvm;
union dvm_value;
#ifdef __cplusplus
extern "C" {
#endif
/* Thread-safe snapshot setters.  Values are copied; NULL or "" clears a
 * field.  They may be called before or after the VM starts. */
void device_runtime_set_snapshot(const char *brand, const char *manufacturer,
                                 const char *model, const char *device,
                                 const char *product, const char *os_full);
/* Real host ABI list as CSV, e.g. "arm64-v8a,armeabi-v7a,armeabi".
 * Empty/NULL clears it, so android/os/Build.SUPPORTED_ABIS stays missing
 * (fail-closed) instead of returning a made-up list. */
void device_runtime_set_abis(const char *csv);
bool device_runtime_static(struct dvm *v, const char *c, const char *f,
                           const char *t, union dvm_value *out);
/* Injects a synthetic snapshot, verifies the read paths, then restores the
 * caller's snapshot.  Must be wired into a VM-start selftest harness before
 * it runs; this function does not install itself. */
bool device_runtime_selftest(struct dvm *v);
#ifdef __cplusplus
}
#endif
