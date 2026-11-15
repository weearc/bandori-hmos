#pragma once
#include "dvm/dvm_internal.h"

/* Real android.system.Os subset over host descriptors.  This round implements
 * only Os.posix_fallocate(FileDescriptor, long, long), the method the BLKV
 * mapped-file path calls.  Other Os methods stay unhandled so the dispatcher
 * keeps reporting MISSING_API instead of inventing buffers or sizes.
 *
 * android_os_configure() derives the same <private_base>/guest-files root as
 * file_runtime.c / random_access_file.c and also ensures the RandomAccessFile
 * module is configured, so the selftest can open a real guest file and obtain
 * its real FileDescriptor.  Configuration is optional for dispatch itself. */

bool android_os_configure(const char *sdk_dir);
void android_os_vm_start(struct dvm *vm);
void android_os_vm_end(struct dvm *vm);
bool android_os_selftest(struct dvm *vm);
bool android_os_call(struct dvm *vm, const char *cls, const char *method,
                     const char *sig, dvm_ref self,
                     const union dvm_value *args, int nargs,
                     union dvm_value *out);
