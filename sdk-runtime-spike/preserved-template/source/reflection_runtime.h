#pragma once
#include "dvm/dvm_internal.h"
void reflection_runtime_vm_start(struct dvm *);
void reflection_runtime_vm_end(struct dvm *);
bool reflection_runtime_call(struct dvm *, const char *, const char *, const char *, dvm_ref, const union dvm_value *, int, union dvm_value *);
bool reflection_runtime_selftest(struct dvm *);
