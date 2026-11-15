#pragma once
#include "dvm/dvm_internal.h"
void discovery_runtime_vm_start(struct dvm *);
void discovery_runtime_vm_end(struct dvm *);
bool discovery_runtime_call(struct dvm *,const char *,const char *,const char *,dvm_ref,const union dvm_value *,int,union dvm_value *);
bool discovery_runtime_static(struct dvm *,const char *,const char *,const char *,union dvm_value *);
bool discovery_runtime_selftest(struct dvm *);
