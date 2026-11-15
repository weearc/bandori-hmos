#pragma once
#include "dvm/dvm_internal.h"
void set_runtime_vm_start(struct dvm*);
bool set_runtime_call(struct dvm*,const char*,const char*,const char*,dvm_ref,const union dvm_value*,int,union dvm_value*);
bool set_runtime_selftest(struct dvm*);
