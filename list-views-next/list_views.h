#pragma once
#include "dvm/dvm_internal.h"
void list_views_vm_start(struct dvm *);
bool list_views_call(struct dvm *,const char *,const char *,const char *,dvm_ref,const union dvm_value *,int,union dvm_value *);
bool list_views_selftest(struct dvm *);
