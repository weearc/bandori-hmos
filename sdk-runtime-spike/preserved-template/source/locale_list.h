#pragma once
#include <stddef.h>
#include "dvm/dvm_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Host bridge: tags is a comma-separated BCP-47 list such as
 * "zh-Hans-CN,en-US".  NULL or an empty string removes the host snapshot and
 * makes Configuration.getLocales() fall back to the VM's Locale.getDefault().
 * Invalid tags are rejected without changing the previous snapshot. */
bool locale_runtime_set_system_locales(const char *tags);
/* Copies the current canonical comma-separated snapshot into tags.  Returns
 * whether a host snapshot is present.  The buffer is always NUL terminated. */
bool locale_runtime_get_system_locales(char *tags, size_t cap);

bool locale_runtime_vm_start(struct dvm *v);
bool locale_runtime_call(struct dvm *v, const char *c, const char *m,
                         const char *sig, dvm_ref self,
                         const union dvm_value *a, int n,
                         union dvm_value *out);
bool locale_runtime_selftest(struct dvm *v);

/* Configuration helpers used by platform_runtime.c. */
bool locale_runtime_configuration_locales(struct dvm *v, dvm_ref configuration,
                                          dvm_ref *out);
bool locale_runtime_configuration_copy_locales(struct dvm *v, dvm_ref dst,
                                               dvm_ref src);
bool locale_runtime_configuration_clear_locales(struct dvm *v,
                                                dvm_ref configuration);

#ifdef __cplusplus
}
#endif
