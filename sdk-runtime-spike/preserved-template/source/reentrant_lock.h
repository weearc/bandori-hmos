#pragma once

#include "dvm/dvm_internal.h"

/* java.util.concurrent.locks.ReentrantLock, backed by one POSIX recursive
 * mutex plus an owner thread token and a hold count per lock object.  The
 * mutex provides real mutual exclusion and blocking; `owner`/`hold` provide
 * the Java-visible state that a native pthread mutex does not expose.
 *
 * `c` is the class the call was declared on (which may be the Lock
 * interface), `m`/`sig` name the method.  Returns false only when this
 * runtime does not own the call, so the strict-runtime dispatch chain can
 * continue to the next implementation.  A handled call that ends in a Java
 * exception returns true with vm->exception set. */
bool reentrant_lock_call(struct dvm *vm, const char *c, const char *m,
                         const char *sig, dvm_ref self,
                         const union dvm_value *args, int nargs,
                         union dvm_value *out);

/* Registers the builtin class and its method table on a new VM. */
bool reentrant_lock_vm_start(struct dvm *vm);

/* Real-thread checks for the runtime selftest group. */
bool reentrant_lock_selftest(struct dvm *vm);
