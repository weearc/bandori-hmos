#pragma once
#include <stdbool.h>
#include "dvm/dvm.h"
struct dvm;
union dvm_value;
#ifdef __cplusplus
extern "C" {
#endif
bool observable_runtime_call(struct dvm *v, const char *c, const char *m,
                             const char *sig, dvm_ref self,
                             const union dvm_value *a, int n,
                             union dvm_value *out);
#ifdef __cplusplus
}
#endif
