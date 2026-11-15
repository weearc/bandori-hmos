#pragma once
#include "dvm/dvm_internal.h"

bool ssl_runtime_call(struct dvm *, const char *, const char *, const char *, dvm_ref,
                     const union dvm_value *, int, union dvm_value *);
void ssl_runtime_vm_start(struct dvm *);
void ssl_runtime_cleanup(void);
bool ssl_runtime_selftest(struct dvm *);
