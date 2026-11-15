#pragma once
#include "dvm/dvm_internal.h"
bool file_runtime_configure(const char *);
void file_runtime_vm_start(struct dvm *);
bool file_runtime_selftest(struct dvm *);
bool file_runtime_call(struct dvm *,const char *,const char *,const char *,dvm_ref,const union dvm_value *,int,union dvm_value *);
