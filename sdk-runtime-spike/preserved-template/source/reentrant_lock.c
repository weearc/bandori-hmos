/*
 * java.util.concurrent.locks.ReentrantLock and its Condition subset, backed by
 * real POSIX primitives.
 *
 * Each lock object owns one PTHREAD_MUTEX_RECURSIVE mutex.  The mutex supplies
 * real blocking and mutual exclusion; `owner` (host thread token) and `hold`
 * expose Java's ownership and hold-count semantics, which a pthread mutex
 * cannot answer without disturbing its recursion count.  All three are
 * updated while the caller holds the VM GIL, but waiters read them after
 * dropping the GIL, so they are atomics.
 *
 * Fairness: the boolean constructor argument is stored and reported by
 * isFair(), but waiting threads are served by the host mutex and are not
 * queued in strict Java fair order.  This is documented as an approximation,
 * not claimed as fair scheduling.
 *
 * Condition: a real per-condition pthread_cond_t and a waiter list.  await()
 * atomically registers on the condition while holding the lock, releases the
 * recursive mutex completely (hold count N means N unlocks), waits, then
 * reacquires it N times before returning.  signal()/signalAll() mark waiters
 * and broadcast; every waiter re-checks its own flag, so spurious wakeups and
 * the choice of the woken thread cannot lose a signal.  condition variables
 * are never paired with the recursive mutex directly (POSIX leaves that
 * undefined): the condition has its own normal gate mutex and the lock mutex
 * is released before waiting.
 *
 * Interrupts: the candidate runtime's dvm__thread_take_interrupt() is a stub
 * that always answers false, so lockInterruptibly()/await() are structured to
 * observe an interrupt but cannot currently be woken by one.  The wait slices
 * still poll it so a real implementation of the hook becomes effective
 * without changes here.
 */

#include "reentrant_lock.h"
#include "java_basics.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>
#include <stdatomic.h>

extern int sdk_trace_fprintf(FILE *, const char *, ...);

#define RL_NAME "java/util/concurrent/locks/ReentrantLock"
#define RL_DESC "Ljava/util/concurrent/locks/ReentrantLock;"
#define RL_LOCK_NAME "java/util/concurrent/locks/Lock"
#define RL_LOCK_DESC "Ljava/util/concurrent/locks/Lock;"
#define RL_COND_NAME "java/util/concurrent/locks/Condition"
#define RL_COND_DESC "Ljava/util/concurrent/locks/Condition;"
#define RL_COND_SIG "()Ljava/util/concurrent/locks/Condition;"

#define RL_POLL_MS 20
#define RL_COND_POLL_MS 100

typedef struct rl_state {
   struct dvm *vm;
   pthread_mutex_t mutex;      /* PTHREAD_MUTEX_RECURSIVE */
   _Atomic uint64_t owner;     /* host thread token; 0 means unlocked */
   _Atomic uint32_t hold;      /* recursive lock count */
   bool fair;                  /* remembered property, not scheduling order */
} rl_state;

struct rl_waiter {
   struct rl_waiter *next;
   bool signaled;
};

typedef struct rl_cond_state {
   struct dvm *vm;
   dvm_ref lock_ref;
   pthread_cond_t cond;
   pthread_mutex_t gate;       /* protects waiters and the wait entry window */
   struct rl_waiter *waiters;
} rl_cond_state;

/* --- small helpers ------------------------------------------------------ */

/* The owner token is the host thread, the same identity the existing
 * ReentrantReadWriteLock implementation uses.  0 is reserved for "free". */
static uint64_t rl_thread_token(void)
{
   uint64_t token = (uint64_t)(uintptr_t)pthread_self();
   return token ? token : 1;
}

