#pragma once
#include "dvm/dvm_internal.h"
bool java_log_runtime_call(struct dvm*,const char*,const char*,const char*,dvm_ref,const union dvm_value*,int,union dvm_value*);
