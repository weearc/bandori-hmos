#pragma once
#include "dvm/dvm_internal.h"
bool package_metadata_vm_start(struct dvm *);
bool package_metadata_set_host_native_library_dir(struct dvm *,const char *path);
bool package_metadata_call(struct dvm *,const char *,const char *,const char *,dvm_ref,const union dvm_value *,int,union dvm_value *);
