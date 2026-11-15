#include "priority_queue.h"
#include "java_basics.h"
#include "dvm/dex.h"
#include <limits.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

extern int sdk_trace_fprintf(FILE *, const char *, ...);

#define PQ_MAGIC 0x50514331u      /* "PQC1" */
#define PQ_ITR_MAGIC 0x50514931u  /* "PQI1" */
#define PQ_DEFAULT_CAP 11u
#define PQ_MAX_ELEMENTS 0x3FFFFFFFu
#define PQ_MAX_DEPTH 64u

enum { PQ_CMP_OK = 0, PQ_CMP_THREW = 1, PQ_CMP_UNHANDLED = 2 };

typedef struct pq {
    uint32_t magic;
    struct dvm *vm;
    dvm_ref *items;
    uint32_t size;
    uint32_t cap;
    dvm_ref comparator; /* 0 = natural ordering */
    uint32_t mod;
} pq;

typedef struct pq_itr {
    uint32_t magic;
    struct dvm *vm;
    dvm_ref owner;
    uint32_t index;
    dvm_ref last;
    uint32_t expected_mod;
} pq_itr;

static void pq_release(void *data);
static void pq_itr_release(void *data);
static bool priority_queue_call_inner(struct dvm *vm, const char *cls,
                                      const char *method, const char *sig,
                                      dvm_ref self,
                                      const union dvm_value *args, int nargs,
                                      union dvm_value *out);

/* ------------------------------------------------------------------ */
/* payload ownership                                                   */
/* ------------------------------------------------------------------ */

static void pq_release(void *data)
{
    pq *q = (pq *)data;
    if (!q) return;
    if (q->items) {
        for (uint32_t i = 0; i < q->size; ++i)
            if (q->items[i]) dvm_unpin(q->vm, q->items[i]);
        free(q->items);
    }
    if (q->comparator) dvm_unpin(q->vm, q->comparator);
    q->magic = 0;
    free(q);
}

static void pq_itr_release(void *data)
{
    pq_itr *it = (pq_itr *)data;
    if (!it) return;
    if (it->owner) dvm_unpin(it->vm, it->owner);
    it->magic = 0;
    free(it);
}

static pq *pq_payload(struct dvm *vm, struct dvm_object *o)
{
    if (!o || o->data_destroy != pq_release || !o->data) return NULL;
    pq *q = (pq *)o->data;
    if (q->magic != PQ_MAGIC) {
        dvm__throw(vm, "java/lang/IllegalStateException", "PriorityQueue payload is corrupt");
        return NULL;
    }
    if (q->vm != vm) {
        dvm__throw(vm, "java/lang/IllegalStateException", "PriorityQueue belongs to another VM");
        return NULL;
    }
    return q;
}

static pq_itr *pq_itr_payload(struct dvm *vm, struct dvm_object *o)
{
    if (!o || o->data_destroy != pq_itr_release || !o->data) return NULL;
    pq_itr *it = (pq_itr *)o->data;
    if (it->magic != PQ_ITR_MAGIC) {
        dvm__throw(vm, "java/lang/IllegalStateException", "PriorityQueue iterator is corrupt");
        return NULL;
    }
    if (it->vm != vm) {
        dvm__throw(vm, "java/lang/IllegalStateException", "PriorityQueue iterator belongs to another VM");
        return NULL;
    }
    return it;
}

/* ------------------------------------------------------------------ */
/* class identity                                                      */
/* ------------------------------------------------------------------ */

static bool pq_is_queue_class(struct dvm *vm, struct dvm_class *cls)
{
    (void)vm;
    for (uint32_t depth = 0; cls && depth < PQ_MAX_DEPTH; ++depth, cls = cls->super)
        if (cls->name && !strcmp(cls->name, "java/util/PriorityQueue")) return true;
    return false;
}

static bool pq_is_itr_class(struct dvm *vm, struct dvm_class *cls)
{
    (void)vm;
    for (uint32_t depth = 0; cls && depth < PQ_MAX_DEPTH; ++depth, cls = cls->super)
        if (cls->name && !strcmp(cls->name, "java/util/PriorityQueue$Itr")) return true;
    return false;
}

static bool pq_is_enum_class(struct dvm *vm, struct dvm_class *cls)
{
    (void)vm;
    for (uint32_t depth = 0; cls && depth < PQ_MAX_DEPTH; ++depth, cls = cls->super)
        if (cls->name && !strcmp(cls->name, "java/lang/Enum")) return true;
    return false;
}

static bool pq_declares_comparable(struct dvm *vm, struct dvm_class *cls)
{
    struct dvm_class *comparable = dvm_find_class_or_external(vm, "java/lang/Comparable");
    return comparable && dvm__class_assignable(vm, cls, comparable);
}

static bool pq_related_name(const char *c)
{
    return !strcmp(c, "java/util/PriorityQueue") ||
           !strcmp(c, "java/util/PriorityQueue$Itr") ||
           !strcmp(c, "java/util/AbstractQueue") ||
           !strcmp(c, "java/util/Queue") ||
           !strcmp(c, "java/util/Collection") ||
           !strcmp(c, "java/util/AbstractCollection") ||
           !strcmp(c, "java/lang/Iterable");
}

/* ------------------------------------------------------------------ */
/* comparison through real DVM calls                                   */
/* ------------------------------------------------------------------ */

static int pq_call_host(struct dvm *vm, const char *cls, const char *method,
                        const char *sig, dvm_ref self,
                        const union dvm_value *args, int nargs,
                        union dvm_value *out)
{
    memset(out, 0, sizeof *out);
    if (!vm->hooks.call_external) return PQ_CMP_UNHANDLED;
    bool handled = vm->hooks.call_external(vm->hooks.user, vm, cls, method, sig,
                                           self, args, nargs, out);
    if (dvm_exception(vm)) return PQ_CMP_THREW;
    return handled ? PQ_CMP_OK : PQ_CMP_UNHANDLED;
}

static int pq_invoke_named(struct dvm *vm, struct dvm_class *cls,
                           const char *method, const char *sig, dvm_ref self,
                           const union dvm_value *args, int nargs,
                           union dvm_value *out)
{
    struct dvm_method *m = cls ? dvm_find_method(vm, cls, method, sig) : NULL;
    if (m && (m->has_code || m->builtin)) {
        memset(out, 0, sizeof *out);
        if (dvm_call(vm, m, self, args, nargs, out)) return PQ_CMP_OK;
        if (dvm_exception(vm)) return PQ_CMP_THREW;
        dvm__throw(vm, "java/lang/IllegalStateException",
                   "PriorityQueue comparison method %s failed without exception", method);
        return PQ_CMP_THREW;
    }
    if (dvm_exception(vm)) return PQ_CMP_THREW;
    return pq_call_host(vm, cls ? cls->name : "", method, sig, self, args, nargs, out);
}

static int pq_sign(int32_t value)
{
    return (value > 0) - (value < 0);
}

static int pq_compare_with_comparator(struct dvm *vm, dvm_ref comparator,
                                      dvm_ref a, dvm_ref b, int *cmp)
{
    struct dvm_object *co = dvm__obj(vm, comparator);
    if (!co || !co->cls) {
        dvm__throw(vm, "java/lang/IllegalStateException", "PriorityQueue comparator is not a live object");
        return PQ_CMP_THREW;
    }
    union dvm_value args[2] = {{.l = a}, {.l = b}};
    union dvm_value out = {0};
    int rc = pq_invoke_named(vm, co->cls, "compare",
                             "(Ljava/lang/Object;Ljava/lang/Object;)I",
                             comparator, args, 2, &out);
    if (rc == PQ_CMP_OK) {
        *cmp = pq_sign(out.i);
        return PQ_CMP_OK;
    }
    if (rc == PQ_CMP_THREW) return PQ_CMP_THREW;
    dvm__throw(vm, "java/lang/NoSuchMethodError",
               "Comparator.compare(Object,Object) not implemented for %s", co->cls->name);
    return PQ_CMP_THREW;
}

