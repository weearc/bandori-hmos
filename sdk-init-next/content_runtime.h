#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Kept self-contained so the NAPI/ArkTS shell can include this file to mirror
 * the host ABI without pulling in the VM headers. */
struct dvm;
union dvm_value;
typedef uint32_t dvm_ref;

/* HarmonyOS @ohos.settings host bridge. The historical Global/fixture path
 * below retains this policy; Secure/System now additionally support a bounded
 * VM-local override table and default/empty unknown reads (settings_compat.inc).
 * Local writes never claim to alter real host settings and emit one bounded
 * settings_stub diagnostic per operation category.
 *
 * @ohos.settings is an ArkTS API with no native C entry point, so the shell
 * installs these callbacks (settings.getValueSync/setValueSync bound to the
 * application Context) through content_runtime_set_host().  The host is the
 * only source of real system state.  The in-memory table is a fixture store
 * used exclusively by the selftest or an explicit fixture session; production
 * execution never reads or writes it.  A host get must be callable
 * synchronously from the runtime worker and must not re-enter the runtime.
 *
 * get: 1 = found and NUL-terminated in value, 0 = key absent, -2 = permission
 *      denied, any other negative = backend failure.  capacity is
 *      CONTENT_SETTINGS_HOST_VALUE_MAX.  An absent key (or no installed host)
 *      means the caller's default is returned.  A negative status is
 *      fail-closed: reads throw java/lang/SecurityException (permission) or
 *      java/lang/IllegalStateException (backend) and never fall back to a
 *      fixture value or a fabricated default.
 * put: 0 = success, nonzero = failure.  A NULL value deletes the key.  With no
 *      installed host the runtime returns false and traces it; it never claims
 *      a system setting changed.
 * The host struct and user pointer must stay valid until replaced or removed;
 * the callback receives every read and write and must not call back into the
 * runtime.  Install the bridge before the SDK run (or on the runtime worker).
 * getValueSync has no presence signal: pass an impossible sentinel default and
 * translate an unchanged sentinel to 0 (absent), so the runtime does not
 * mistake a host default for the real system state.  Map permission denial to
 * -2 and other backend failures to another negative status.
 *
 * Suggested ArkTS mapping: Settings.Global -> settings.general keys through
 * the default-domain getValueSync/setValueSync overload, and
 * "airplane_mode_on" -> settings.general.AIRPLANE_MODE_STATUS, whose "1"/"0"
 * string is the same shape Android stores.  DEVICE_SHARED and USER_PROPERTY
 * need ohos.permission.MANAGE_SETTINGS, as does setValueSync itself; use them
 * only when the shell holds the permission, otherwise report absent/failure.
 * A host write failure is returned to the guest as false, never mirrored into
 * the fixture table, so the runtime does not fake system state. */
#define CONTENT_SETTINGS_HOST_VALUE_MAX 4096
enum content_settings_domain {
 CONTENT_SETTINGS_GLOBAL=0,
 CONTENT_SETTINGS_SYSTEM=1,
 CONTENT_SETTINGS_SECURE=2
};
typedef struct content_settings_host {
 int (*get)(void *host, int domain, const char *name, char *value, size_t capacity);
 int (*put)(void *host, int domain, const char *name, const char *value);
} content_settings_host;

#ifdef __cplusplus
extern "C" {
#endif
/* Installed by the NAPI shell before the SDK run.  Passing NULL removes the
 * bridge: reads then return the caller default and put* reports false, unless
 * an explicit fixture session is active. */
void content_runtime_set_host(const content_settings_host *host, void *user);
/* Explicit fixture mode for selftest harnesses only.  Enabling clears the
 * in-memory table and makes it readable/writable when no host is installed;
 * disabling gates the table off again (production reads see the caller
 * default and put* returns false).  The selftest parks the live host bridge
 * before enabling it: fixtures must never be mixed with an installed
 * @ohos.settings bridge or with business execution. */
void content_runtime_set_fixture_mode(bool enabled);
/* Stable dlsym alias, following sdk_set_activity_host/battery_runtime_set_host. */
__attribute__((visibility("default"))) void sdk_set_settings_host(const content_settings_host *host, void *user);
void content_runtime_vm_start(struct dvm *vm);
bool content_runtime_call(struct dvm *vm, const char *cls, const char *method,
 const char *sig, dvm_ref self, const union dvm_value *args, int nargs,
 union dvm_value *out);
bool content_runtime_selftest(struct dvm *vm);
/* Exact discovery/dispatch inventory for the local Secure/System policy. */
bool content_runtime_class_supported(const char *cls);
bool content_runtime_method_supported(const char *cls,const char *method,const char *sig);
bool content_runtime_constant(struct dvm *vm,const char *cls,const char *field,const char *type,union dvm_value *out);
#ifdef __cplusplus
}
#endif
