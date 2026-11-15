#pragma once
#include "dvm/dvm_internal.h"
bool sqlite_runtime_call(struct dvm *, const char *, const char *, const char *, dvm_ref, const union dvm_value *, union dvm_value *);
