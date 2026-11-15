#pragma once
#include "dvm/dvm_internal.h"
bool asset_reader_vm_start(struct dvm *);
bool asset_reader_call(struct dvm *,const char *,const char *,const char *,dvm_ref,const union dvm_value *,int,union dvm_value *);
