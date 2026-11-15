#ifndef LAYOUT_PARAMS_RUNTIME_H
#define LAYOUT_PARAMS_RUNTIME_H
#include "dvm/dvm.h"
#include <stdbool.h>
bool layout_params_runtime_vm_start(struct dvm *);
void layout_params_runtime_vm_shutdown(struct dvm *);
bool layout_params_runtime_call(struct dvm *,const char *,const char *,const char *,dvm_ref,const union dvm_value *,int,union dvm_value *);
bool layout_params_runtime_valid(struct dvm *,dvm_ref);
bool layout_params_runtime_selftest(struct dvm *);
#endif
