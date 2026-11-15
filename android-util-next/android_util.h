#pragma once
#include "dvm/dvm_internal.h"

/* android.util.Pair and android.content.ContentValues, backed by the real
 * object heap and the collections module's HashMap. */
void android_util_vm_start(struct dvm *vm);
void android_util_vm_end(struct dvm *vm);
bool android_util_call(struct dvm *vm, const char *cls, const char *method,
                       const char *sig, dvm_ref self,
                       const union dvm_value *args, int nargs,
                       union dvm_value *out);
bool android_util_selftest(struct dvm *vm);
