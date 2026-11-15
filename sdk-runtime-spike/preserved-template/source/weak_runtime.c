/* WeakReference/Reference subset for the host-published
 * CommonUtils.currentActivity field.
 *
 * The DVM has no collector: a dvm_ref stored here is retained strongly for
 * the life of the VM.  This module therefore does not implement weak
 * reachability, reference processing or ReferenceQueue.enqueue; no referent
 * is reclaimed or cleared early.  get()/clear() are real state changes on
 * this object, and isEnqueued()/enqueue() report false instead of
 * pretending that a queue operation happened.
 *
 * The class hierarchy must exist before bytecode resolves
 * Reference.get()/clear() through a WeakReference receiver.  The DEX owner of
 * these calls is not uniform: CommonUtils.getCurrentActivity compiles to
 * Ljava/lang/ref/WeakReference;->get, while other code uses
 * Ljava/lang/ref/Reference;->get.  Both owners are handled below, but only
 * for a receiver carrying this module's WeakReference payload.
 */
#include "weak_runtime.h"
#include "dvm/dvm_internal.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int sdk_trace_fprintf(FILE *stream, const char *format, ...);

/* activity_runtime.c declares this in activity_runtime.h.  The declaration is
 * repeated here so this file also compiles against a stale candidate header
 * that predates the accessor. */
extern dvm_ref activity_runtime_current(struct dvm *v);

#define WEAK_MAGIC 0x574b5246u /* "WKRF" */

struct weak_state {
 struct dvm *vm;
 dvm_ref referent;
 dvm_ref queue;
 uint32_t magic;
};

/* One host-published wrapper is enough for dvm.c's null-static-field hook.
 * The key includes the VM and the current referent; weak_runtime_vm_start
 * clears it.  A single entry is safe when multiple VMs exist: a nonmatching
 * VM is never reused, it just causes a rebuild. */
static struct {
 struct dvm *vm;
 dvm_ref referent;
 dvm_ref weak;
} cache;

static void weak_destroy(void *payload) {
 free(payload);
}

static struct weak_state *weak_data(struct dvm *v, struct dvm_object *o) {
 if (!o || o->data_destroy != weak_destroy || !o->data) return NULL;
 struct weak_state *s = (struct weak_state *)o->data;
 if (s->magic != WEAK_MAGIC || s->vm != v) return NULL;
 return s;
}

static bool derives(struct dvm_class *c, const char *name) {
 for (; c; c = c->super)
  if (!strcmp(c->name, name)) return true;
 return false;
}

static bool is_weak_receiver(struct dvm *v, dvm_ref self) {
 struct dvm_object *o = dvm__obj(v, self);
 return o && derives(o->cls, "java/lang/ref/WeakReference");
}

#define IS(n, s) (!strcmp(m, n) && !strcmp(sig, s))

bool weak_runtime_call(struct dvm *v, const char *c, const char *m, const char *sig,
                       dvm_ref self, const union dvm_value *a, int n,
                       union dvm_value *out) {
 if (!v || !c || !m || !sig) return false;
 struct dvm_object *o = dvm__obj(v, self);
 if (!o) return false;

 bool owner = !strcmp(c, "java/lang/ref/WeakReference") ||
              !strcmp(c, "java/lang/ref/Reference");
 if (!owner) return false;

 /* Only WeakReference constructors are implemented.  A two-argument
  * constructor stores the queue reference without implementing enqueueing. */
 if (!strcmp(c, "java/lang/ref/WeakReference") && !strcmp(m, "<init>")) {
  if (IS("<init>", "(Ljava/lang/Object;)V") && n == 1) {
   if (!is_weak_receiver(v, self) || !a) return false;
   if (weak_data(v, o)) {
    dvm__throw(v, "java/lang/IllegalStateException",
               "WeakReference already initialized");
    return true;
   }
   if (o->data) return false; /* another runtime owns this receiver */
   struct weak_state *s = malloc(sizeof *s);
   if (!s) {
    dvm__throw(v, "java/lang/OutOfMemoryError", "WeakReference state");
    return true;
   }
   s->vm = v;
   s->referent = a[0].l;
   s->queue = 0;
   s->magic = WEAK_MAGIC;
   o->data = s;
   o->data_destroy = weak_destroy;
   if (out) out->l = 0;
   return true;
  }
  if (IS("<init>", "(Ljava/lang/Object;Ljava/lang/ref/ReferenceQueue;)V") && n == 2) {
   if (!is_weak_receiver(v, self) || !a) return false;
   if (weak_data(v, o)) {
    dvm__throw(v, "java/lang/IllegalStateException",
               "WeakReference already initialized");
    return true;
   }
   if (o->data) return false;
   struct weak_state *s = malloc(sizeof *s);
   if (!s) {
    dvm__throw(v, "java/lang/OutOfMemoryError", "WeakReference state");
    return true;
   }
   s->vm = v;
   s->referent = a[0].l;
   s->queue = a[1].l;
   s->magic = WEAK_MAGIC;
   o->data = s;
   o->data_destroy = weak_destroy;
   if (out) out->l = 0;
   return true;
  }
  return false;
 }

 /* Reference.get() and Reference.clear() are the declaration-owner forms
  * javac may emit for a WeakReference receiver.  Never treat an arbitrary
  * Reference subclass as implemented: the receiver must belong to this VM and
  * carry this module's payload. */
 struct weak_state *s = weak_data(v, o);
 if (!s || !is_weak_receiver(v, self)) return false;

 if (IS("get", "()Ljava/lang/Object;") && n == 0) {
  if (!out) return false;
  out->l = s->referent;
  return true;
 }
 if (IS("clear", "()V") && n == 0) {
  s->referent = 0;
  if (out) out->l = 0;
  return true;
 }
 /* No collector means no reference processing.  These answers are truthful:
  * this object has not been enqueued and no enqueue succeeds. */
 if (IS("isEnqueued", "()Z") && n == 0) {
  if (!out) return false;
  out->i = 0;
  return true;
 }
 if (IS("enqueue", "()Z") && n == 0) {
  if (!out) return false;
  out->i = 0;
  return true;
 }
 return false;
}

