#pragma once
#include "dvm/dvm_internal.h"
bool bundle_runtime_call(struct dvm *,const char *,const char *,const char *,dvm_ref,const union dvm_value *,int,union dvm_value *);
/* Creates an empty android.os.Bundle whose payload belongs to bundle_runtime.
 * Intended for host-side modules (Intent) that need an extras container.
 * Returns 0 with a VM exception when allocation or class lookup fails. */
dvm_ref bundle_runtime_new(struct dvm *);
bool bundle_runtime_selftest(struct dvm *);
/* Read-only callback diagnostics: keys/types, token length and prefix only. */
bool bundle_runtime_trace_keys(struct dvm *,dvm_ref,int);
