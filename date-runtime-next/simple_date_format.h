#pragma once

#include "date_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

bool simple_date_format_call(struct dvm *v, const char *c, const char *m,
                             const char *sig, dvm_ref self,
                             const union dvm_value *a, int nargs,
                             union dvm_value *out);
bool simple_date_format_selftest(struct dvm *v);

#ifdef __cplusplus
}
#endif
