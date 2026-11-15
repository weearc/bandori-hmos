#pragma once
#include "dvm/dvm_internal.h"
bool platform_runtime_vm_start(struct dvm *);
bool platform_runtime_call(struct dvm *, const char *, const char *, const char *, dvm_ref, const union dvm_value *, int, union dvm_value *);
bool platform_runtime_selftest(struct dvm *);
