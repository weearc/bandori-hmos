#ifndef CONCURRENT_RUNTIME_H
#define CONCURRENT_RUNTIME_H

#include <stdbool.h>
#include "dvm/dvm.h"

/* Real java.util.concurrent subset for the guest DEX:
 *
 *   java/util/concurrent/ConcurrentLinkedQueue
 *     <init>()V, offer, add, poll, peek, size, isEmpty, contains, remove,
 *     clear, iterator, toArray
 *   java/util/concurrent/atomic/AtomicReference
 *     <init>()V, <init>(Object), get, set, getAndSet, compareAndSet,
 *     lazySet, toString
 *   java/util/concurrent/Semaphore
 *     <init>(I)V, <init>(IZ)V, acquire()V, acquire(I)V, release()V,
 *     release(I)V, tryAcquire()Z, tryAcquire(J,TimeUnit)Z,
 *     tryAcquire(I,TimeUnit)Z, drainPermits()I, availablePermits()I,
 *     getQueueLength()I, hasQueuedThreads()Z, isFair()Z
 *
 * The queue is a real FIFO protected by a pthread mutex, and its iterator is
 * weakly consistent (a creation-time snapshot, so concurrent modification
 * never throws ConcurrentModificationException).  Semaphore waiters use a
 * per-object pthread mutex/condvar and drop the VM GIL while blocked; the
 * fair constructor flag is recorded for isFair() but scheduling is
 * deliberately non-fair (release() broadcasts and waiters race for permits).
 * Unknown classes, methods and signatures return false so the dispatcher can
 * report MISSING_API instead of fabricating a result. */
void concurrent_runtime_vm_start(struct dvm *vm);
void concurrent_runtime_vm_end(struct dvm *vm);
bool concurrent_runtime_call(struct dvm *vm, const char *class_name,
                             const char *method, const char *sig, dvm_ref self,
                             const union dvm_value *args, int nargs,
                             union dvm_value *out);
bool concurrent_runtime_selftest(struct dvm *vm);

#endif
