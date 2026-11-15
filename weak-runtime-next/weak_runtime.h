#pragma once
#include <stdbool.h>
#include "dvm/dvm.h"
struct dvm;
union dvm_value;
#ifdef __cplusplus
extern "C" {
#endif
void weak_runtime_vm_start(struct dvm *v);
void weak_runtime_vm_end(struct dvm *v);
bool weak_runtime_gc_selftest(struct dvm *v);
bool weak_runtime_call(struct dvm *v,const char *c,const char *m,const char *sig,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out);
bool weak_runtime_static(struct dvm *v,const char *c,const char *f,const char *t,union dvm_value *o);
dvm_ref weak_runtime_make(struct dvm *v,dvm_ref referent);
#ifdef __cplusplus
}
#endif