static int pq_compare_natural(struct dvm *vm, dvm_ref a, dvm_ref b, int *cmp)
{
    struct dvm_object *ao = dvm__obj(vm, a);
    if (!ao || !ao->cls) {
        dvm__throw(vm, "java/lang/IllegalStateException", "PriorityQueue element is not a live object");
        return PQ_CMP_THREW;
    }
    /* A real bridge method (compareTo(Object)I) is the primary natural-order
     * path for application classes and for any dex-defined Comparable. */
    struct dvm_method *bridge = dvm_find_method(vm, ao->cls, "compareTo",
                                                "(Ljava/lang/Object;)I");
    if (bridge && (bridge->has_code || bridge->builtin)) {
        union dvm_value arg = {.l = b};
        union dvm_value out = {0};
        if (!dvm_call(vm, bridge, a, &arg, 1, &out)) {
            if (!dvm_exception(vm))
                dvm__throw(vm, "java/lang/IllegalStateException",
                           "compareTo(Object) failed without exception");
            return PQ_CMP_THREW;
        }
        *cmp = pq_sign(out.i);
        return PQ_CMP_OK;
    }

    const char *name = ao->cls->name ? ao->cls->name : "";
    const char *typed = NULL;
    if (!strcmp(name, "java/lang/String")) typed = "(Ljava/lang/String;)I";
    else if (!strcmp(name, "java/lang/Byte")) typed = "(Ljava/lang/Byte;)I";
    else if (!strcmp(name, "java/lang/Short")) typed = "(Ljava/lang/Short;)I";
    else if (!strcmp(name, "java/lang/Integer")) typed = "(Ljava/lang/Integer;)I";
    else if (!strcmp(name, "java/lang/Long")) typed = "(Ljava/lang/Long;)I";
    else if (!strcmp(name, "java/lang/Character")) typed = "(Ljava/lang/Character;)I";
    else if (!strcmp(name, "java/lang/Float")) typed = "(Ljava/lang/Float;)I";
    else if (!strcmp(name, "java/lang/Double")) typed = "(Ljava/lang/Double;)I";
    else if (!strcmp(name, "java/lang/Boolean")) typed = "(Ljava/lang/Boolean;)I";
    else if (!strcmp(name, "java/util/concurrent/TimeUnit"))
        typed = "(Ljava/util/concurrent/TimeUnit;)I";

    if (typed) {
        union dvm_value arg = {.l = b};
        union dvm_value out = {0};
        int rc = pq_call_host(vm, name, "compareTo", typed, a, &arg, 1, &out);
        if (rc == PQ_CMP_OK) {
            *cmp = pq_sign(out.i);
            return PQ_CMP_OK;
        }
        if (rc == PQ_CMP_THREW) return PQ_CMP_THREW;
        dvm__throw(vm, "java/lang/NoSuchMethodError",
                   "%s.compareTo is not implemented by the runtime", name);
        return PQ_CMP_THREW;
    }

    if (pq_is_enum_class(vm, ao->cls)) {
        union dvm_value arg = {.l = b};
        union dvm_value out = {0};
        int rc = pq_call_host(vm, "java/lang/Enum", "compareTo",
                              "(Ljava/lang/Enum;)I", a, &arg, 1, &out);
        if (rc == PQ_CMP_OK) {
            *cmp = pq_sign(out.i);
            return PQ_CMP_OK;
        }
        if (rc == PQ_CMP_THREW) return PQ_CMP_THREW;
        dvm__throw(vm, "java/lang/NoSuchMethodError",
                   "java.lang.Enum.compareTo is not implemented by the runtime");
        return PQ_CMP_THREW;
    }

    if (pq_declares_comparable(vm, ao->cls)) {
        dvm__throw(vm, "java/lang/NoSuchMethodError",
                   "Comparable.compareTo(Object) is not implemented for %s", name);
        return PQ_CMP_THREW;
    }

    dvm__throw(vm, "java/lang/ClassCastException",
               "%s cannot be cast to java.lang.Comparable", name);
    return PQ_CMP_THREW;
}

static int pq_compare(struct dvm *vm, pq *q, dvm_ref a, dvm_ref b, int *cmp)
{
    if (q->comparator)
        return pq_compare_with_comparator(vm, q->comparator, a, b, cmp);
    return pq_compare_natural(vm, a, b, cmp);
}

/* ------------------------------------------------------------------ */
/* array storage and heap maintenance                                  */
/* ------------------------------------------------------------------ */

static bool pq_reserve(struct dvm *vm, pq *q, uint32_t need)
{
    if (need <= q->cap) return true;
    uint32_t cap = q->cap ? q->cap : 1;
    while (cap < need) {
        if (cap > PQ_MAX_ELEMENTS / 2) { cap = PQ_MAX_ELEMENTS; break; }
        cap *= 2;
    }
    if (cap < need) {
        dvm__throw(vm, "java/lang/OutOfMemoryError", "PriorityQueue element capacity");
        return false;
    }
    dvm_ref *items = (dvm_ref *)realloc(q->items, (size_t)cap * sizeof *items);
    if (!items) {
        dvm__throw(vm, "java/lang/OutOfMemoryError", "PriorityQueue element array");
        return false;
    }
    q->items = items;
    q->cap = cap;
    return true;
}

/* Two-phase sift planning: every guest-visible comparison happens before the
 * array is touched.  A comparison that throws, or guest code that mutates the
 * queue during comparison (detected through mod), leaves the queue unchanged. */
static bool pq_plan_up(struct dvm *vm, pq *q, uint32_t k, dvm_ref x,
                       uint32_t *path, uint32_t *npath, uint32_t expected)
{
    uint32_t n = 0;
    while (k > 0) {
        uint32_t parent = (k - 1) >> 1;
        int cmp = 0;
        if (pq_compare(vm, q, x, q->items[parent], &cmp) != PQ_CMP_OK) return false;
        if (q->mod != expected) {
            dvm__throw(vm, "java/util/ConcurrentModificationException",
                       "PriorityQueue modified during comparison");
            return false;
        }
        if (cmp >= 0) break;
        if (n >= PQ_MAX_DEPTH) {
            dvm__throw(vm, "java/lang/IllegalStateException", "PriorityQueue heap depth");
            return false;
        }
        path[n++] = parent;
        k = parent;
    }
    *npath = n;
    return true;
}

static bool pq_plan_down(struct dvm *vm, pq *q, uint32_t k, dvm_ref x, uint32_t n,
                         uint32_t *path, uint32_t *npath, uint32_t expected)
{
    uint32_t count = 0;
    uint32_t half = n >> 1;
    while (k < half) {
        uint32_t child = (k << 1) + 1;
        dvm_ref c = q->items[child];
        uint32_t right = child + 1;
        if (right < n) {
            int cmp = 0;
            if (pq_compare(vm, q, c, q->items[right], &cmp) != PQ_CMP_OK) return false;
            if (q->mod != expected) {
                dvm__throw(vm, "java/util/ConcurrentModificationException",
                           "PriorityQueue modified during comparison");
                return false;
            }
            if (cmp > 0) {
                child = right;
                c = q->items[right];
            }
        }
        int cmp2 = 0;
        if (pq_compare(vm, q, x, c, &cmp2) != PQ_CMP_OK) return false;
        if (q->mod != expected) {
            dvm__throw(vm, "java/util/ConcurrentModificationException",
                       "PriorityQueue modified during comparison");
            return false;
        }
        if (cmp2 <= 0) break;
        if (count >= PQ_MAX_DEPTH) {
            dvm__throw(vm, "java/lang/IllegalStateException", "PriorityQueue heap depth");
            return false;
        }
        path[count++] = child;
        k = child;
    }
    *npath = count;
    return true;
}

static void pq_commit_up_at(pq *q, uint32_t k, dvm_ref x,
                            const uint32_t *path, uint32_t n)
{
    uint32_t pos = k;
    for (uint32_t i = 0; i < n; ++i) {
        q->items[pos] = q->items[path[i]];
        pos = path[i];
    }
    q->items[pos] = x;
}

static void pq_commit_down_at(pq *q, uint32_t k, dvm_ref x,
                              const uint32_t *path, uint32_t n)
{
    uint32_t pos = k;
    for (uint32_t i = 0; i < n; ++i) {
        q->items[pos] = q->items[path[i]];
        pos = path[i];
    }
    q->items[pos] = x;
}

