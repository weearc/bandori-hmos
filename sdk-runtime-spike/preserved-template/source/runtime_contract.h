#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct dvm;

/*
 * Install the runtime-level class contracts that cannot be expressed by the
 * application DEX alone.  Called once for every VM instance after the runtime
 * installers have run.
 *
 * The current contract is the java.lang.String -> java.lang.CharSequence
 * interface edge.  java.lang.String is a boot-class placeholder in this VM
 * (the application DEX never defines it), so its interface table is empty even
 * though a real device reports String as a CharSequence.  The contract is
 * idempotent: a second call for the same VM does not append a duplicate edge.
 *
 * Ownership: the ifaces array remains owned by struct dvm_class and is freed
 * exactly once by dvm_destroy().  This function hands the reallocated block
 * back to the class and keeps no alias; on allocation failure the old block
 * and class state are left untouched.
 *
 * Returns false when the edge could not be verified or installed.  The VM may
 * still be usable; callers must not treat this as a reason to destroy it.
 */
bool runtime_contract_vm_start(struct dvm *vm);

#ifdef __cplusplus
}
#endif
