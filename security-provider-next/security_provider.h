#ifndef SECURITY_PROVIDER_H
#define SECURITY_PROVIDER_H
#include <stdbool.h>
#include "dvm/dvm.h"

/* Call vm_end before destroying the VM; the registry pins its provider. */
void security_provider_vm_start(struct dvm *vm);
void security_provider_vm_end(struct dvm *vm);
bool security_provider_call(struct dvm *vm, const char *class_name,
                            const char *method, const char *sig, dvm_ref self,
                            const union dvm_value *args, int nargs,
                            union dvm_value *out);
bool security_provider_selftest(struct dvm *vm);
#endif
