#ifndef SYSTEM_RUNTIME_H
#define SYSTEM_RUNTIME_H

#include <stdbool.h>
#include "dvm/dvm.h"

/* Real java.lang.System and android.util.Base64 subset backed by the DVM's
 * own array representation.  Unsupported methods stay unhandled so the
 * dispatcher can report MISSING_API instead of fabricating a result. */
void system_runtime_vm_start(struct dvm *vm);
void system_runtime_vm_end(struct dvm *vm);
bool system_runtime_call(struct dvm *vm, const char *class_name,
                         const char *method, const char *sig, dvm_ref self,
                         const union dvm_value *args, int nargs,
                         union dvm_value *out);
bool system_runtime_selftest(struct dvm *vm);

#endif
