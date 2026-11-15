#ifndef JCE_RUNTIME_H
#define JCE_RUNTIME_H

#include <stdbool.h>
#include "dvm/dvm.h"

/* Real javax.crypto subset backed by mbedtls AES.  The module handles only
 * the transformations the guest DEX actually requests:
 *   AES/CBC/PKCS5Padding, AES/ECB/PKCS5Padding and the PKCS7 spellings.
 * Unsupported transformations and methods stay unhandled so the dispatcher can
 * report MISSING_API instead of returning a fabricated object or plaintext. */
void jce_runtime_vm_start(struct dvm *vm);
void jce_runtime_vm_end(struct dvm *vm);
bool jce_runtime_call(struct dvm *vm, const char *class_name,
                      const char *method, const char *sig, dvm_ref self,
                      const union dvm_value *args, int nargs,
                      union dvm_value *out);
bool jce_runtime_selftest(struct dvm *vm);

#endif