dvm_ref weak_runtime_make(struct dvm *v, dvm_ref referent) {
 if (!v) return 0;
 struct dvm_class *cls = dvm_find_class_or_external(v, "java/lang/ref/WeakReference");
 if (!cls) return 0;
 dvm_ref weak = dvm_new_object(v, cls);
 if (!weak) return 0;
 union dvm_value arg = { .l = referent };
 union dvm_value ignored = { 0 };
 if (!weak_runtime_call(v, "java/lang/ref/WeakReference", "<init>",
                        "(Ljava/lang/Object;)V", weak, &arg, 1, &ignored) ||
     dvm_exception(v)) {
  dvm__discard_temporary(v, weak);
  return 0;
 }
 return weak;
}

bool weak_runtime_static(struct dvm *v, const char *c, const char *f,
                         const char *t, union dvm_value *o) {
 if (!v || !c || !f || !t || !o) return false;
 if (strcmp(c, "com/gsc/base/utils/CommonUtils") ||
     strcmp(f, "currentActivity") ||
     strcmp(t, "Ljava/lang/ref/WeakReference;"))
  return false;

 /* The app's setCurrentActivity(Activity) writes this field through its own
  * sput-object; dvm.c only calls this hook while the static slot is null.
  * Returning true with null here is the real Java field value, not a
  * fabricated WeakReference. */
 dvm_ref activity = activity_runtime_current(v);
 if (!activity) {
  o->l = 0;
  return true;
 }

 if (cache.vm == v && cache.referent == activity && cache.weak) {
  struct dvm_object *cached = dvm__obj(v, cache.weak);
  if (cached && weak_data(v, cached)) {
   o->l = cache.weak;
   return true;
  }
  cache.vm = 0;
  cache.referent = 0;
  cache.weak = 0;
 }

 dvm_ref weak = weak_runtime_make(v, activity);
 if (!weak) return false; /* fail closed; the static field stays null */
 cache.vm = v;
 cache.referent = activity;
 cache.weak = weak;
 sdk_trace_fprintf(stderr, "WEAK_CURRENT_ACTIVITY published=1 reference=0x%x\n",
                   (unsigned)weak);
 o->l = weak;
 return true;
}

void weak_runtime_vm_start(struct dvm *v) {
 if (!v) return;
 memset(&cache, 0, sizeof cache);
 static const char *pairs[][2] = {
  { "java/lang/ref/Reference", "java/lang/Object" },
  { "java/lang/ref/WeakReference", "java/lang/ref/Reference" }
 };
 for (size_t i = 0; i < sizeof pairs / sizeof pairs[0]; ++i) {
  struct dvm_class *c = dvm_find_class_or_external(v, pairs[i][0]);
  struct dvm_class *parent = dvm_find_class_or_external(v, pairs[i][1]);
  /* Match throwable_reference_vm_start: only fill a placeholder that has no
   * DEX definition of its own. */
  if (c && parent && c->external) c->super = parent;
 }
}
