#ifndef WINDOW_ATTRIBUTES_H
#define WINDOW_ATTRIBUTES_H
#include "dvm/dvm.h"
#include <stdbool.h>
/* Install field metadata before any LayoutParams instances are allocated.
 * One live VM; shutdown after all callers have stopped, before VM destroy. */
bool window_attributes_vm_start(struct dvm *vm);
void window_attributes_vm_shutdown(struct dvm *vm);
/* Return the stable guest LayoutParams for this Window. No host policy is
 * applied; setAttributes and actual cutout handling require separate support. */
dvm_ref window_attributes_get(struct dvm *vm,dvm_ref window);
typedef int (*window_flags_host)(void *user,int flags,int mask);
void sdk_set_window_flags_host(window_flags_host callback,void *user);
/* Current bridge, for selftests that must install a deterministic host and
 * restore the real one afterwards. */
window_flags_host window_attributes_get_flags_host(void **user);
bool window_attributes_set_flags(struct dvm *,dvm_ref,int flags,int mask);
bool window_attributes_selftest(struct dvm *vm);
#endif

