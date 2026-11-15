#pragma once
#include "dvm/dvm_internal.h"
void throwable_reference_vm_start(struct dvm*);
bool throwable_reference_call(struct dvm*,const char*,const char*,const char*,dvm_ref,const union dvm_value*,int,union dvm_value*);
bool throwable_reference_selftest(struct dvm*);
bool throwable_reference_stack_string(struct dvm*,dvm_ref,union dvm_value*);
