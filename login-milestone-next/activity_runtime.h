#ifndef ACTIVITY_RUNTIME_H
#define ACTIVITY_RUNTIME_H
#include "dvm/dvm_internal.h"
void activity_runtime_set_host(int (*mount)(void *, int), void *user);
void sdk_set_activity_host(int (*mount)(void *, int), void *user);
void activity_runtime_vm_start(struct dvm *v);
bool activity_runtime_call(struct dvm *v,const char *c,const char *m,const char *sig,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out);
#endif