static bool pq_offer_value(struct dvm *vm, pq *q, dvm_ref x)
{
    if (!x || !dvm__obj(vm, x)) {
        dvm__throw(vm, "java/lang/NullPointerException", "PriorityQueue does not permit null elements");
        return false;
    }
    if (q->size >= PQ_MAX_ELEMENTS) {
        dvm__throw(vm, "java/lang/OutOfMemoryError", "PriorityQueue element limit");
        return false;
    }
    if (!pq_reserve(vm, q, q->size + 1)) return false;
    uint32_t expected = q->mod;
    uint32_t path[PQ_MAX_DEPTH];
    uint32_t npath = 0;
    dvm_pin(vm, x);
    if (!pq_plan_up(vm, q, q->size, x, path, &npath, expected)) {
        dvm_unpin(vm, x);
        return false;
    }
    pq_commit_up_at(q, q->size, x, path, npath);
    q->size++;
    q->mod++;
    return true;
}

/* Remove position `at`.  The returned element is still pinned by the caller's
 * pin accounting; the caller must dvm_unpin it after a successful removal. */
static bool pq_remove_at(struct dvm *vm, pq *q, uint32_t at, dvm_ref *removed)
{
    if (at >= q->size) {
        dvm__throw(vm, "java/lang/IllegalStateException", "PriorityQueue remove index");
        return false;
    }
    dvm_ref result = q->items[at];
    uint32_t n = q->size - 1;
    if (at == n) {
        q->items[n] = 0;
        q->size = n;
        q->mod++;
        *removed = result;
        return true;
    }

    uint32_t expected = q->mod;
    dvm_ref x = q->items[n];
    uint32_t down[PQ_MAX_DEPTH];
    uint32_t ndown = 0;
    if (!pq_plan_down(vm, q, at, x, n, down, &ndown, expected)) return false;

    uint32_t up[PQ_MAX_DEPTH];
    uint32_t nup = 0;
    if (ndown == 0) {
        if (!pq_plan_up(vm, q, at, x, up, &nup, expected)) return false;
    }

    if (ndown) pq_commit_down_at(q, at, x, down, ndown);
    else if (nup) pq_commit_up_at(q, at, x, up, nup);
    else q->items[at] = x;

    q->items[n] = 0;
    q->size = n;
    q->mod++;
    *removed = result;
    return true;
}

static bool pq_poll_value(struct dvm *vm, pq *q, dvm_ref *out_ref)
{
    if (q->size == 0) {
        *out_ref = 0;
        return true;
    }
    dvm_ref removed = 0;
    if (!pq_remove_at(vm, q, 0, &removed)) return false;
    dvm_unpin(vm, removed);
    *out_ref = removed;
    return true;
}

