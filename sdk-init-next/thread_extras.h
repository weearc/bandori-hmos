#pragma once
#include "dvm/dvm_internal.h"
#include "gc_runtime.h"
void thread_extras_vm_start(struct dvm *);
void thread_extras_vm_end(struct dvm *);
bool thread_extras_gc_visit_owner(struct dvm *,dvm_ref,dvm_gc_visit_fn,void *);
bool thread_extras_call(struct dvm *,const char *,const char *,const char *,dvm_ref,const union dvm_value *,int,union dvm_value *);
bool thread_extras_selftest(struct dvm *);
bool thread_extras_gc_selftest(struct dvm *);
