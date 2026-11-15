#ifndef DISPLAY_RUNTIME_H
#define DISPLAY_RUNTIME_H
#include "dvm/dvm.h"
#include <stdbool.h>
/* One live VM. Call start with DVM lock held; shutdown only after in-flight
 * callbacks have completed and before destroying VM. */
bool display_runtime_vm_start(struct dvm *vm);
void display_runtime_vm_shutdown(struct dvm *vm);
/* Stable guest Display for this VM; it is an identity, not a cached rotation. */
dvm_ref display_runtime_default(struct dvm *vm);
bool display_runtime_call(struct dvm *vm,const char *owner,const char *method,
 const char *sig,dvm_ref receiver,const union dvm_value *args,int nargs,union dvm_value *out);
/* Return 0 only after obtaining a real Android-compatible rotation 0/1/2/3.
 * Marshal to host UI thread where needed. callback and user must outlive all
 * calls and the VM. No fallback is manufactured when host is unavailable. */
typedef int (*display_rotation_host)(void *user,int *rotation);
void sdk_set_display_rotation_host(display_rotation_host callback,void *user);
bool display_runtime_selftest(struct dvm *vm);
#endif
