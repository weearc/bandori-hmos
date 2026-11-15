#ifndef ENUM_RUNTIME_H
#define ENUM_RUNTIME_H
#include "dvm/dvm_internal.h"
bool enum_runtime_call(struct dvm *vm, const char *c, const char *m,
                      const char *sig, dvm_ref self,
                      const union dvm_value *a, int nargs,
                      union dvm_value *out);
#endif
