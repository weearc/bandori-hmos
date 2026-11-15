#pragma once
#include "dvm/dvm_internal.h"
bool prefs_runtime_configure(const char *sdkdir);
void prefs_runtime_vm_start(struct dvm *);
bool prefs_runtime_call(struct dvm *,const char *,const char *,const char *,dvm_ref,const union dvm_value *,int,union dvm_value *);
bool prefs_runtime_selftest(struct dvm *);
