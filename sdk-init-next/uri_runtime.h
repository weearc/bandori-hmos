#pragma once
#include "dvm/dvm_internal.h"
/* Minimal android/net/Uri string parsing. No URL decoding is performed. */
bool uri_runtime_call(struct dvm *vm, const char *cls, const char *method,
 const char *sig, dvm_ref self, const union dvm_value *args, int nargs,
 union dvm_value *out);
bool uri_runtime_selftest(struct dvm *vm);
/* Included by uri_runtime_selftest; separately callable by an existing VM. */
bool uri_runtime_builder_selftest(struct dvm *vm);
/* True only for references created by this module's android/net/Uri payload.
 * Read-only, never throws, never dereferences foreign payloads. */
bool uri_runtime_is_uri(struct dvm *vm, dvm_ref ref);