static uint64_t rl_now_ns(clockid_t clock)
{
   struct timespec t;
   if (clock_gettime(clock, &t) != 0) return 0;
   return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static struct timespec rl_realtime_after_ms(unsigned ms)
{
   struct timespec t;
   clock_gettime(CLOCK_REALTIME, &t);
   t.tv_sec += (time_t)(ms / 1000u);
   t.tv_nsec += (long)(ms % 1000u) * 1000000L;
   if (t.tv_nsec >= 1000000000L) {
      ++t.tv_sec;
      t.tv_nsec -= 1000000000L;
   }
   return t;
}

static uint64_t rl_sat_add(uint64_t a, uint64_t b)
{
   uint64_t r = a + b;
   return r < a ? UINT64_MAX : r;
}

static void rl_sleep_ms(unsigned ms)
{
   struct timespec t;
   t.tv_sec = (time_t)(ms / 1000u);
   t.tv_nsec = (long)(ms % 1000u) * 1000000L;
   while (nanosleep(&t, &t) != 0 && errno == EINTR) {
   }
}

/* --- object payloads ---------------------------------------------------- */

static void rl_state_destroy(void *p)
{
   rl_state *st = p;
   if (!st) return;
   pthread_mutex_destroy(&st->mutex);
   free(st);
}

static void rl_cond_destroy(void *p)
{
   rl_cond_state *cs = p;
   if (!cs) return;
   pthread_cond_destroy(&cs->cond);
   pthread_mutex_destroy(&cs->gate);
   free(cs);
}

static rl_state *rl_state_of(struct dvm *v, dvm_ref self)
{
   struct dvm_object *o = dvm__obj(v, self);
   if (!o || o->data_destroy != rl_state_destroy || !o->data) return NULL;
   rl_state *st = o->data;
   return st->vm == v ? st : NULL;
}

static rl_cond_state *rl_cond_of(struct dvm *v, dvm_ref self)
{
   struct dvm_object *o = dvm__obj(v, self);
   if (!o || o->data_destroy != rl_cond_destroy || !o->data) return NULL;
   rl_cond_state *cs = o->data;
   return cs->vm == v ? cs : NULL;
}

/* Resolve the lock a condition was created from.  The lock object can be
 * destroyed only at VM teardown; checking the class and payload identity
 * makes a torn-down lock an explicit error instead of a stale pointer. */
static rl_state *rl_lock_from_cond(struct dvm *v, rl_cond_state *cs)
{
   if (!cs || cs->vm != v) return NULL;
   struct dvm_object *o = dvm__obj(v, cs->lock_ref);
   if (!o || !o->cls || !o->cls->name || strcmp(o->cls->name, RL_NAME))
      return NULL;
   if (o->data_destroy != rl_state_destroy || !o->data) return NULL;
   rl_state *st = o->data;
   return st->vm == v ? st : NULL;
}

static bool rl_is_lock_receiver(struct dvm *v, dvm_ref self)
{
   struct dvm_object *o = dvm__obj(v, self);
   return o && o->cls && o->cls->name && !strcmp(o->cls->name, RL_NAME);
}

/* --- acquire / release primitives --------------------------------------- */

static void rl_note_acquired(rl_state *st)
{
   atomic_store_explicit(&st->owner, rl_thread_token(), memory_order_release);
   atomic_fetch_add_explicit(&st->hold, 1u, memory_order_acq_rel);
}

/* 1 acquired, 0 busy, -1 host error. */
static int rl_try_acquire(rl_state *st)
{
   int rc = pthread_mutex_trylock(&st->mutex);
   if (rc == 0) {
      rl_note_acquired(st);
      return 1;
   }
   if (rc == EBUSY || rc == EDEADLK) return 0;
   return -1;
}

/* Release every recursion level of the lock the current thread owns.  The
 * metadata is cleared before the mutex is unlocked so another thread can
 * never observe a new owner and then have this thread overwrite it. */
static bool rl_drop_all(struct dvm *v, rl_state *st)
{
   (void)v;
   uint64_t me = rl_thread_token();
   uint32_t hold = atomic_load_explicit(&st->hold, memory_order_acquire);
   if (atomic_load_explicit(&st->owner, memory_order_acquire) != me ||
       hold == 0)
      return false;
   atomic_store_explicit(&st->hold, 0, memory_order_release);
   atomic_store_explicit(&st->owner, 0, memory_order_release);
   for (uint32_t i = 0; i < hold; ++i) {
      /* An owner thread unlocking a live recursive mutex cannot fail; if it
       * somehow does there is no safe recovery, and the next Java-visible
       * operation will report the broken state. */
      (void)pthread_mutex_unlock(&st->mutex);
   }
   return true;
}

/* Block until the mutex is acquired, dropping the GIL around the host wait.
 * A bounded slice keeps VM shutdown and (once implemented) interrupts
 * observable even while the lock is contended. */
static int rl_wait_acquire(struct dvm *v, rl_state *st, bool interruptible,
                           bool timed, uint64_t deadline_ns)
{
   for (;;) {
      if (dvm_is_stopping(v)) {
         if (interruptible)
            dvm__throw(v, "java/lang/InterruptedException",
                       "ReentrantLock wait during VM shutdown");
         else
            dvm__throw(v, "java/lang/IllegalStateException",
                       "ReentrantLock wait during VM shutdown");
         return -1;
      }
      if (interruptible && dvm__thread_take_interrupt(v)) {
         dvm__throw(v, "java/lang/InterruptedException",
                    "ReentrantLock wait interrupted");
         return -1;
      }
      unsigned ms = RL_POLL_MS;
      if (timed) {
         uint64_t now = rl_now_ns(CLOCK_MONOTONIC);
         if (now >= deadline_ns) return 0;
         uint64_t remaining = deadline_ns - now;
         uint64_t slice_ns = (uint64_t)RL_POLL_MS * 1000000ull;
         if (remaining <= slice_ns) {
            ms = (unsigned)((remaining + 999999ull) / 1000000ull);
            if (ms == 0) ms = 1;
         }
      }
      unsigned depth = dvm_gil_unlock_all(v);
      struct timespec until = rl_realtime_after_ms(ms);
      int rc = pthread_mutex_timedlock(&st->mutex, &until);
      dvm_gil_relock(v, depth);
      if (rc == 0) {
         rl_note_acquired(st);
         return 1;
      }
      if (rc == ETIMEDOUT || rc == EINTR) continue;
      dvm__throw(v, "java/lang/IllegalStateException",
                 "ReentrantLock wait failed (%d)", rc);
      return -1;
   }
}

/* Reacquire a lock after Condition.await(), restoring the saved hold count. */
static int rl_reacquire_hold(struct dvm *v, rl_state *st, uint32_t hold)
{
   int got = rl_wait_acquire(v, st, false, false, 0);
   if (got != 1) return got;
   for (uint32_t i = 1; i < hold; ++i) {
      int rc = pthread_mutex_trylock(&st->mutex);
      if (rc != 0) {
         dvm__throw(v, "java/lang/IllegalStateException",
                    "ReentrantLock condition reacquire failed (%d)", rc);
         return -1;
      }
      atomic_fetch_add_explicit(&st->hold, 1u, memory_order_acq_rel);
   }
   return 1;
}

/* --- lock methods ------------------------------------------------------- */

static bool rl_construct(struct dvm *v, dvm_ref self, bool fair,
                         union dvm_value *out)
{
   (void)out;
   struct dvm_object *o = dvm__obj(v, self);
   if (!o) {
      dvm__throw(v, "java/lang/NullPointerException",
                 "ReentrantLock constructor receiver");
      return true;
   }
   if (o->data) {
      dvm__throw(v, "java/lang/IllegalStateException",
                 "ReentrantLock already initialized");
      return true;
   }
   rl_state *st = calloc(1, sizeof *st);
   if (!st) {
      dvm__throw(v, "java/lang/OutOfMemoryError", "ReentrantLock");
      return true;
   }
   pthread_mutexattr_t attr;
   int rc = pthread_mutexattr_init(&attr);
   if (rc == 0) {
      rc = pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
      if (rc == 0) rc = pthread_mutex_init(&st->mutex, &attr);
      (void)pthread_mutexattr_destroy(&attr);
   }
   if (rc != 0) {
      free(st);
      dvm__throw(v, "java/lang/IllegalStateException",
                 "ReentrantLock mutex (%d)", rc);
      return true;
   }
   st->vm = v;
   st->fair = fair;
   atomic_store(&st->owner, 0);
   atomic_store(&st->hold, 0);
   o->data = st;
   o->data_destroy = rl_state_destroy;
   return true;
}

static bool rl_lock_op(struct dvm *v, rl_state *st, bool interruptible)
{
   /* AQS checks the interrupt status before the first acquisition attempt. */
   if (interruptible && dvm__thread_take_interrupt(v)) {
      dvm__throw(v, "java/lang/InterruptedException",
                 "ReentrantLock.lockInterruptibly interrupted");
      return true;
   }
   int got = rl_try_acquire(st);
   if (got == 1) return true;
   if (got < 0) {
      dvm__throw(v, "java/lang/IllegalStateException",
                 "ReentrantLock lock failed");
      return true;
   }
   (void)rl_wait_acquire(v, st, interruptible, false, 0);
   return true;
}

static bool rl_try_lock_bool(struct dvm *v, rl_state *st,
                             union dvm_value *out)
{
   int got = rl_try_acquire(st);
   if (got < 0) {
      dvm__throw(v, "java/lang/IllegalStateException",
                 "ReentrantLock.tryLock failed");
      return true;
   }
   out->i = got == 1;
   return true;
}

static bool rl_try_lock_timed(struct dvm *v, rl_state *st,
                              const union dvm_value *a, int n,
                              union dvm_value *out)
{
   if (n < 2) {
      dvm__throw(v, "java/lang/IllegalStateException",
                 "ReentrantLock.tryLock arguments");
      return true;
   }
   if (a[1].l == 0) {
      dvm__throw(v, "java/lang/NullPointerException",
                 "ReentrantLock.tryLock unit");
      return true;
   }
   uint64_t unit_ns = 0;
   if (!java_basics_timeunit_nanos(v, a[1].l, &unit_ns)) {
      dvm__throw(v, "java/lang/IllegalArgumentException",
                 "ReentrantLock.tryLock: unsupported TimeUnit");
      return true;
   }
   int64_t timeout = a[0].j;
   if (dvm__thread_take_interrupt(v)) {
      dvm__throw(v, "java/lang/InterruptedException",
                 "ReentrantLock.tryLock interrupted");
      return true;
   }
   int got = rl_try_acquire(st);
   if (got == 1) {
      out->i = 1;
      return true;
   }
   if (got < 0) {
      dvm__throw(v, "java/lang/IllegalStateException",
                 "ReentrantLock.tryLock failed");
      return true;
   }
   if (timeout <= 0) {
      /* Java still performs the immediate acquisition attempt and does not
       * wait; that attempt already failed above. */
      out->i = 0;
      return true;
   }
   uint64_t nanos;
   if ((uint64_t)timeout > UINT64_MAX / unit_ns)
      nanos = UINT64_MAX;
   else
      nanos = (uint64_t)timeout * unit_ns;
   uint64_t deadline = rl_sat_add(rl_now_ns(CLOCK_MONOTONIC), nanos);
   int r = rl_wait_acquire(v, st, true, true, deadline);
   if (r < 0) return true;
   out->i = r == 1;
   return true;
}

static bool rl_unlock_op(struct dvm *v, rl_state *st, union dvm_value *out)
{
   (void)out;
   uint64_t me = rl_thread_token();
   uint64_t owner = atomic_load_explicit(&st->owner, memory_order_acquire);
   uint32_t hold = atomic_load_explicit(&st->hold, memory_order_acquire);
   if (owner != me || hold == 0) {
      dvm__throw(v, "java/lang/IllegalMonitorStateException",
                 "ReentrantLock.unlock: not held by current thread");
      return true;
   }
   if (hold == 1) {
      atomic_store_explicit(&st->hold, 0, memory_order_release);
      atomic_store_explicit(&st->owner, 0, memory_order_release);
   } else {
      atomic_store_explicit(&st->hold, hold - 1u, memory_order_release);
   }
   int rc = pthread_mutex_unlock(&st->mutex);
   if (rc != 0) {
      /* An owner unlock of a valid recursive mutex does not fail; restore the
       * Java-visible state rather than leaving it desynchronized. */
      if (hold == 1) {
         atomic_store_explicit(&st->owner, me, memory_order_release);
         atomic_store_explicit(&st->hold, 1u, memory_order_release);
      } else {
         atomic_store_explicit(&st->hold, hold, memory_order_release);
      }
      dvm__throw(v, "java/lang/IllegalStateException",
                 "ReentrantLock.unlock failed (%d)", rc);
   }
   return true;
}

/* --- Condition ---------------------------------------------------------- */

static bool rl_new_condition(struct dvm *v, dvm_ref lock_ref,
                             union dvm_value *out)
{
   struct dvm_class *cls = dvm__class_by_desc(v, RL_COND_DESC);
   if (!cls) {
      dvm__throw(v, "java/lang/NoClassDefFoundError", RL_COND_NAME);
      return true;
   }
   dvm_ref ref = dvm_new_object(v, cls);
   if (!ref) {
      dvm__throw(v, "java/lang/OutOfMemoryError",
                 "ReentrantLock.newCondition");
      return true;
   }
   rl_cond_state *cs = calloc(1, sizeof *cs);
   if (!cs) {
      dvm__discard_temporary(v, ref);
      dvm__throw(v, "java/lang/OutOfMemoryError",
                 "ReentrantLock.newCondition state");
      return true;
   }
   int rc = pthread_cond_init(&cs->cond, NULL);
   if (rc == 0) {
      rc = pthread_mutex_init(&cs->gate, NULL);
      if (rc != 0) pthread_cond_destroy(&cs->cond);
   }
   if (rc != 0) {
      free(cs);
      dvm__discard_temporary(v, ref);
      dvm__throw(v, "java/lang/IllegalStateException",
                 "Condition primitives (%d)", rc);
      return true;
   }
   cs->vm = v;
   cs->lock_ref = lock_ref;
   struct dvm_object *o = dvm__obj(v, ref);
   if (!o) {
      rl_cond_destroy(cs);
      dvm__throw(v, "java/lang/OutOfMemoryError", "Condition object");
      return true;
   }
   o->data = cs;
   o->data_destroy = rl_cond_destroy;
   out->l = ref;
   return true;
}

static bool rl_need_lock_held(struct dvm *v, rl_state *st, const char *what)
{
   if (atomic_load_explicit(&st->owner, memory_order_acquire) !=
           rl_thread_token() ||
       atomic_load_explicit(&st->hold, memory_order_acquire) == 0) {
      dvm__throw(v, "java/lang/IllegalMonitorStateException",
                 "Condition.%s: lock not held by current thread", what);
      return false;
   }
   return true;
}

static bool rl_cond_signal(struct dvm *v, rl_cond_state *cs, bool all,
                           union dvm_value *out)
{
   (void)out;
   rl_state *st = rl_lock_from_cond(v, cs);
   if (!st) {
      dvm__throw(v, "java/lang/IllegalStateException",
                 "Condition lock is unavailable");
      return true;
   }
   if (!rl_need_lock_held(v, st, all ? "signalAll" : "signal")) return true;
   pthread_mutex_lock(&cs->gate);
   bool any = false;
   if (all) {
      for (struct rl_waiter *w = cs->waiters; w; w = w->next) {
         w->signaled = true;
         any = true;
      }
   } else {
      for (struct rl_waiter *w = cs->waiters; w; w = w->next) {
         if (!w->signaled) {
            w->signaled = true;
            any = true;
            break;
         }
      }
   }
   if (any) pthread_cond_broadcast(&cs->cond);
   pthread_mutex_unlock(&cs->gate);
   return true;
}

/* Fully release, wait, reacquire, restore hold count.  `nanos` is only
 * consulted when `timed`; a timed await with nanos <= 0 still performs the
 * release/reacquire, matching Java's observe-no-wait path. */
static bool rl_cond_await(struct dvm *v, rl_cond_state *cs, bool timed,
                          uint64_t nanos, union dvm_value *out)
{
   rl_state *st = rl_lock_from_cond(v, cs);
   if (!st) {
      dvm__throw(v, "java/lang/IllegalStateException",
                 "Condition lock is unavailable");
      return true;
   }
   if (dvm__thread_take_interrupt(v)) {
      dvm__throw(v, "java/lang/InterruptedException",
                 "Condition.await interrupted");
      return true;
   }
   if (!rl_need_lock_held(v, st, "await")) return true;
   uint32_t hold = atomic_load_explicit(&st->hold, memory_order_acquire);
   if (hold == 0) {
      dvm__throw(v, "java/lang/IllegalMonitorStateException",
                 "Condition.await: lock not held");
      return true;
   }

   if (timed && nanos == 0) {
      if (!rl_drop_all(v, st)) {
         dvm__throw(v, "java/lang/IllegalMonitorStateException",
                    "Condition.await: lock not held");
         return true;
      }
      if (rl_reacquire_hold(v, st, hold) < 0) return true;
      out->i = 0;
      return true;
   }

   struct rl_waiter node;
   node.next = NULL;
   node.signaled = false;
   pthread_mutex_lock(&cs->gate);
   node.next = cs->waiters;
   cs->waiters = &node;
   if (!rl_drop_all(v, st)) {
      struct rl_waiter **pp = &cs->waiters;
      while (*pp && *pp != &node) pp = &(*pp)->next;
      if (*pp) *pp = node.next;
      pthread_mutex_unlock(&cs->gate);
      dvm__throw(v, "java/lang/IllegalMonitorStateException",
                 "Condition.await: lock not held");
      return true;
   }
   /* The gate is held across the transition from "holds the lock" to "is
    * queued on the condvar": a signaler must acquire the lock first, so it
    * cannot signal before this thread is actually waiting. */
   unsigned depth = dvm_gil_unlock_all(v);
   uint64_t deadline =
      timed ? rl_sat_add(rl_now_ns(CLOCK_MONOTONIC), nanos) : 0;
   bool timedout = false;
   bool interrupted = false;
   bool stopped = false;
   int wait_error = 0;
   for (;;) {
      if (node.signaled) break;
      /* dvm_is_stopping() is a relaxed atomic read and needs no GIL.  Check
       * it before waiting and again before the GIL round-trip below, so VM
       * teardown cannot deadlock against a joiner that still holds the GIL. */
      if (dvm_is_stopping(v)) { stopped = true; break; }
      unsigned slice = RL_COND_POLL_MS;
      if (timed) {
         uint64_t now = rl_now_ns(CLOCK_MONOTONIC);
         if (now >= deadline) {
            timedout = true;
            break;
         }
         uint64_t remaining = deadline - now;
         uint64_t slice_ns = (uint64_t)RL_COND_POLL_MS * 1000000ull;
         if (remaining < slice_ns) {
            slice = (unsigned)((remaining + 999999ull) / 1000000ull);
            if (slice == 0) slice = 1;
         }
      }
      struct timespec until = rl_realtime_after_ms(slice);
      int rc = pthread_cond_timedwait(&cs->cond, &cs->gate, &until);
      if (node.signaled) break;
      if (rc != 0 && rc != ETIMEDOUT && rc != EINTR) {
         wait_error = rc;
         break;
      }
      if (dvm_is_stopping(v)) { stopped = true; break; }
      /* Take the GIL briefly to observe VM shutdown and interrupts.  The
       * gate is dropped first: a signaler holds the GIL while it wants the
       * gate, and taking the GIL while holding the gate would deadlock. */
      pthread_mutex_unlock(&cs->gate);
      dvm_gil_relock(v, depth);
      stopped = dvm_is_stopping(v);
      interrupted = dvm__thread_take_interrupt(v);
      depth = dvm_gil_unlock_all(v);
      pthread_mutex_lock(&cs->gate);
      if (stopped || interrupted) break;
   }
   struct rl_waiter **pp = &cs->waiters;
   while (*pp && *pp != &node) pp = &(*pp)->next;
   if (*pp) *pp = node.next;
   pthread_mutex_unlock(&cs->gate);
   dvm_gil_relock(v, depth);

   /* Java reacquires the lock before await() returns, including the timeout
    * and interrupt paths. */
   if (rl_reacquire_hold(v, st, hold) < 0) return true;
   if (stopped) {
      dvm__throw(v, interrupted ? "java/lang/InterruptedException"
                                : "java/lang/IllegalStateException",
                 "Condition.await: VM shutdown");
      return true;
   }
   if (interrupted) {
      dvm__throw(v, "java/lang/InterruptedException",
                 "Condition.await interrupted");
      return true;
   }
   if (wait_error) {
      dvm__throw(v, "java/lang/IllegalStateException",
                 "Condition.await failed (%d)", wait_error);
      return true;
   }
   out->i = node.signaled ? 1 : 0;
   (void)timedout;
   return true;
}

static bool rl_cond_call(struct dvm *v, const char *c, const char *m,
                         const char *sig, dvm_ref self,
                         const union dvm_value *a, int n,
                         union dvm_value *out)
{
   if (!c || strcmp(c, RL_COND_NAME) || !m || !sig) return false;
   bool await_void = !strcmp(m, "await") && !strcmp(sig, "()V");
   bool await_timed =
      !strcmp(m, "await") && !strcmp(sig, "(JLjava/util/concurrent/TimeUnit;)Z");
   bool signal = !strcmp(m, "signal") && !strcmp(sig, "()V");
   bool signal_all = !strcmp(m, "signalAll") && !strcmp(sig, "()V");
   if (!await_void && !await_timed && !signal && !signal_all) return false;

   struct dvm_object *o = dvm__obj(v, self);
   if (!o) {
      dvm__throw(v, "java/lang/NullPointerException", "Condition receiver");
      return true;
   }
   rl_cond_state *cs = rl_cond_of(v, self);
   if (!cs) {
      dvm__throw(v, "java/lang/IllegalStateException",
                 "Condition receiver is not a ReentrantLock Condition");
      return true;
   }
   if (signal || signal_all) return rl_cond_signal(v, cs, signal_all, out);

   if (await_void) return rl_cond_await(v, cs, false, 0, out);

   if (n < 2) {
      dvm__throw(v, "java/lang/IllegalStateException",
                 "Condition.await arguments");
      return true;
   }
   if (a[1].l == 0) {
      dvm__throw(v, "java/lang/NullPointerException",
                 "Condition.await unit");
      return true;
   }
   uint64_t unit_ns = 0;
   if (!java_basics_timeunit_nanos(v, a[1].l, &unit_ns)) {
      dvm__throw(v, "java/lang/IllegalArgumentException",
                 "Condition.await: unsupported TimeUnit");
      return true;
   }
   int64_t timeout = a[0].j;
   uint64_t nanos = 0;
   if (timeout > 0) {
      if ((uint64_t)timeout > UINT64_MAX / unit_ns)
         nanos = UINT64_MAX;
      else
         nanos = (uint64_t)timeout * unit_ns;
   }
   return rl_cond_await(v, cs, true, nanos, out);
}

/* --- dispatch ----------------------------------------------------------- */

static bool rl_lock_methods(struct dvm *v, const char *m, const char *sig,
                            dvm_ref self, const union dvm_value *a, int n,
                            union dvm_value *out)
{
   if (!m || !sig) return false;
   if (!strcmp(m, "<init>")) {
      if (!strcmp(sig, "()V")) return rl_construct(v, self, false, out);
      if (!strcmp(sig, "(Z)V")) {
         bool fair = n >= 1 && a[0].i != 0;
         return rl_construct(v, self, fair, out);
      }
      return false;
   }
   struct dvm_object *o = dvm__obj(v, self);
   if (!o) {
      dvm__throw(v, "java/lang/NullPointerException", "ReentrantLock receiver");
      return true;
   }
   rl_state *st = rl_state_of(v, self);
   if (!st) {
      dvm__throw(v, "java/lang/IllegalStateException",
                 "ReentrantLock constructor not called");
      return true;
   }
   if (!strcmp(m, "lock") && !strcmp(sig, "()V"))
      return rl_lock_op(v, st, false);
   if (!strcmp(m, "lockInterruptibly") && !strcmp(sig, "()V"))
      return rl_lock_op(v, st, true);
   if (!strcmp(m, "unlock") && !strcmp(sig, "()V"))
      return rl_unlock_op(v, st, out);
   if (!strcmp(m, "tryLock") && !strcmp(sig, "()Z"))
      return rl_try_lock_bool(v, st, out);
   if (!strcmp(m, "tryLock") &&
       !strcmp(sig, "(JLjava/util/concurrent/TimeUnit;)Z"))
      return rl_try_lock_timed(v, st, a, n, out);
   if (!strcmp(m, "isLocked") && !strcmp(sig, "()Z")) {
      out->i = atomic_load_explicit(&st->owner, memory_order_acquire) != 0;
      return true;
   }
   if (!strcmp(m, "isHeldByCurrentThread") && !strcmp(sig, "()Z")) {
      out->i = atomic_load_explicit(&st->owner, memory_order_acquire) ==
               rl_thread_token();
      return true;
   }
   if (!strcmp(m, "getHoldCount") && !strcmp(sig, "()I")) {
      out->i = atomic_load_explicit(&st->owner, memory_order_acquire) ==
                     rl_thread_token()
                  ? (int32_t)atomic_load_explicit(&st->hold,
                                                  memory_order_acquire)
                  : 0;
      return true;
   }
   if (!strcmp(m, "isFair") && !strcmp(sig, "()Z")) {
      out->i = st->fair ? 1 : 0;
      return true;
   }
   if (!strcmp(m, "newCondition") && !strcmp(sig, RL_COND_SIG))
      return rl_new_condition(v, self, out);
   return false;
}

static bool rl_lock_declared(const char *m, const char *sig)
{
   if (!m || !sig) return false;
   if (!strcmp(m, "lock") && !strcmp(sig, "()V")) return true;
   if (!strcmp(m, "lockInterruptibly") && !strcmp(sig, "()V")) return true;
   if (!strcmp(m, "unlock") && !strcmp(sig, "()V")) return true;
   if (!strcmp(m, "tryLock") && !strcmp(sig, "()Z")) return true;
   if (!strcmp(m, "tryLock") &&
       !strcmp(sig, "(JLjava/util/concurrent/TimeUnit;)Z"))
      return true;
   if (!strcmp(m, "newCondition") && !strcmp(sig, RL_COND_SIG)) return true;
   return false;
}

bool reentrant_lock_call(struct dvm *v, const char *c, const char *m,
                         const char *sig, dvm_ref self,
                         const union dvm_value *a, int n,
                         union dvm_value *out)
{
   if (!v || !c || !m || !sig) return false;
   if (!strcmp(c, RL_COND_NAME))
      return rl_cond_call(v, c, m, sig, self, a, n, out);
   if (!strcmp(c, RL_NAME))
      return rl_lock_methods(v, m, sig, self, a, n, out);
   if (!strcmp(c, RL_LOCK_NAME)) {
      /* The Lock interface is also owned by the ReentrantReadWriteLock
       * handler.  Claim it only for a live ReentrantLock receiver, so a
       * ReentrantReadWriteLock lock still reaches its own implementation. */
      if (!rl_is_lock_receiver(v, self)) return false;
      if (!rl_lock_declared(m, sig)) return false;
      return rl_lock_methods(v, m, sig, self, a, n, out);
   }
   return false;
}

static bool rl_builtin(struct dvm *v, dvm_ref self,
                       const union dvm_value *a, int n, union dvm_value *out)
{
   struct dvm_method *bm = dvm__builtin_method();
   if (!bm || !bm->cls || !bm->cls->name || !bm->name || !bm->sig)
      return false;
   return reentrant_lock_call(v, bm->cls->name, bm->name, bm->sig, self, a, n,
                              out);
}

static bool rl_cond_builtin(struct dvm *v, dvm_ref self,
                            const union dvm_value *a, int n,
                            union dvm_value *out)
{
   struct dvm_method *bm = dvm__builtin_method();
   if (!bm || !bm->cls || !bm->cls->name || !bm->name || !bm->sig)
      return false;
   return rl_cond_call(v, bm->cls->name, bm->name, bm->sig, self, a, n, out);
}

/* --- class installation ------------------------------------------------- */

static bool rl_install(struct dvm_class *c, int index, const char *name,
                       const char *sig, char ret, int args, int slots,
                       dvm_builtin_fn fn)
{
   struct dvm_method *m = &c->methods[index];
   m->cls = c;
   m->name = name;
   m->sig = strdup(sig);
   m->builtin = fn;
   m->ret_kind = ret;
   m->arg_count = args;
   m->arg_slots = slots;
   m->access = DEX_ACC_PUBLIC;
   return m->sig != NULL;
}

/* ReentrantLock implements java.util.concurrent.locks.Lock on a device.  The
 * application DEX declares the interface but defines no class for it, so a
 * builtin ReentrantLock would otherwise have no interface edge and
 * class_assignable() -- the sole test behind checkcast and instanceof --
 * would reject the app's `(Lock) reentrantLock` cast with a
 * ClassCastException.  Resolving the descriptor here can yield the boot
 * placeholder synthesized before thread_runtime_init() registers its own Lock
 * builtin; class_assignable() compares descriptors as well as pointers, so
 * the edge stays valid after that later registration. */
static bool rl_iface_present(const struct dvm_class *cls,
                             const struct dvm_class *iface)
{
   for (int i = 0; i < cls->nifaces; ++i) {
      struct dvm_class *have = cls->ifaces[i];
      if (have == iface) return true;
      if (have && have->desc && iface && iface->desc &&
          !strcmp(have->desc, iface->desc))
         return true;
   }
   return false;
}

static bool rl_install_lock_iface(struct dvm *v, struct dvm_class *lock)
{
   struct dvm_class *iface = dvm_find_class_or_external(v, RL_LOCK_NAME);
   if (!iface) {
      sdk_trace_fprintf(stderr,
                        "REENTRANT_LOCK FAIL lock_interface_unavailable\n");
      return false;
   }
   /* Idempotent: a repeated vm_start registers a fresh ReentrantLock class,
    * and an edge that already names this descriptor is not appended twice.
    * All state lives on the VM's own class objects, so VMs stay independent. */
   if (rl_iface_present(lock, iface)) return true;
   if (lock->nifaces < 0) {
      sdk_trace_fprintf(stderr, "REENTRANT_LOCK FAIL negative_iface_count\n");
      return false;
   }
   size_t count = (size_t)lock->nifaces + 1u;
   if (count > (size_t)INT32_MAX ||
       count > SIZE_MAX / sizeof *lock->ifaces) {
      sdk_trace_fprintf(stderr, "REENTRANT_LOCK FAIL iface_count_overflow\n");
      return false;
   }
   /* next_interpose.h routes realloc()/free() to the VM arena; dvm_destroy()
    * releases the final block through the same allocator.  On failure the old
    * block stays owned by the class, as runtime_contract.c relies on. */
   struct dvm_class **grown = realloc(lock->ifaces, count * sizeof *grown);
   if (!grown) {
      sdk_trace_fprintf(stderr, "REENTRANT_LOCK FAIL iface_realloc\n");
      return false;
   }
   grown[lock->nifaces] = iface;
   lock->ifaces = grown;
   lock->nifaces++;

   /* class_assignable() consults the edge; this flag keeps is_interface
    * truthful for metadata readers. */
   iface->is_interface = true;
   sdk_trace_fprintf(stderr,
                     "REENTRANT_LOCK Lock interface installed nifaces=%d\n",
                     lock->nifaces);
   return true;
}

bool reentrant_lock_vm_start(struct dvm *v)
{
   if (!v) return false;
   struct dvm_class *lock = dvm__register_builtin(v, RL_DESC);
   struct dvm_class *cond = dvm__register_builtin(v, RL_COND_DESC);
   if (!lock || !cond) {
      dvm__throw(v, "java/lang/OutOfMemoryError", "ReentrantLock classes");
      return false;
   }
   struct dvm_class *object = dvm__class_by_desc(v, "Ljava/lang/Object;");
   lock->super = object;
   lock->access |= DEX_ACC_PUBLIC;
   lock->init_state = 2;
   cond->super = object;
   cond->access |= DEX_ACC_PUBLIC | DEX_ACC_INTERFACE | DEX_ACC_ABSTRACT;
   cond->is_interface = true;
   cond->init_state = 2;

   lock->methods = calloc(12, sizeof *lock->methods);
   cond->methods = calloc(4, sizeof *cond->methods);
   if (!lock->methods || !cond->methods) {
      free(lock->methods);
      free(cond->methods);
      lock->methods = NULL;
      cond->methods = NULL;
      dvm__throw(v, "java/lang/OutOfMemoryError", "ReentrantLock methods");
      return false;
   }
   int n = 0;
   if (!rl_install(lock, n, "<init>", "()V", 'V', 0, 0, rl_builtin)) goto oom;
   ++n;
   if (!rl_install(lock, n, "<init>", "(Z)V", 'V', 1, 1, rl_builtin)) goto oom;
   ++n;
   if (!rl_install(lock, n, "lock", "()V", 'V', 0, 0, rl_builtin)) goto oom;
   ++n;
   if (!rl_install(lock, n, "lockInterruptibly", "()V", 'V', 0, 0,
                   rl_builtin))
      goto oom;
   ++n;
   if (!rl_install(lock, n, "unlock", "()V", 'V', 0, 0, rl_builtin)) goto oom;
   ++n;
   if (!rl_install(lock, n, "tryLock", "()Z", 'Z', 0, 0, rl_builtin)) goto oom;
   ++n;
   if (!rl_install(lock, n, "tryLock",
                   "(JLjava/util/concurrent/TimeUnit;)Z", 'Z', 2, 3,
                   rl_builtin))
      goto oom;
   ++n;
   if (!rl_install(lock, n, "isLocked", "()Z", 'Z', 0, 0, rl_builtin))
      goto oom;
   ++n;
   if (!rl_install(lock, n, "isHeldByCurrentThread", "()Z", 'Z', 0, 0,
                   rl_builtin))
      goto oom;
   ++n;
   if (!rl_install(lock, n, "getHoldCount", "()I", 'I', 0, 0, rl_builtin))
      goto oom;
   ++n;
   if (!rl_install(lock, n, "isFair", "()Z", 'Z', 0, 0, rl_builtin)) goto oom;
   ++n;
   if (!rl_install(lock, n, "newCondition", RL_COND_SIG, 'L', 0, 0, rl_builtin))
      goto oom;
   ++n;
   lock->nmethods = n;

   /* The builtin table above is what dispatches Lock.lock()/unlock()/...
    * for a ReentrantLock receiver; the edge installed here is only the type
    * relation.  thread_runtime_call() keeps owning every other Lock receiver
    * because reentrant_lock_call() claims the Lock declaration only for an
    * exact ReentrantLock receiver. */
   if (!rl_install_lock_iface(v, lock)) {
      if (!dvm_exception(v))
         dvm__throw(v, "java/lang/IllegalStateException",
                    "ReentrantLock Lock interface");
      return false;
   }

   n = 0;
   if (!rl_install(cond, n, "await", "()V", 'V', 0, 0, rl_cond_builtin))
      goto oom;
   ++n;
   if (!rl_install(cond, n, "await",
                   "(JLjava/util/concurrent/TimeUnit;)Z", 'Z', 2, 3,
                   rl_cond_builtin))
      goto oom;
   ++n;
   if (!rl_install(cond, n, "signal", "()V", 'V', 0, 0, rl_cond_builtin))
      goto oom;
   ++n;
   if (!rl_install(cond, n, "signalAll", "()V", 'V', 0, 0, rl_cond_builtin))
      goto oom;
   ++n;
   cond->nmethods = n;
   return true;

oom:
   lock->nmethods = n > 0 ? n : 0;
   cond->nmethods = 0;
   dvm__throw(v, "java/lang/OutOfMemoryError", "ReentrantLock method name");
   return false;
}

/* --- selftest ----------------------------------------------------------- */

static bool rl_test_call(struct dvm *v, const char *cls, const char *name,
                         const char *sig, dvm_ref self,
                         const union dvm_value *args, int nargs,
                         union dvm_value *out)
{
   struct dvm_method *m = dvm_lookup(v, cls, name, sig);
   union dvm_value sink = {0};
   if (!out) out = &sink;
   memset(out, 0, sizeof *out);
   return m && dvm_call(v, m, self, args, nargs, out);
}

static bool rl_test_threw(struct dvm *v, const char *slash_name)
{
   dvm_ref e = dvm_exception(v);
   bool match = false;
   if (e) {
      struct dvm_class *c = dvm_object_class(v, e);
      match = c && c->name && !strcmp(c->name, slash_name);
      dvm_clear_exception(v);
   }
   return match;
}

static bool rl_test_expect_throw(struct dvm *v, const char *cls,
                                 const char *name, const char *sig,
                                 dvm_ref self, const union dvm_value *args,
                                 int nargs, const char *want)
{
   union dvm_value out = {0};
   bool called = rl_test_call(v, cls, name, sig, self, args, nargs, &out);
   return !called && rl_test_threw(v, want);
}

static bool rl_test_geti(struct dvm *v, const char *cls, const char *name,
                         const char *sig, dvm_ref self, int32_t want)
{
   union dvm_value out = {0};
   if (!rl_test_call(v, cls, name, sig, self, NULL, 0, &out)) {
      if (dvm_exception(v)) dvm_clear_exception(v);
      return false;
   }
   return out.i == want;
}

static dvm_ref rl_test_new_lock(struct dvm *v, bool fair)
{
   struct dvm_class *cls = dvm__class_by_desc(v, RL_DESC);
   if (!cls) return 0;
   dvm_ref ref = dvm_new_object(v, cls);
   if (!ref) return 0;
   union dvm_value arg = {.i = fair ? 1 : 0};
   union dvm_value out = {0};
   bool ok = fair ? rl_test_call(v, RL_NAME, "<init>", "(Z)V", ref, &arg, 1,
                                 &out)
                  : rl_test_call(v, RL_NAME, "<init>", "()V", ref, NULL, 0,
                                 &out);
   if (!ok || dvm_exception(v)) {
      if (dvm_exception(v)) dvm_clear_exception(v);
      return 0;
   }
   return ref;
}

static bool rl_test_timeunit(struct dvm *v, union dvm_value *out)
{
   memset(out, 0, sizeof *out);
   if (!java_basics_static(v, "java/util/concurrent/TimeUnit", "MILLISECONDS",
                           "Ljava/util/concurrent/TimeUnit;", out))
      return false;
   return out->l != 0 && !dvm_exception(v);
}

struct rl_test_await_worker {
   struct dvm *v;
   dvm_ref lock;
   dvm_ref cond;
   _Atomic int locked;
   _Atomic int done;
   _Atomic int failed;
   _Atomic int await_result;
   _Atomic int held_after;
   _Atomic int hold_after;
};

static void *rl_test_await_main(void *arg)
{
   struct rl_test_await_worker *w = arg;
   struct dvm *v = w->v;
   union dvm_value out = {0};
   union dvm_value args[2];
   dvm__mark_bytecode_thread();
   dvm_gil_acquire(v);
   if (!rl_test_call(v, RL_NAME, "lock", "()V", w->lock, NULL, 0, &out) ||
       dvm_exception(v))
      goto fail;
   memset(&out, 0, sizeof out);
   if (!rl_test_call(v, RL_NAME, "lock", "()V", w->lock, NULL, 0, &out) ||
       dvm_exception(v))
      goto fail;
   atomic_store_explicit(&w->locked, 1, memory_order_release);
   if (!rl_test_timeunit(v, &args[1])) goto fail;
   args[0].j = 2000; /* bounded: an unsignaled await must not hang forever */
   memset(&out, 0, sizeof out);
   if (!rl_test_call(v, RL_COND_NAME, "await",
                     "(JLjava/util/concurrent/TimeUnit;)Z", w->cond, args, 2,
                     &out) ||
       dvm_exception(v))
      goto fail;
   atomic_store_explicit(&w->await_result, out.i, memory_order_release);
   int32_t held = 0, hold = -1;
   {
      union dvm_value q = {0};
      if (!rl_test_call(v, RL_NAME, "isHeldByCurrentThread", "()Z", w->lock,
                        NULL, 0, &q) ||
          dvm_exception(v))
         goto fail;
      held = q.i;
      memset(&q, 0, sizeof q);
      if (!rl_test_call(v, RL_NAME, "getHoldCount", "()I", w->lock, NULL, 0,
                        &q) ||
          dvm_exception(v))
         goto fail;
      hold = q.i;
   }
   atomic_store_explicit(&w->held_after, held, memory_order_release);
   atomic_store_explicit(&w->hold_after, hold, memory_order_release);
   memset(&out, 0, sizeof out);
   if (!rl_test_call(v, RL_NAME, "unlock", "()V", w->lock, NULL, 0, &out) ||
       dvm_exception(v))
      goto fail;
   memset(&out, 0, sizeof out);
   if (!rl_test_call(v, RL_NAME, "unlock", "()V", w->lock, NULL, 0, &out) ||
       dvm_exception(v))
      goto fail;
   atomic_store_explicit(&w->done, 1, memory_order_release);
   dvm_gil_release(v);
   return NULL;

fail:
   if (dvm_exception(v)) dvm_clear_exception(v);
   atomic_store_explicit(&w->failed, 1, memory_order_release);
   /* Best-effort release so a worker failure cannot strand the main thread
    * inside lock().  Errors are discarded because this path already reports
    * failure; an extra unlock of an unheld lock simply throws IMSE here. */
   for (int i = 0; i < 2; ++i) {
      union dvm_value sink = {0};
      (void)rl_test_call(v, RL_NAME, "unlock", "()V", w->lock, NULL, 0,
                         &sink);
      if (dvm_exception(v)) dvm_clear_exception(v);
   }
   atomic_store_explicit(&w->done, 1, memory_order_release);
   dvm_gil_release(v);
   return NULL;
}

struct rl_test_hold_worker {
   struct dvm *v;
   dvm_ref lock;
   _Atomic int locked;
   _Atomic int done;
   _Atomic int failed;
};

static void *rl_test_hold_main(void *arg)
{
   struct rl_test_hold_worker *w = arg;
   struct dvm *v = w->v;
   union dvm_value out = {0};
   dvm__mark_bytecode_thread();
   dvm_gil_acquire(v);
   if (!rl_test_call(v, RL_NAME, "lock", "()V", w->lock, NULL, 0, &out) ||
       dvm_exception(v))
      goto fail;
   atomic_store_explicit(&w->locked, 1, memory_order_release);
   {
      unsigned depth = dvm_gil_unlock_all(v);
      rl_sleep_ms(200);
      dvm_gil_relock(v, depth);
   }
   memset(&out, 0, sizeof out);
   if (!rl_test_call(v, RL_NAME, "unlock", "()V", w->lock, NULL, 0, &out) ||
       dvm_exception(v))
      goto fail;
   atomic_store_explicit(&w->done, 1, memory_order_release);
   dvm_gil_release(v);
   return NULL;

fail:
   if (dvm_exception(v)) dvm_clear_exception(v);
   atomic_store_explicit(&w->failed, 1, memory_order_release);
   /* Best-effort release so a worker failure cannot strand the main thread
    * inside lock().  Errors are discarded because this path already reports
    * failure; an extra unlock of an unheld lock simply throws IMSE here. */
   for (int i = 0; i < 2; ++i) {
      union dvm_value sink = {0};
      (void)rl_test_call(v, RL_NAME, "unlock", "()V", w->lock, NULL, 0,
                         &sink);
      if (dvm_exception(v)) dvm_clear_exception(v);
   }
   atomic_store_explicit(&w->done, 1, memory_order_release);
   dvm_gil_release(v);
   return NULL;
}

static bool rl_test_wait_flag(struct dvm *v, _Atomic int *flag,
                              unsigned limit_ms)
{
   unsigned waited = 0;
   while (!atomic_load_explicit(flag, memory_order_acquire) &&
          waited < limit_ms) {
      unsigned depth = dvm_gil_unlock_all(v);
      rl_sleep_ms(5);
      dvm_gil_relock(v, depth);
      waited += 5;
   }
   return atomic_load_explicit(flag, memory_order_acquire) != 0;
}

static void rl_test_join(struct dvm *v, pthread_t thread)
{
   unsigned depth = dvm_gil_unlock_all(v);
   pthread_join(thread, NULL);
   dvm_gil_relock(v, depth);
}

bool reentrant_lock_selftest(struct dvm *v)
{
   bool ok = true;
   union dvm_value out = {0};
   union dvm_value args[2];
   dvm_ref lock = 0, fair_lock = 0, cond = 0, raw = 0;
#define RL_CHECK(expr)                                                       \
   do {                                                                      \
      if (!(expr)) {                                                         \
         ok = false;                                                         \
         sdk_trace_fprintf(stderr,                                           \
                           "REENTRANT_LOCK_SELFTEST_FAIL line=%d "           \
                           "expression=%s\n",                                \
                           __LINE__, #expr);                                 \
      }                                                                      \
   } while (0)

   dvm_gil_acquire(v);

   struct dvm_class *cls = dvm__class_by_desc(v, RL_DESC);
   struct dvm_class *lock_iface = dvm__class_by_desc(v, RL_LOCK_DESC);
   RL_CHECK(cls != NULL && !cls->external);
   RL_CHECK(lock_iface != NULL);
   /* checkcast and instanceof both reduce to dvm__class_assignable(); the
    * ReentrantLock class must be assignable to the real Lock interface. */
   if (cls && lock_iface)
      RL_CHECK(dvm__class_assignable(v, cls, lock_iface));
   RL_CHECK(dvm_lookup(v, RL_NAME, "<init>", "()V") != NULL);
   RL_CHECK(dvm_lookup(v, RL_NAME, "<init>", "(Z)V") != NULL);
   RL_CHECK(dvm_lookup(v, RL_NAME, "lock", "()V") != NULL);
   RL_CHECK(dvm_lookup(v, RL_NAME, "unlock", "()V") != NULL);
   RL_CHECK(dvm_lookup(v, RL_NAME, "tryLock", "()Z") != NULL);
   RL_CHECK(dvm_lookup(v, RL_NAME, "tryLock",
                       "(JLjava/util/concurrent/TimeUnit;)Z") != NULL);
   RL_CHECK(dvm_lookup(v, RL_NAME, "newCondition", RL_COND_SIG) != NULL);
   RL_CHECK(dvm_lookup(v, RL_COND_NAME, "await",
                       "(JLjava/util/concurrent/TimeUnit;)Z") != NULL);

   lock = rl_test_new_lock(v, false);
   RL_CHECK(lock != 0);
   if (lock) {
      struct dvm_class *actual = dvm_object_class(v, lock);
      RL_CHECK(actual != NULL);
      if (actual && lock_iface)
         RL_CHECK(dvm__class_assignable(v, actual, lock_iface));
      RL_CHECK(rl_test_geti(v, RL_NAME, "isLocked", "()Z", lock, 0));
      RL_CHECK(rl_test_geti(v, RL_NAME, "isHeldByCurrentThread", "()Z", lock,
                            0));
      RL_CHECK(rl_test_geti(v, RL_NAME, "getHoldCount", "()I", lock, 0));
      RL_CHECK(rl_test_geti(v, RL_NAME, "isFair", "()Z", lock, 0));

      /* Recursive acquisition and the hold count. */
      memset(&out, 0, sizeof out);
      RL_CHECK(rl_test_call(v, RL_NAME, "lock", "()V", lock, NULL, 0, &out) &&
               !dvm_exception(v));
      memset(&out, 0, sizeof out);
      RL_CHECK(rl_test_call(v, RL_NAME, "lock", "()V", lock, NULL, 0, &out) &&
               !dvm_exception(v));
      RL_CHECK(rl_test_geti(v, RL_NAME, "isLocked", "()Z", lock, 1));
      RL_CHECK(rl_test_geti(v, RL_NAME, "isHeldByCurrentThread", "()Z", lock,
                            1));
      RL_CHECK(rl_test_geti(v, RL_NAME, "getHoldCount", "()I", lock, 2));
      memset(&out, 0, sizeof out);
      RL_CHECK(rl_test_call(v, RL_NAME, "unlock", "()V", lock, NULL, 0, &out) &&
               !dvm_exception(v));
      RL_CHECK(rl_test_geti(v, RL_NAME, "getHoldCount", "()I", lock, 1));
      memset(&out, 0, sizeof out);
      RL_CHECK(rl_test_call(v, RL_NAME, "unlock", "()V", lock, NULL, 0, &out) &&
               !dvm_exception(v));
      RL_CHECK(rl_test_geti(v, RL_NAME, "isLocked", "()Z", lock, 0));
      RL_CHECK(rl_test_geti(v, RL_NAME, "getHoldCount", "()I", lock, 0));

      /* Unlock without ownership is an IllegalMonitorStateException. */
      RL_CHECK(rl_test_expect_throw(
         v, RL_NAME, "unlock", "()V", lock, NULL, 0,
         "java/lang/IllegalMonitorStateException"));

      /* tryLock and the timed form on a free lock. */
      memset(&out, 0, sizeof out);
      RL_CHECK(rl_test_call(v, RL_NAME, "tryLock", "()Z", lock, NULL, 0,
                            &out) &&
               !dvm_exception(v) && out.i == 1);
      RL_CHECK(rl_test_geti(v, RL_NAME, "getHoldCount", "()I", lock, 1));
      memset(&out, 0, sizeof out);
      RL_CHECK(rl_test_call(v, RL_NAME, "unlock", "()V", lock, NULL, 0, &out) &&
               !dvm_exception(v));

      RL_CHECK(rl_test_timeunit(v, &args[1]));
      args[0].j = 50;
      memset(&out, 0, sizeof out);
      RL_CHECK(rl_test_call(v, RL_NAME, "tryLock",
                            "(JLjava/util/concurrent/TimeUnit;)Z", lock, args,
                            2, &out) &&
               !dvm_exception(v) && out.i == 1);
      memset(&out, 0, sizeof out);
      RL_CHECK(rl_test_call(v, RL_NAME, "unlock", "()V", lock, NULL, 0, &out) &&
               !dvm_exception(v));

      /* lockInterruptibly acquires like lock() when the lock is free. */
      memset(&out, 0, sizeof out);
      RL_CHECK(rl_test_call(v, RL_NAME, "lockInterruptibly", "()V", lock, NULL,
                            0, &out) &&
               !dvm_exception(v));
      RL_CHECK(rl_test_geti(v, RL_NAME, "getHoldCount", "()I", lock, 1));
      memset(&out, 0, sizeof out);
      RL_CHECK(rl_test_call(v, RL_NAME, "unlock", "()V", lock, NULL, 0, &out) &&
               !dvm_exception(v));

      /* A real Condition bound to the lock. */
      memset(&out, 0, sizeof out);
      RL_CHECK(rl_test_call(v, RL_NAME, "newCondition", RL_COND_SIG, lock,
                            NULL, 0, &out) &&
               !dvm_exception(v) && out.l != 0);
      cond = out.l;
      RL_CHECK(cond != 0);
      if (cond) {
         struct dvm_object *co = dvm__obj(v, cond);
         RL_CHECK(co && co->cls && co->cls->name &&
                  !strcmp(co->cls->name, RL_COND_NAME));

         /* await/signal require the lock. */
         RL_CHECK(rl_test_expect_throw(
            v, RL_COND_NAME, "await", "()V", cond, NULL, 0,
            "java/lang/IllegalMonitorStateException"));
         RL_CHECK(rl_test_expect_throw(
            v, RL_COND_NAME, "signal", "()V", cond, NULL, 0,
            "java/lang/IllegalMonitorStateException"));
         RL_CHECK(rl_test_expect_throw(
            v, RL_COND_NAME, "signalAll", "()V", cond, NULL, 0,
            "java/lang/IllegalMonitorStateException"));

         /* Same-thread signal with no waiter, then a real timed await with
          * holdCount restored across the wait. */
         memset(&out, 0, sizeof out);
         RL_CHECK(rl_test_call(v, RL_NAME, "lock", "()V", lock, NULL, 0,
                               &out) &&
                  !dvm_exception(v));
         memset(&out, 0, sizeof out);
         RL_CHECK(rl_test_call(v, RL_NAME, "lock", "()V", lock, NULL, 0,
                               &out) &&
                  !dvm_exception(v));
         memset(&out, 0, sizeof out);
         RL_CHECK(rl_test_call(v, RL_COND_NAME, "signal", "()V", cond, NULL, 0,
                               &out) &&
                  !dvm_exception(v));
         memset(&out, 0, sizeof out);
         RL_CHECK(rl_test_call(v, RL_COND_NAME, "signalAll", "()V", cond, NULL,
                               0, &out) &&
                  !dvm_exception(v));
         RL_CHECK(rl_test_timeunit(v, &args[1]));
         args[0].j = 20;
         memset(&out, 0, sizeof out);
         RL_CHECK(rl_test_call(v, RL_COND_NAME, "await",
                               "(JLjava/util/concurrent/TimeUnit;)Z", cond,
                               args, 2, &out) &&
                  !dvm_exception(v) && out.i == 0);
         RL_CHECK(rl_test_geti(v, RL_NAME, "isHeldByCurrentThread", "()Z",
                               lock, 1));
         RL_CHECK(rl_test_geti(v, RL_NAME, "getHoldCount", "()I", lock, 2));
         memset(&out, 0, sizeof out);
         RL_CHECK(rl_test_call(v, RL_NAME, "unlock", "()V", lock, NULL, 0,
                               &out) &&
                  !dvm_exception(v));
         memset(&out, 0, sizeof out);
         RL_CHECK(rl_test_call(v, RL_NAME, "unlock", "()V", lock, NULL, 0,
                               &out) &&
                  !dvm_exception(v));

         /* A zero timeout must not wait, but still restores ownership. */
         memset(&out, 0, sizeof out);
         RL_CHECK(rl_test_call(v, RL_NAME, "lock", "()V", lock, NULL, 0,
                               &out) &&
                  !dvm_exception(v));
         args[0].j = 0;
         memset(&out, 0, sizeof out);
         RL_CHECK(rl_test_call(v, RL_COND_NAME, "await",
                               "(JLjava/util/concurrent/TimeUnit;)Z", cond,
                               args, 2, &out) &&
                  !dvm_exception(v) && out.i == 0);
         RL_CHECK(rl_test_geti(v, RL_NAME, "getHoldCount", "()I", lock, 1));
         memset(&out, 0, sizeof out);
         RL_CHECK(rl_test_call(v, RL_NAME, "unlock", "()V", lock, NULL, 0,
                               &out) &&
                  !dvm_exception(v));

         /* Cross-thread await: the worker holds the lock twice, await fully
          * releases it, the main thread acquires and signals, and the worker
          * observes the restored hold count afterwards. */
         {
            struct rl_test_await_worker *aw = calloc(1, sizeof *aw);
            RL_CHECK(aw != NULL);
            if (aw) {
               pthread_t thread;
               aw->v = v;
               aw->lock = lock;
               aw->cond = cond;
               if (pthread_create(&thread, NULL, rl_test_await_main,
                                  aw) != 0) {
                  ok = false;
               } else {
                  RL_CHECK(rl_test_wait_flag(v, &aw->locked, 2000));
                  memset(&out, 0, sizeof out);
                  RL_CHECK(rl_test_call(v, RL_NAME, "lock", "()V", lock, NULL,
                                        0, &out) &&
                           !dvm_exception(v));
                  memset(&out, 0, sizeof out);
                  RL_CHECK(rl_test_call(v, RL_COND_NAME, "signal", "()V", cond,
                                        NULL, 0, &out) &&
                           !dvm_exception(v));
                  memset(&out, 0, sizeof out);
                  RL_CHECK(rl_test_call(v, RL_NAME, "unlock", "()V", lock,
                                       NULL, 0, &out) &&
                           !dvm_exception(v));
                  RL_CHECK(rl_test_wait_flag(v, &aw->done, 4000));
                  rl_test_join(v, thread);
                  RL_CHECK(atomic_load_explicit(&aw->failed,
                                                memory_order_acquire) == 0);
                  RL_CHECK(atomic_load_explicit(&aw->await_result,
                                                memory_order_acquire) == 1);
                  RL_CHECK(atomic_load_explicit(&aw->held_after,
                                                memory_order_acquire) == 1);
                  RL_CHECK(atomic_load_explicit(&aw->hold_after,
                                                memory_order_acquire) == 2);
               }
               free(aw);
            }
         }
      }
   }

   /* Fairness flag is remembered but does not claim fair scheduling. */
   fair_lock = rl_test_new_lock(v, true);
   RL_CHECK(fair_lock != 0);
   if (fair_lock)
      RL_CHECK(rl_test_geti(v, RL_NAME, "isFair", "()Z", fair_lock, 1));

   /* Fail-closed receiver paths. */
   if (cls) {
      raw = dvm_new_object(v, cls);
      RL_CHECK(raw != 0);
      if (raw) {
         RL_CHECK(rl_test_expect_throw(
            v, RL_NAME, "lock", "()V", raw, NULL, 0,
            "java/lang/IllegalStateException"));
         memset(&out, 0, sizeof out);
         RL_CHECK(rl_test_call(v, RL_NAME, "<init>", "()V", raw, NULL, 0,
                               &out) &&
                  !dvm_exception(v));
         RL_CHECK(rl_test_expect_throw(
            v, RL_NAME, "<init>", "()V", raw, NULL, 0,
            "java/lang/IllegalStateException"));
      }
   }
   if (lock) {
      memset(&out, 0, sizeof out);
      RL_CHECK(!reentrant_lock_call(v, RL_NAME, "noSuchMethod", "()V", lock,
                                    NULL, 0, &out));
      RL_CHECK(!dvm_exception(v));
      memset(&out, 0, sizeof out);
      RL_CHECK(!reentrant_lock_call(v, RL_NAME, "lock", "()I", lock, NULL, 0,
                                    &out));
      RL_CHECK(!dvm_exception(v));
   }
   {
      struct dvm_class *oc = dvm__class_by_desc(v, "Ljava/lang/Object;");
      dvm_ref foreign = oc ? dvm_new_object(v, oc) : 0;
      RL_CHECK(foreign != 0);
      if (foreign) {
         memset(&out, 0, sizeof out);
         RL_CHECK(!reentrant_lock_call(v, RL_LOCK_NAME, "lock", "()V", foreign,
                                       NULL, 0, &out));
         RL_CHECK(!dvm_exception(v));
      }
   }

   /* Lock.lock()/tryLock()/unlock()/newCondition() declared on the
    * interface are still dispatched by this module when the receiver really
    * is a ReentrantLock. */
   if (lock) {
      memset(&out, 0, sizeof out);
      RL_CHECK(reentrant_lock_call(v, RL_LOCK_NAME, "lock", "()V", lock, NULL,
                                   0, &out) &&
               !dvm_exception(v));
      RL_CHECK(rl_test_geti(v, RL_NAME, "getHoldCount", "()I", lock, 1));
      memset(&out, 0, sizeof out);
      RL_CHECK(reentrant_lock_call(v, RL_LOCK_NAME, "tryLock", "()Z", lock,
                                   NULL, 0, &out) &&
               !dvm_exception(v) && out.i == 1);
      RL_CHECK(rl_test_geti(v, RL_NAME, "getHoldCount", "()I", lock, 2));
      memset(&out, 0, sizeof out);
      RL_CHECK(reentrant_lock_call(v, RL_LOCK_NAME, "unlock", "()V", lock,
                                   NULL, 0, &out) &&
               !dvm_exception(v));
      memset(&out, 0, sizeof out);
      RL_CHECK(reentrant_lock_call(v, RL_LOCK_NAME, "unlock", "()V", lock,
                                   NULL, 0, &out) &&
               !dvm_exception(v));
      RL_CHECK(rl_test_geti(v, RL_NAME, "getHoldCount", "()I", lock, 0));
      memset(&out, 0, sizeof out);
      RL_CHECK(reentrant_lock_call(v, RL_LOCK_NAME, "newCondition", RL_COND_SIG,
                                   lock, NULL, 0, &out) &&
               !dvm_exception(v) && out.l != 0);
   }

   /* The Lock interface edge must not make this module claim
    * ReentrantReadWriteLock's Lock receivers.  A genuine ReadLock is created
    * and passed to reentrant_lock_call() with the Lock declaration: the
    * exact-class receiver test must decline it, and the real dispatch chain
    * must still acquire and release it through thread_runtime_call(). */
   {
      static const char rw_name[] =
         "java/util/concurrent/locks/ReentrantReadWriteLock";
      static const char rw_read_name[] =
         "java/util/concurrent/locks/ReentrantReadWriteLock$ReadLock";
      static const char rw_read_sig[] =
         "()Ljava/util/concurrent/locks/ReentrantReadWriteLock$ReadLock;";
      struct dvm_class *rw_cls = dvm__class_by_desc(
         v, "Ljava/util/concurrent/locks/ReentrantReadWriteLock;");
      RL_CHECK(rw_cls != NULL);
      if (rw_cls) {
         dvm_ref rw = dvm_new_object(v, rw_cls);
         RL_CHECK(rw != 0);
         if (rw) {
            memset(&out, 0, sizeof out);
            RL_CHECK(rl_test_call(v, rw_name, "<init>", "()V", rw, NULL, 0,
                                  &out) &&
                     !dvm_exception(v));
            memset(&out, 0, sizeof out);
            RL_CHECK(rl_test_call(v, rw_name, "readLock", rw_read_sig, rw,
                                  NULL, 0, &out) &&
                     !dvm_exception(v) && out.l != 0);
            if (out.l) {
               dvm_ref read_lock = out.l;
               memset(&out, 0, sizeof out);
               RL_CHECK(!reentrant_lock_call(v, RL_LOCK_NAME, "lock", "()V",
                                             read_lock, NULL, 0, &out));
               RL_CHECK(!dvm_exception(v));
               memset(&out, 0, sizeof out);
               RL_CHECK(rl_test_call(v, rw_read_name, "lock", "()V", read_lock,
                                     NULL, 0, &out) &&
                        !dvm_exception(v));
               memset(&out, 0, sizeof out);
               RL_CHECK(rl_test_call(v, rw_read_name, "unlock", "()V",
                                     read_lock, NULL, 0, &out) &&
                        !dvm_exception(v));
            }
         }
      }
   }

   /* Cross-thread timed tryLock against a thread that really holds the lock. */
   if (lock) {
      struct rl_test_hold_worker *hw = calloc(1, sizeof *hw);
      RL_CHECK(hw != NULL);
      if (hw) {
         pthread_t thread;
         hw->v = v;
         hw->lock = lock;
         if (pthread_create(&thread, NULL, rl_test_hold_main, hw) != 0) {
            ok = false;
         } else {
            RL_CHECK(rl_test_wait_flag(v, &hw->locked, 2000));
            memset(&out, 0, sizeof out);
            RL_CHECK(rl_test_call(v, RL_NAME, "tryLock", "()Z", lock, NULL, 0,
                                  &out) &&
                     !dvm_exception(v) && out.i == 0);
            if (rl_test_timeunit(v, &args[1])) {
               args[0].j = -1;
               memset(&out, 0, sizeof out);
               RL_CHECK(rl_test_call(v, RL_NAME, "tryLock",
                                     "(JLjava/util/concurrent/TimeUnit;)Z",
                                     lock, args, 2, &out) &&
                        !dvm_exception(v) && out.i == 0);
               args[0].j = 30;
               memset(&out, 0, sizeof out);
               RL_CHECK(rl_test_call(v, RL_NAME, "tryLock",
                                     "(JLjava/util/concurrent/TimeUnit;)Z",
                                     lock, args, 2, &out) &&
                        !dvm_exception(v) && out.i == 0);
            } else {
               ok = false;
            }
            RL_CHECK(rl_test_wait_flag(v, &hw->done, 4000));
            rl_test_join(v, thread);
            RL_CHECK(atomic_load_explicit(&hw->failed, memory_order_acquire) ==
                     0);
         }
         free(hw);
      }
   }

#undef RL_CHECK
   if (dvm_exception(v)) {
      dvm_clear_exception(v);
      ok = false;
   }
   dvm_gil_release(v);
   sdk_trace_fprintf(stderr, "REENTRANT_LOCK_SELFTEST %s\n",
                     ok ? "PASS" : "FAIL");
   return ok;
}
