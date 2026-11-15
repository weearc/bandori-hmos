#pragma once
#include "dvm/dvm_internal.h"

/* Real java.io.RandomAccessFile over the guest file roots that
 * file_runtime.c owns.  This module is compiled into the runtime by the main
 * agent; it never invents a successful file operation. */

bool random_access_file_configure(const char *sdk_dir);
void random_access_file_vm_start(struct dvm *vm);
bool random_access_file_selftest(struct dvm *vm);
bool random_access_file_call(struct dvm *vm, const char *cls, const char *method,
                             const char *sig, dvm_ref self,
                             const union dvm_value *args, int nargs,
                             union dvm_value *out);

/* Real host descriptor behind a RandomAccessFile's FileDescriptor or
 * FileChannel object, or -1 when the object is not one of ours or is closed.
 * Used by a future android/system/Os bridge; no descriptor is duplicated. */
int random_access_file_fd_of(struct dvm *vm, dvm_ref ref);
