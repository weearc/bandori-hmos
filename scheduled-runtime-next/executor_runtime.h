#pragma once
#include "dvm/dvm_internal.h"

bool executor_runtime_call(struct dvm *, const char *, const char *, const char *,
                           dvm_ref, const union dvm_value *, int,
                           union dvm_value *);
void executor_runtime_shutdown(struct dvm *, unsigned);
bool executor_runtime_selftest(struct dvm *, unsigned *);
