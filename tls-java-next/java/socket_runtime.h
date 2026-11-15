#pragma once
#include "dvm/dvm_internal.h"
bool socket_runtime_call(struct dvm *,const char *,const char *,const char *,dvm_ref,const union dvm_value *,int,union dvm_value *);
void socket_runtime_vm_start(struct dvm *);
bool socket_runtime_selftest(struct dvm *);
void socket_runtime_cleanup(void);
void socket_runtime_set_external(bool allowed);
bool socket_runtime_external_allowed(void);
