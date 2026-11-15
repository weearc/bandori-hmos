#pragma once
#include "dvm/dvm_internal.h"
void runtime_extra_vm_start(struct dvm *);
bool runtime_extra_call(struct dvm *,const char *,const char *,const char *,dvm_ref,const union dvm_value *,int,union dvm_value *);
bool runtime_extra_selftest(struct dvm *,unsigned *);