static bool pq_heapify(struct dvm *vm, pq *q)
{
    if (q->size < 2) return true;
    uint32_t expected = q->mod;
    for (uint32_t i = (q->size >> 1); i-- > 0;) {
        dvm_ref x = q->items[i];
        uint32_t path[PQ_MAX_DEPTH];
        uint32_t npath = 0;
        if (!pq_plan_down(vm, q, i, x, q->size, path, &npath, expected)) return false;
        pq_commit_down_at(q, i, x, path, npath);
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* construction and collection copying                                 */
/* ------------------------------------------------------------------ */

static pq *pq_alloc(struct dvm *vm, struct dvm_object *o, uint32_t cap, dvm_ref comparator)
{
    if (cap < 1) cap = 1;
    if (cap > PQ_MAX_ELEMENTS) {
        dvm__throw(vm, "java/lang/OutOfMemoryError", "PriorityQueue initial capacity");
        return NULL;
    }
    if (comparator && !dvm__obj(vm, comparator)) {
        dvm__throw(vm, "java/lang/NullPointerException", "PriorityQueue comparator is not a live object");
        return NULL;
    }
    pq *q = (pq *)calloc(1, sizeof *q);
    if (!q) {
        dvm__throw(vm, "java/lang/OutOfMemoryError", "PriorityQueue state");
        return NULL;
    }
    q->items = (dvm_ref *)calloc(cap, sizeof *q->items);
    if (!q->items) {
        free(q);
        dvm__throw(vm, "java/lang/OutOfMemoryError", "PriorityQueue element array");
        return NULL;
    }
    q->magic = PQ_MAGIC;
    q->vm = vm;
    q->cap = cap;
    q->comparator = comparator;
    if (comparator) dvm_pin(vm, comparator);
    o->data = q;
    o->data_destroy = pq_release;
    return q;
}

static bool pq_source_call(struct dvm *vm, dvm_ref src, const char *method,
                           const char *sig, const union dvm_value *args,
                           int nargs, union dvm_value *out)
{
    struct dvm_object *o = src ? dvm__obj(vm, src) : NULL;
    if (!o || !o->cls) return false;
    memset(out, 0, sizeof *out);
    if (o->data_destroy == pq_release || o->data_destroy == pq_itr_release)
        return priority_queue_call_inner(vm, o->cls->name, method, sig, src, args, nargs, out);
    struct dvm_method *m = dvm_find_method(vm, o->cls, method, sig);
    if (m && (m->has_code || m->builtin)) {
        if (dvm_call(vm, m, src, args, nargs, out)) return true;
        return false; /* exception, if any, is preserved */
    }
    if (dvm_exception(vm)) return false;
    if (vm->hooks.call_external &&
        vm->hooks.call_external(vm->hooks.user, vm, o->cls->name, method, sig,
                                src, args, nargs, out))
        return !dvm_exception(vm);
    return false;
}

static bool pq_append_raw(struct dvm *vm, pq *q, dvm_ref x)
{
    if (q->size >= PQ_MAX_ELEMENTS) {
        dvm__throw(vm, "java/lang/OutOfMemoryError", "PriorityQueue element limit");
        return false;
    }
    if (!pq_reserve(vm, q, q->size + 1)) return false;
    q->items[q->size++] = x;
    dvm_pin(vm, x);
    return true;
}

static bool pq_copy_collection(struct dvm *vm, pq *q, dvm_ref src)
{
    union dvm_value iterator = {0};
    if (!pq_source_call(vm, src, "iterator", "()Ljava/util/Iterator;", NULL, 0, &iterator)) {
        if (!dvm_exception(vm))
            dvm__throw(vm, "java/lang/UnsupportedOperationException", "Collection.iterator");
        return false;
    }
    if (!iterator.l) {
        dvm__throw(vm, "java/lang/NullPointerException", "Collection.iterator returned null");
        return false;
    }
    for (;;) {
        union dvm_value more = {0};
        if (!pq_source_call(vm, iterator.l, "hasNext", "()Z", NULL, 0, &more)) {
            if (!dvm_exception(vm))
                dvm__throw(vm, "java/lang/UnsupportedOperationException", "Iterator.hasNext");
            return false;
        }
        if (!more.i) break;
        union dvm_value item = {0};
        if (!pq_source_call(vm, iterator.l, "next", "()Ljava/lang/Object;", NULL, 0, &item)) {
            if (!dvm_exception(vm))
                dvm__throw(vm, "java/lang/UnsupportedOperationException", "Iterator.next");
            return false;
        }
        if (!item.l) {
            dvm__throw(vm, "java/lang/NullPointerException", "Collection contains null");
            return false;
        }
        if (!pq_append_raw(vm, q, item.l)) return false;
    }
    return pq_heapify(vm, q);
}

static bool pq_construct(struct dvm *vm, struct dvm_object *o,
                         const char *sig, const union dvm_value *args, int nargs,
                         union dvm_value *out)
{
    (void)out;
    if (o->data || o->data_destroy) {
        dvm__throw(vm, "java/lang/IllegalStateException", "PriorityQueue already initialized");
        return true;
    }

    uint32_t cap = PQ_DEFAULT_CAP;
    dvm_ref comparator = 0;

    if (!strcmp(sig, "()V")) {
        if (nargs != 0) return false;
    } else if (!strcmp(sig, "(I)V")) {
        if (nargs != 1) return false;
        if (args[0].i < 1) {
            dvm__throw(vm, "java/lang/IllegalArgumentException",
                       "PriorityQueue initial capacity must be positive");
            return true;
        }
        if ((uint32_t)args[0].i > PQ_MAX_ELEMENTS) {
            dvm__throw(vm, "java/lang/OutOfMemoryError",
                       "PriorityQueue initial capacity is too large");
            return true;
        }
        cap = (uint32_t)args[0].i;
    } else if (!strcmp(sig, "(Ljava/util/Comparator;)V")) {
        if (nargs != 1) return false;
        comparator = args[0].l;
    } else if (!strcmp(sig, "(Ljava/util/Collection;)V") ||
               !strcmp(sig, "(Ljava/util/PriorityQueue;)V")) {
        if (nargs != 1) return false;
        if (!args[0].l) {
            dvm__throw(vm, "java/lang/NullPointerException", "PriorityQueue source");
            return true;
        }
        struct dvm_object *src = dvm__obj(vm, args[0].l);
        if (!src || !src->cls) {
            dvm__throw(vm, "java/lang/NullPointerException", "PriorityQueue source");
            return true;
        }
        if (src->data_destroy == pq_release && src->data) {
            pq *source = (pq *)src->data;
            if (source->magic != PQ_MAGIC || source->vm != vm) {
                dvm__throw(vm, "java/lang/IllegalStateException",
                           "PriorityQueue source belongs to another VM");
                return true;
            }
            comparator = source->comparator;
            cap = source->size > PQ_DEFAULT_CAP ? source->size : PQ_DEFAULT_CAP;
            pq *q = pq_alloc(vm, o, cap, comparator);
            if (!q) return true;
            if (source->size) {
                memcpy(q->items, source->items, (size_t)source->size * sizeof *q->items);
                q->size = source->size;
                for (uint32_t i = 0; i < q->size; ++i) dvm_pin(vm, q->items[i]);
            }
            return true;
        }
        /* JDK semantics: a SortedSet source supplies its comparator. */
        struct dvm_class *sorted_set = dvm_find_class_or_external(vm, "java/util/SortedSet");
        if (sorted_set && dvm__class_assignable(vm, src->cls, sorted_set)) {
            union dvm_value got = {0};
            if (!pq_source_call(vm, args[0].l, "comparator", "()Ljava/util/Comparator;",
                                NULL, 0, &got)) {
                if (!dvm_exception(vm))
                    dvm__throw(vm, "java/lang/UnsupportedOperationException",
                               "SortedSet.comparator");
                return true;
            }
            comparator = got.l;
        }
        pq *q = pq_alloc(vm, o, PQ_DEFAULT_CAP, comparator);
        if (!q) return true;
        pq_copy_collection(vm, q, args[0].l);
        return true;
    } else {
        return false;
    }

    return pq_alloc(vm, o, cap, comparator) != NULL;
}

/* ------------------------------------------------------------------ */
/* instance methods                                                    */
/* ------------------------------------------------------------------ */

static bool pq_contains_value(struct dvm *vm, pq *q, dvm_ref key, bool *found)
{
    *found = false;
    if (!key) return true;
    for (uint32_t i = 0; i < q->size; ++i) {
        bool equal = java_basics_equal(vm, key, q->items[i]);
        if (dvm_exception(vm)) return false;
        if (equal) {
            *found = true;
            break;
        }
    }
    return true;
}

static bool pq_remove_value(struct dvm *vm, pq *q, dvm_ref key, bool *found)
{
    *found = false;
    if (!key) return true;
    for (uint32_t i = 0; i < q->size; ++i) {
        bool equal = java_basics_equal(vm, key, q->items[i]);
        if (dvm_exception(vm)) return false;
        if (equal) {
            dvm_ref removed = 0;
            if (!pq_remove_at(vm, q, i, &removed)) return false;
            dvm_unpin(vm, removed);
            *found = true;
            return true;
        }
    }
    return true;
}

static bool pq_to_array(struct dvm *vm, pq *q, dvm_ref array_ref, union dvm_value *out)
{
    if (!array_ref) {
        dvm__throw(vm, "java/lang/NullPointerException", "PriorityQueue.toArray array");
        return false;
    }
    struct dvm_object *array = dvm__obj(vm, array_ref);
    if (!array || array->kind != DVM_OBJ_ARRAY ||
        (array->elem_kind != 'L' && array->elem_kind != '[')) {
        dvm__throw(vm, "java/lang/ArrayStoreException", "PriorityQueue.toArray requires an object array");
        return false;
    }
    uint32_t length = array->length;
    if (length >= q->size) {
        dvm_ref *dst = (dvm_ref *)dvm_array_data(vm, array_ref);
        if (!dst && q->size) {
            dvm__throw(vm, "java/lang/IllegalStateException", "PriorityQueue.toArray array data");
            return false;
        }
        if (q->size) memcpy(dst, q->items, (size_t)q->size * sizeof *dst);
        if (length > q->size) dst[q->size] = 0;
        out->l = array_ref;
        return true;
    }
    const char *elem_desc = array->cls && array->cls->elem ? array->cls->elem->desc
                                                          : "Ljava/lang/Object;";
    dvm_ref created = dvm_new_array(vm, array->elem_kind, elem_desc, q->size);
    if (!created) {
        dvm__throw(vm, "java/lang/OutOfMemoryError", "PriorityQueue.toArray result");
        return false;
    }
    dvm_ref *dst = (dvm_ref *)dvm_array_data(vm, created);
    if (!dst && q->size) {
        dvm__throw(vm, "java/lang/IllegalStateException", "PriorityQueue.toArray result data");
        return false;
    }
    if (q->size) memcpy(dst, q->items, (size_t)q->size * sizeof *dst);
    out->l = created;
    return true;
}

static bool pq_handle_queue(struct dvm *vm, dvm_ref owner, pq *q, const char *method,
                            const char *sig, const union dvm_value *args,
                            int nargs, union dvm_value *out)
{
    if (!strcmp(method, "offer") && !strcmp(sig, "(Ljava/lang/Object;)Z") && nargs == 1) {
        if (!pq_offer_value(vm, q, args[0].l)) return true;
        out->i = 1;
        return true;
    }
    if (!strcmp(method, "add") && !strcmp(sig, "(Ljava/lang/Object;)Z") && nargs == 1) {
        if (!pq_offer_value(vm, q, args[0].l)) return true;
        out->i = 1;
        return true;
    }
    if (!strcmp(method, "peek") && !strcmp(sig, "()Ljava/lang/Object;")) {
        out->l = q->size ? q->items[0] : 0;
        return true;
    }
    if (!strcmp(method, "element") && !strcmp(sig, "()Ljava/lang/Object;")) {
        if (!q->size) {
            dvm__throw(vm, "java/util/NoSuchElementException", "PriorityQueue is empty");
            return true;
        }
        out->l = q->items[0];
        return true;
    }
    if (!strcmp(method, "poll") && !strcmp(sig, "()Ljava/lang/Object;")) {
        dvm_ref value = 0;
        if (!pq_poll_value(vm, q, &value)) return true;
        out->l = value;
        return true;
    }
    if (!strcmp(method, "remove") && !strcmp(sig, "()Ljava/lang/Object;")) {
        if (!q->size) {
            dvm__throw(vm, "java/util/NoSuchElementException", "PriorityQueue is empty");
            return true;
        }
        dvm_ref value = 0;
        if (!pq_poll_value(vm, q, &value)) return true;
        out->l = value;
        return true;
    }
    if (!strcmp(method, "remove") && !strcmp(sig, "(Ljava/lang/Object;)Z") && nargs == 1) {
        bool found = false;
        if (!pq_remove_value(vm, q, args[0].l, &found)) return true;
        out->i = found;
        return true;
    }
    if (!strcmp(method, "contains") && !strcmp(sig, "(Ljava/lang/Object;)Z") && nargs == 1) {
        bool found = false;
        if (!pq_contains_value(vm, q, args[0].l, &found)) return true;
        out->i = found;
        return true;
    }
    if (!strcmp(method, "size") && !strcmp(sig, "()I")) {
        out->i = (int32_t)q->size;
        return true;
    }
    if (!strcmp(method, "isEmpty") && !strcmp(sig, "()Z")) {
        out->i = q->size == 0;
        return true;
    }
    if (!strcmp(method, "clear") && !strcmp(sig, "()V")) {
        for (uint32_t i = 0; i < q->size; ++i) dvm_unpin(vm, q->items[i]);
        if (q->items) memset(q->items, 0, (size_t)q->size * sizeof *q->items);
        q->size = 0;
        q->mod++;
        return true;
    }
    if (!strcmp(method, "comparator") && !strcmp(sig, "()Ljava/util/Comparator;")) {
        out->l = q->comparator;
        return true;
    }
    if (!strcmp(method, "iterator") && !strcmp(sig, "()Ljava/util/Iterator;")) {
        struct dvm_class *cls = dvm_find_class_or_external(vm, "java/util/PriorityQueue$Itr");
        if (!cls) {
            dvm__throw(vm, "java/lang/NoClassDefFoundError", "java/util/PriorityQueue$Itr");
            return true;
        }
        dvm_ref ref = dvm_new_object(vm, cls);
        if (!ref) {
            dvm__throw(vm, "java/lang/OutOfMemoryError", "PriorityQueue iterator");
            return true;
        }
        struct dvm_object *object = dvm__obj(vm, ref);
        pq_itr *it = (pq_itr *)calloc(1, sizeof *it);
        if (!object || !it) {
            free(it);
            dvm__discard_temporary(vm, ref);
            dvm__throw(vm, "java/lang/OutOfMemoryError", "PriorityQueue iterator state");
            return true;
        }
        it->magic = PQ_ITR_MAGIC;
        it->vm = vm;
        it->owner = owner;
        it->index = 0;
        it->last = 0;
        it->expected_mod = q->mod;
        dvm_pin(vm, owner);
        object->data = it;
        object->data_destroy = pq_itr_release;
        out->l = ref;
        return true;
    }
    if (!strcmp(method, "toArray") && !strcmp(sig, "()[Ljava/lang/Object;")) {
        dvm_ref array = dvm_new_array(vm, 'L', "Ljava/lang/Object;", q->size);
        if (!array) {
            dvm__throw(vm, "java/lang/OutOfMemoryError", "PriorityQueue.toArray result");
            return true;
        }
        dvm_ref *dst = (dvm_ref *)dvm_array_data(vm, array);
        if (!dst && q->size) {
            dvm__throw(vm, "java/lang/IllegalStateException", "PriorityQueue.toArray data");
            return true;
        }
        if (q->size) memcpy(dst, q->items, (size_t)q->size * sizeof *dst);
        out->l = array;
        return true;
    }
    if (!strcmp(method, "toArray") && !strcmp(sig, "([Ljava/lang/Object;)[Ljava/lang/Object;") &&
        nargs == 1) {
        if (!pq_to_array(vm, q, args[0].l, out)) return true;
        return true;
    }
    return false;
}

static bool pq_handle_iterator(struct dvm *vm, struct dvm_object *o, pq_itr *it,
                               const char *method, const char *sig,
                               union dvm_value *out)
{
    dvm_ref owner = it->owner;
    struct dvm_object *owner_object = owner ? dvm__obj(vm, owner) : NULL;
    if (!owner_object || owner_object->data_destroy != pq_release || !owner_object->data) {
        dvm__throw(vm, "java/lang/IllegalStateException",
                   "PriorityQueue iterator owner is not a live queue");
        return true;
    }
    pq *q = pq_payload(vm, owner_object);
    if (!q) return true;

    if (!strcmp(method, "hasNext") && !strcmp(sig, "()Z")) {
        out->i = it->index < q->size;
        return true;
    }
    if (!strcmp(method, "next") && !strcmp(sig, "()Ljava/lang/Object;")) {
        if (it->expected_mod != q->mod) {
            dvm__throw(vm, "java/util/ConcurrentModificationException",
                       "PriorityQueue modified during iteration");
            return true;
        }
        if (it->index >= q->size) {
            dvm__throw(vm, "java/util/NoSuchElementException", "PriorityQueue iterator exhausted");
            return true;
        }
        it->last = q->items[it->index++];
        out->l = it->last;
        return true;
    }
    if (!strcmp(method, "remove") && !strcmp(sig, "()V")) {
        if (!it->last) {
            dvm__throw(vm, "java/lang/IllegalStateException", "Iterator.remove without next");
            return true;
        }
        if (it->expected_mod != q->mod) {
            dvm__throw(vm, "java/util/ConcurrentModificationException",
                       "PriorityQueue modified during iteration");
            return true;
        }
        for (uint32_t i = 0; i < q->size; ++i) {
            if (q->items[i] == it->last) {
                dvm_ref removed = 0;
                if (!pq_remove_at(vm, q, i, &removed)) return true;
                dvm_unpin(vm, removed);
                it->last = 0;
                break;
            }
        }
        it->expected_mod = q->mod;
        return true;
    }
    (void)o;
    return false;
}

static bool pq_handle_instance(struct dvm *vm, dvm_ref self, struct dvm_object *o,
                               const char *method, const char *sig,
                               const union dvm_value *args, int nargs,
                               union dvm_value *out)
{
    if (o->data_destroy == pq_release) {
        pq *q = pq_payload(vm, o);
        if (!q) return true;
        return pq_handle_queue(vm, self, q, method, sig, args, nargs, out);
    }
    if (o->data_destroy == pq_itr_release) {
        pq_itr *it = pq_itr_payload(vm, o);
        if (!it) return true;
        return pq_handle_iterator(vm, o, it, method, sig, out);
    }
    if (pq_is_queue_class(vm, o->cls)) {
        dvm__throw(vm, "java/lang/IllegalStateException", "PriorityQueue is not initialized");
        return true;
    }
    if (pq_is_itr_class(vm, o->cls)) {
        dvm__throw(vm, "java/lang/IllegalStateException", "PriorityQueue iterator is not initialized");
        return true;
    }
    return false;
}

static bool priority_queue_call_inner(struct dvm *vm, const char *cls,
                                      const char *method, const char *sig,
                                      dvm_ref self,
                                      const union dvm_value *args, int nargs,
                                      union dvm_value *out)
{
    if (!vm || !cls || !method || !sig || !out) return false;
    memset(out, 0, sizeof *out);

    if (!strcmp(method, "<init>")) {
        /* Only the real PriorityQueue constructor is host-implemented.  A
         * subclass constructor has dex code and is executed by the VM; a
         * super() call naming AbstractQueue or Object must not be mistaken
         * for queue initialization. */
        if (strcmp(cls, "java/util/PriorityQueue")) return false;
        struct dvm_object *o = self ? dvm__obj(vm, self) : NULL;
        if (!o || !o->cls) {
            dvm__throw(vm, "java/lang/NullPointerException", "PriorityQueue constructor receiver");
            return true;
        }
        return pq_construct(vm, o, sig, args, nargs, out);
    }

    if (!self) {
        if (pq_related_name(cls)) {
            dvm__throw(vm, "java/lang/NullPointerException", "PriorityQueue receiver");
            return true;
        }
        return false;
    }

    struct dvm_object *o = dvm__obj(vm, self);
    if (!o || !o->cls) {
        if (pq_related_name(cls)) {
            dvm__throw(vm, "java/lang/NullPointerException", "PriorityQueue receiver");
            return true;
        }
        return false;
    }
    return pq_handle_instance(vm, self, o, method, sig, args, nargs, out);
}

bool priority_queue_call(struct dvm *vm, const char *cls, const char *method,
                         const char *sig, dvm_ref self,
                         const union dvm_value *args, int nargs,
                         union dvm_value *out)
{
    return priority_queue_call_inner(vm, cls, method, sig, self, args, nargs, out);
}

/* ------------------------------------------------------------------ */
/* class setup                                                         */
/* ------------------------------------------------------------------ */

void priority_queue_vm_start(struct dvm *vm)
{
    if (!vm) return;

    struct dvm_class *object = dvm_find_class_or_external(vm, "java/lang/Object");
    struct dvm_class *collection = dvm_find_class_or_external(vm, "java/util/Collection");
    struct dvm_class *queue = dvm_find_class_or_external(vm, "java/util/Queue");
    struct dvm_class *abstract_collection =
        dvm_find_class_or_external(vm, "java/util/AbstractCollection");
    struct dvm_class *abstract_queue =
        dvm_find_class_or_external(vm, "java/util/AbstractQueue");
    struct dvm_class *priority_queue =
        dvm_find_class_or_external(vm, "java/util/PriorityQueue");
    struct dvm_class *iterator = dvm_find_class_or_external(vm, "java/util/Iterator");
    struct dvm_class *iterator_impl =
        dvm_find_class_or_external(vm, "java/util/PriorityQueue$Itr");

    if (queue) {
        queue->is_interface = true;
        queue->access |= DEX_ACC_INTERFACE | DEX_ACC_ABSTRACT;
        if (!queue->nifaces && collection) {
            queue->ifaces = (struct dvm_class **)calloc(1, sizeof *queue->ifaces);
            if (queue->ifaces) {
                queue->ifaces[0] = collection;
                queue->nifaces = 1;
            }
        }
    }
    if (abstract_collection && object && !abstract_collection->super)
        abstract_collection->super = object;
    if (abstract_collection && collection && !abstract_collection->nifaces) {
        abstract_collection->ifaces =
            (struct dvm_class **)calloc(1, sizeof *abstract_collection->ifaces);
        if (abstract_collection->ifaces) {
            abstract_collection->ifaces[0] = collection;
            abstract_collection->nifaces = 1;
        }
    }
    if (abstract_queue) {
        if (abstract_collection) abstract_queue->super = abstract_collection;
        if (!abstract_queue->nifaces && queue) {
            abstract_queue->ifaces =
                (struct dvm_class **)calloc(1, sizeof *abstract_queue->ifaces);
            if (abstract_queue->ifaces) {
                abstract_queue->ifaces[0] = queue;
                abstract_queue->nifaces = 1;
            }
        }
    }
    if (priority_queue && abstract_queue) priority_queue->super = abstract_queue;
    if (iterator_impl) {
        if (object) iterator_impl->super = object;
        if (!iterator_impl->nifaces && iterator) {
            iterator_impl->ifaces =
                (struct dvm_class **)calloc(1, sizeof *iterator_impl->ifaces);
            if (iterator_impl->ifaces) {
                iterator_impl->ifaces[0] = iterator;
                iterator_impl->nifaces = 1;
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* selftest                                                            */
/* ------------------------------------------------------------------ */

struct pq_test_item { int32_t value; };

static void pq_test_item_destroy(void *data)
{
    free(data);
}

static struct pq_test_item *pq_test_item_of(struct dvm *vm, dvm_ref ref)
{
    struct dvm_object *o = ref ? dvm__obj(vm, ref) : NULL;
    return o && o->data_destroy == pq_test_item_destroy
               ? (struct pq_test_item *)o->data
               : NULL;
}

static dvm_ref pq_test_item_new(struct dvm *vm, int32_t value)
{
    struct dvm_class *cls = dvm_find_class_or_external(vm, "test/PriorityQueueItem");
    dvm_ref ref = dvm_new_object(vm, cls);
    if (!ref) return 0;
    struct pq_test_item *item = (struct pq_test_item *)calloc(1, sizeof *item);
    if (!item) {
        dvm__discard_temporary(vm, ref);
        return 0;
    }
    item->value = value;
    struct dvm_object *o = dvm__obj(vm, ref);
    o->data = item;
    o->data_destroy = pq_test_item_destroy;
    return ref;
}

static bool pq_test_item_compare(struct dvm *vm, dvm_ref self,
                                 const union dvm_value *args, int nargs,
                                 union dvm_value *out)
{
    (void)nargs;
    struct pq_test_item *left = pq_test_item_of(vm, self);
    struct pq_test_item *right = args && args[0].l ? pq_test_item_of(vm, args[0].l) : NULL;
    if (!left || !right) {
        dvm__throw(vm, "java/lang/ClassCastException", "PriorityQueue test item");
        return true;
    }
    out->i = (left->value > right->value) - (left->value < right->value);
    return true;
}

static bool pq_test_item_equals(struct dvm *vm, dvm_ref self,
                                const union dvm_value *args, int nargs,
                                union dvm_value *out)
{
    (void)vm;
    out->i = nargs >= 1 && args && args[0].l == self;
    return true;
}

static bool pq_test_desc_compare(struct dvm *vm, dvm_ref self,
                                 const union dvm_value *args, int nargs,
                                 union dvm_value *out)
{
    (void)self;
    if (nargs < 2 || !args) {
        dvm__throw(vm, "java/lang/ClassCastException", "PriorityQueue test comparator arguments");
        return true;
    }
    struct pq_test_item *left = args[0].l ? pq_test_item_of(vm, args[0].l) : NULL;
    struct pq_test_item *right = args[1].l ? pq_test_item_of(vm, args[1].l) : NULL;
    if (!left || !right) {
        dvm__throw(vm, "java/lang/ClassCastException", "PriorityQueue test comparator");
        return true;
    }
    out->i = (right->value > left->value) - (right->value < left->value);
    return true;
}

static bool pq_test_add_method(struct dvm *vm, const char *class_name, const char *name,
                               const char *sig, dvm_builtin_fn fn, char ret_kind,
                               int arg_count, int arg_slots)
{
    struct dvm_class *cls = dvm_find_class(vm, class_name);
    if (!cls) {
        char desc[512];
        snprintf(desc, sizeof desc, "L%s;", class_name);
        cls = dvm__register_builtin(vm, desc);
    }
    if (!cls) return false;
    struct dvm_method *methods = (struct dvm_method *)realloc(
        cls->methods, (size_t)(cls->nmethods + 1) * sizeof *methods);
    if (!methods) return false;
    cls->methods = methods;
    struct dvm_method *m = &cls->methods[cls->nmethods];
    memset(m, 0, sizeof *m);
    m->cls = cls;
    m->name = name;
    m->sig = strdup(sig);
    if (!m->sig) return false;
    m->arg_count = arg_count;
    m->arg_slots = arg_slots;
    m->ret_kind = ret_kind;
    m->builtin = fn;
    cls->nmethods++;
    return true;
}

static bool pq_test_setup_classes(struct dvm *vm)
{
    if (!pq_test_add_method(vm, "test/PriorityQueueItem", "compareTo",
                            "(Ljava/lang/Object;)I", pq_test_item_compare, 'I', 1, 1))
        return false;
    if (!pq_test_add_method(vm, "test/PriorityQueueItem", "equals",
                            "(Ljava/lang/Object;)Z", pq_test_item_equals, 'Z', 1, 1))
        return false;
    if (!pq_test_add_method(vm, "test/PriorityQueueDescending", "compare",
                            "(Ljava/lang/Object;Ljava/lang/Object;)I",
                            pq_test_desc_compare, 'I', 2, 2))
        return false;
    return true;
}

static dvm_ref pq_test_new_queue(struct dvm *vm)
{
    return dvm_new_object(vm, dvm_find_class_or_external(vm, "java/util/PriorityQueue"));
}

static bool pq_test_call(struct dvm *vm, const char *method, const char *sig,
                         dvm_ref self, const union dvm_value *args, int nargs,
                         union dvm_value *out)
{
    return priority_queue_call_inner(vm, "java/util/PriorityQueue", method, sig,
                                     self, args, nargs, out);
}

#define PQ_CHECK(expr)                                                        \
    do {                                                                      \
        ++checks;                                                             \
        bool pq_ok_ = (expr);                                                 \
        if (dvm_exception(vm)) pq_ok_ = false;                                \
        if (!pq_ok_) {                                                        \
            ok = false;                                                       \
            sdk_trace_fprintf(stderr, "PRIORITY_QUEUE_SELFTEST_FAIL line=%d\n", \
                              __LINE__);                                      \
            dvm_clear_exception(vm);                                          \
        }                                                                     \
    } while (0)

#define PQ_EXPECT(expr)                                                       \
    do {                                                                      \
        ++checks;                                                             \
        bool pq_handled_ = (expr);                                            \
        if (!pq_handled_ || !dvm_exception(vm)) {                             \
            ok = false;                                                       \
            sdk_trace_fprintf(stderr, "PRIORITY_QUEUE_SELFTEST_FAIL line=%d expected_exception=1\n", \
                              __LINE__);                                      \
        }                                                                     \
        dvm_clear_exception(vm);                                              \
    } while (0)

bool priority_queue_selftest(struct dvm *vm)
{
    if (!vm) return false;
    priority_queue_vm_start(vm);
    bool ok = true;
    unsigned checks = 0;
    union dvm_value out = {0};

    PQ_CHECK(pq_test_setup_classes(vm));

    /* ---- natural ordering: out-of-order offers, peek/poll sorted ---- */
    dvm_ref natural = pq_test_new_queue(vm);
    PQ_CHECK(natural != 0);
    PQ_CHECK(pq_test_call(vm, "<init>", "()V", natural, NULL, 0, &out));
    PQ_CHECK(pq_test_call(vm, "size", "()I", natural, NULL, 0, &out) && out.i == 0);
    PQ_CHECK(pq_test_call(vm, "isEmpty", "()Z", natural, NULL, 0, &out) && out.i == 1);
    PQ_CHECK(pq_test_call(vm, "peek", "()Ljava/lang/Object;", natural, NULL, 0, &out) && out.l == 0);
    PQ_CHECK(pq_test_call(vm, "poll", "()Ljava/lang/Object;", natural, NULL, 0, &out) && out.l == 0);

    dvm_ref i5 = pq_test_item_new(vm, 5);
    dvm_ref i1 = pq_test_item_new(vm, 1);
    dvm_ref i3 = pq_test_item_new(vm, 3);
    PQ_CHECK(i5 && i1 && i3);
    union dvm_value arg = {.l = i5};
    PQ_CHECK(pq_test_call(vm, "offer", "(Ljava/lang/Object;)Z", natural, &arg, 1, &out) && out.i == 1);
    arg.l = i1;
    PQ_CHECK(pq_test_call(vm, "add", "(Ljava/lang/Object;)Z", natural, &arg, 1, &out) && out.i == 1);
    arg.l = i3;
    PQ_CHECK(pq_test_call(vm, "add", "(Ljava/lang/Object;)Z", natural, &arg, 1, &out) && out.i == 1);
    PQ_CHECK(pq_test_call(vm, "size", "()I", natural, NULL, 0, &out) && out.i == 3);
    PQ_CHECK(pq_test_call(vm, "peek", "()Ljava/lang/Object;", natural, NULL, 0, &out) && out.l == i1);

    /* contains/remove use the test class's real equals(Object) builtin. */
    arg.l = i3;
    PQ_CHECK(pq_test_call(vm, "contains", "(Ljava/lang/Object;)Z", natural, &arg, 1, &out) && out.i == 1);
    PQ_CHECK(pq_test_call(vm, "remove", "(Ljava/lang/Object;)Z", natural, &arg, 1, &out) && out.i == 1);
    PQ_CHECK(pq_test_call(vm, "contains", "(Ljava/lang/Object;)Z", natural, &arg, 1, &out) && out.i == 0);
    PQ_CHECK(pq_test_call(vm, "size", "()I", natural, NULL, 0, &out) && out.i == 2);
    arg.l = i3;
    PQ_CHECK(pq_test_call(vm, "add", "(Ljava/lang/Object;)Z", natural, &arg, 1, &out) && out.i == 1);

    PQ_CHECK(pq_test_call(vm, "poll", "()Ljava/lang/Object;", natural, NULL, 0, &out) && out.l == i1);
    PQ_CHECK(pq_test_call(vm, "poll", "()Ljava/lang/Object;", natural, NULL, 0, &out) && out.l == i3);
    PQ_CHECK(pq_test_call(vm, "poll", "()Ljava/lang/Object;", natural, NULL, 0, &out) && out.l == i5);
    PQ_CHECK(pq_test_call(vm, "poll", "()Ljava/lang/Object;", natural, NULL, 0, &out) && out.l == 0);

    /* ---- null element: real NPE, queue unchanged ---- */
    dvm_ref null_queue = pq_test_new_queue(vm);
    PQ_CHECK(null_queue != 0);
    PQ_CHECK(pq_test_call(vm, "<init>", "()V", null_queue, NULL, 0, &out));
    union dvm_value null_arg = {.l = 0};
    PQ_EXPECT(pq_test_call(vm, "offer", "(Ljava/lang/Object;)Z", null_queue, &null_arg, 1, &out));
    PQ_CHECK(pq_test_call(vm, "size", "()I", null_queue, NULL, 0, &out) && out.i == 0);

    /* ---- non-comparable elements: real ClassCastException ---- */
    dvm_ref plain1 = dvm_new_object(vm, dvm_find_class_or_external(vm, "test/PriorityQueuePlain"));
    dvm_ref plain2 = dvm_new_object(vm, dvm_find_class_or_external(vm, "test/PriorityQueuePlain"));
    dvm_ref plain_queue = pq_test_new_queue(vm);
    PQ_CHECK(plain1 && plain2 && plain_queue);
    PQ_CHECK(pq_test_call(vm, "<init>", "()V", plain_queue, NULL, 0, &out));
    union dvm_value plain_arg = {.l = plain1};
    PQ_CHECK(pq_test_call(vm, "offer", "(Ljava/lang/Object;)Z", plain_queue, &plain_arg, 1, &out) && out.i == 1);
    plain_arg.l = plain2;
    PQ_EXPECT(pq_test_call(vm, "offer", "(Ljava/lang/Object;)Z", plain_queue, &plain_arg, 1, &out));
    PQ_CHECK(pq_test_call(vm, "size", "()I", plain_queue, NULL, 0, &out) && out.i == 1);

    /* ---- Comparator: descending order and comparator() identity ---- */
    dvm_ref descending = dvm_new_object(
        vm, dvm_find_class_or_external(vm, "test/PriorityQueueDescending"));
    dvm_ref compared = pq_test_new_queue(vm);
    PQ_CHECK(descending && compared);
    union dvm_value comp_arg = {.l = descending};
    PQ_CHECK(pq_test_call(vm, "<init>", "(Ljava/util/Comparator;)V", compared, &comp_arg, 1, &out));
    PQ_CHECK(pq_test_call(vm, "comparator", "()Ljava/util/Comparator;", compared, NULL, 0, &out) &&
             out.l == descending);
    dvm_ref c1 = pq_test_item_new(vm, 1);
    dvm_ref c3 = pq_test_item_new(vm, 3);
    dvm_ref c5 = pq_test_item_new(vm, 5);
    PQ_CHECK(c1 && c3 && c5);
    arg.l = c1;
    PQ_CHECK(pq_test_call(vm, "offer", "(Ljava/lang/Object;)Z", compared, &arg, 1, &out) && out.i == 1);
    arg.l = c5;
    PQ_CHECK(pq_test_call(vm, "offer", "(Ljava/lang/Object;)Z", compared, &arg, 1, &out) && out.i == 1);
    arg.l = c3;
    PQ_CHECK(pq_test_call(vm, "offer", "(Ljava/lang/Object;)Z", compared, &arg, 1, &out) && out.i == 1);
    PQ_CHECK(pq_test_call(vm, "poll", "()Ljava/lang/Object;", compared, NULL, 0, &out) && out.l == c5);
    PQ_CHECK(pq_test_call(vm, "poll", "()Ljava/lang/Object;", compared, NULL, 0, &out) && out.l == c3);
    PQ_CHECK(pq_test_call(vm, "poll", "()Ljava/lang/Object;", compared, NULL, 0, &out) && out.l == c1);

    /* ---- null comparator means natural ordering ---- */
    dvm_ref null_comp_queue = pq_test_new_queue(vm);
    PQ_CHECK(null_comp_queue != 0);
    union dvm_value null_comp = {.l = 0};
    PQ_CHECK(pq_test_call(vm, "<init>", "(Ljava/util/Comparator;)V", null_comp_queue,
                          &null_comp, 1, &out));
    PQ_CHECK(pq_test_call(vm, "comparator", "()Ljava/util/Comparator;", null_comp_queue,
                          NULL, 0, &out) && out.l == 0);
    arg.l = i5;
    PQ_CHECK(pq_test_call(vm, "offer", "(Ljava/lang/Object;)Z", null_comp_queue, &arg, 1, &out));
    arg.l = i1;
    PQ_CHECK(pq_test_call(vm, "offer", "(Ljava/lang/Object;)Z", null_comp_queue, &arg, 1, &out));
    PQ_CHECK(pq_test_call(vm, "poll", "()Ljava/lang/Object;", null_comp_queue, NULL, 0, &out) && out.l == i1);

    /* ---- invalid capacity: real IllegalArgumentException ---- */
    dvm_ref bad_cap = pq_test_new_queue(vm);
    PQ_CHECK(bad_cap != 0);
    union dvm_value zero = {.i = 0};
    PQ_EXPECT(pq_test_call(vm, "<init>", "(I)V", bad_cap, &zero, 1, &out));

    /* ---- capacity hint and growth ---- */
    dvm_ref small = pq_test_new_queue(vm);
    PQ_CHECK(small != 0);
    union dvm_value one = {.i = 1};
    PQ_CHECK(pq_test_call(vm, "<init>", "(I)V", small, &one, 1, &out));
    dvm_ref g5 = pq_test_item_new(vm, 5);
    dvm_ref g1 = pq_test_item_new(vm, 1);
    dvm_ref g3 = pq_test_item_new(vm, 3);
    PQ_CHECK(g5 && g1 && g3);
    arg.l = g5;
    PQ_CHECK(pq_test_call(vm, "offer", "(Ljava/lang/Object;)Z", small, &arg, 1, &out));
    arg.l = g1;
    PQ_CHECK(pq_test_call(vm, "offer", "(Ljava/lang/Object;)Z", small, &arg, 1, &out));
    arg.l = g3;
    PQ_CHECK(pq_test_call(vm, "offer", "(Ljava/lang/Object;)Z", small, &arg, 1, &out));
    PQ_CHECK(pq_test_call(vm, "element", "()Ljava/lang/Object;", small, NULL, 0, &out) && out.l == g1);
    dvm_ref removed_min = 0;
    PQ_CHECK(pq_test_call(vm, "remove", "()Ljava/lang/Object;", small, NULL, 0, &out));
    removed_min = out.l;
    PQ_CHECK(removed_min == g1);
    PQ_CHECK(pq_test_call(vm, "poll", "()Ljava/lang/Object;", small, NULL, 0, &out) && out.l == g3);
    PQ_CHECK(pq_test_call(vm, "poll", "()Ljava/lang/Object;", small, NULL, 0, &out) && out.l == g5);
    PQ_EXPECT(pq_test_call(vm, "remove", "()Ljava/lang/Object;", small, NULL, 0, &out));

    /* ---- copy constructor keeps comparator and heap order ---- */
    dvm_ref copy_source = pq_test_new_queue(vm);
    dvm_ref copy = pq_test_new_queue(vm);
    PQ_CHECK(copy_source && copy);
    comp_arg.l = descending;
    PQ_CHECK(pq_test_call(vm, "<init>", "(Ljava/util/Comparator;)V", copy_source,
                          &comp_arg, 1, &out));
    dvm_ref k2 = pq_test_item_new(vm, 2);
    dvm_ref k9 = pq_test_item_new(vm, 9);
    dvm_ref k4 = pq_test_item_new(vm, 4);
    PQ_CHECK(k2 && k9 && k4);
    arg.l = k2;
    PQ_CHECK(pq_test_call(vm, "offer", "(Ljava/lang/Object;)Z", copy_source, &arg, 1, &out));
    arg.l = k9;
    PQ_CHECK(pq_test_call(vm, "offer", "(Ljava/lang/Object;)Z", copy_source, &arg, 1, &out));
    arg.l = k4;
    PQ_CHECK(pq_test_call(vm, "offer", "(Ljava/lang/Object;)Z", copy_source, &arg, 1, &out));
    union dvm_value source_arg = {.l = copy_source};
    PQ_CHECK(pq_test_call(vm, "<init>", "(Ljava/util/PriorityQueue;)V", copy, &source_arg, 1, &out));
    PQ_CHECK(pq_test_call(vm, "comparator", "()Ljava/util/Comparator;", copy, NULL, 0, &out) &&
             out.l == descending);
    PQ_CHECK(pq_test_call(vm, "poll", "()Ljava/lang/Object;", copy, NULL, 0, &out) && out.l == k9);
    PQ_CHECK(pq_test_call(vm, "poll", "()Ljava/lang/Object;", copy, NULL, 0, &out) && out.l == k4);
    PQ_CHECK(pq_test_call(vm, "poll", "()Ljava/lang/Object;", copy, NULL, 0, &out) && out.l == k2);

    /* ---- iterator and toArray expose the heap-array order ---- */
    dvm_ref iter_queue = pq_test_new_queue(vm);
    PQ_CHECK(iter_queue != 0);
    PQ_CHECK(pq_test_call(vm, "<init>", "()V", iter_queue, NULL, 0, &out));
    dvm_ref t7 = pq_test_item_new(vm, 7);
    dvm_ref t2 = pq_test_item_new(vm, 2);
    dvm_ref t6 = pq_test_item_new(vm, 6);
    PQ_CHECK(t7 && t2 && t6);
    arg.l = t7;
    PQ_CHECK(pq_test_call(vm, "offer", "(Ljava/lang/Object;)Z", iter_queue, &arg, 1, &out));
    arg.l = t2;
    PQ_CHECK(pq_test_call(vm, "offer", "(Ljava/lang/Object;)Z", iter_queue, &arg, 1, &out));
    arg.l = t6;
    PQ_CHECK(pq_test_call(vm, "offer", "(Ljava/lang/Object;)Z", iter_queue, &arg, 1, &out));
    union dvm_value array_out = {0};
    PQ_CHECK(pq_test_call(vm, "toArray", "()[Ljava/lang/Object;", iter_queue, NULL, 0, &array_out) &&
             array_out.l != 0);
    dvm_ref array = array_out.l;
    PQ_CHECK(dvm_array_length(vm, array) == 3);
    union dvm_value iter_out = {0};
    PQ_CHECK(pq_test_call(vm, "iterator", "()Ljava/util/Iterator;", iter_queue, NULL, 0, &iter_out) &&
             iter_out.l != 0);
    dvm_ref iterator_ref = iter_out.l;
    for (uint32_t i = 0; i < 3; ++i) {
        union dvm_value item_out = {0};
        PQ_CHECK(priority_queue_call_inner(vm, "java/util/Iterator", "next",
                                           "()Ljava/lang/Object;", iterator_ref, NULL, 0,
                                           &item_out));
        PQ_CHECK(item_out.l == ((dvm_ref *)dvm_array_data(vm, array))[i]);
    }
    PQ_CHECK(priority_queue_call_inner(vm, "java/util/Iterator", "hasNext", "()Z",
                                       iterator_ref, NULL, 0, &out) && out.i == 0);
    PQ_CHECK(pq_test_call(vm, "poll", "()Ljava/lang/Object;", iter_queue, NULL, 0, &out) && out.l == t2);
    PQ_CHECK(pq_test_call(vm, "poll", "()Ljava/lang/Object;", iter_queue, NULL, 0, &out) && out.l == t6);
    PQ_CHECK(pq_test_call(vm, "poll", "()Ljava/lang/Object;", iter_queue, NULL, 0, &out) && out.l == t7);

    /* ---- clear ---- */
    PQ_CHECK(pq_test_call(vm, "clear", "()V", iter_queue, NULL, 0, &out));
    PQ_CHECK(pq_test_call(vm, "size", "()I", iter_queue, NULL, 0, &out) && out.i == 0);
    PQ_CHECK(pq_test_call(vm, "isEmpty", "()Z", iter_queue, NULL, 0, &out) && out.i == 1);
    PQ_CHECK(pq_test_call(vm, "poll", "()Ljava/lang/Object;", iter_queue, NULL, 0, &out) && out.l == 0);

    bool passed = ok && !dvm_exception(vm);
    sdk_trace_fprintf(stderr,
                      "PRIORITY_QUEUE_SELFTEST %s checks=%u natural_order=1 "
                      "comparator_order=1 null_npe=1 empty_peek_poll=1 "
                      "remove_contains=1 cce=1 capacity_iae=1 copy_ctor=1 "
                      "iterator_toarray=1 clear=1\n",
                      passed ? "PASS" : "FAIL", checks);
    return passed;
}

#undef PQ_CHECK
#undef PQ_EXPECT
