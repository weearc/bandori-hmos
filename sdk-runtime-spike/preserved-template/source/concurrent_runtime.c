#include "concurrent_runtime.h"
#include "dvm/dvm_internal.h"
#include "java_basics.h"
#include "object_runtime.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

extern int sdk_trace_fprintf(FILE *, const char *, ...);

/* ------------------------------------------------------------------ */
/* Object payloads.  A payload is recognised only by its destructor, so a
 * queue can never be confused with an iterator or an AtomicReference. */

struct clq_node {
    dvm_ref value;
    struct clq_node *next;
};

struct clq {
    pthread_mutex_t lock;
    struct clq_node *head;
    struct clq_node *tail;
    size_t size;
};

struct clq_iterator {
    dvm_ref queue;
    dvm_ref *items;
    size_t count;
    size_t position;
    dvm_ref last;
    bool last_valid;
    bool removed_last;
};

struct atomic_reference {
    dvm_ref value;
};

/* Semaphore.  permits is the real permit count; waiters is maintained under
 * the same mutex while a thread is blocked in acquire/tryAcquire, so
 * getQueueLength()/hasQueuedThreads() observe real waiters rather than an
 * approximation.  fair is recorded for isFair() only: the scheduler below is
 * deliberately non-fair (release() wakes every waiter, and every waiter
 * rechecks the count), so barging is possible and is not hidden. */
struct semaphore_state {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int32_t permits;
    int32_t waiters;
    bool fair;
};

#define SEMAPHORE_POLL_MS 50

static struct dvm *active_vm;

static bool is_call(const char *method, const char *signature,
                    const char *wanted_method, const char *wanted_signature)
{
    return method && signature && !strcmp(method, wanted_method) &&
           !strcmp(signature, wanted_signature);
}

static bool fail_message(struct dvm *vm, const char *type, const char *message)
{
    dvm__throw(vm, type, "%s", message ? message : "concurrent runtime failure");
    return true;
}

static void *payload(struct dvm *vm, dvm_ref ref, void (*destroy)(void *))
{
    struct dvm_object *object = dvm__obj(vm, ref);
    if (!object || object->data_destroy != destroy) return NULL;
    return object->data;
}

static bool attach_payload(struct dvm *vm, dvm_ref ref, void *data,
                           void (*destroy)(void *))
{
    struct dvm_object *object = dvm__obj(vm, ref);
    if (!object || object->data) return false;
    object->data = data;
    object->data_destroy = destroy;
    return true;
}

static void clq_drop(void *pointer)
{
    struct clq *queue = pointer;
    if (!queue) return;
    struct clq_node *node = queue->head;
    while (node) {
        struct clq_node *next = node->next;
        free(node);
        node = next;
    }
    pthread_mutex_destroy(&queue->lock);
    free(queue);
}

static void iterator_drop(void *pointer)
{
    struct clq_iterator *iterator = pointer;
    if (!iterator) return;
    free(iterator->items);
    free(iterator);
}

static void atomic_drop(void *pointer)
{
    free(pointer);
}

static void semaphore_drop(void *pointer)
{
    struct semaphore_state *semaphore = pointer;
    if (!semaphore) return;
    pthread_cond_destroy(&semaphore->cond);
    pthread_mutex_destroy(&semaphore->lock);
    free(semaphore);
}

/* ------------------------------------------------------------------ */
/* ConcurrentLinkedQueue.  head is the first live node and tail the last one;
 * size is maintained under the same mutex so it is a real element count. */

static bool queue_init(struct dvm *vm, dvm_ref self)
{
    if (payload(vm, self, clq_drop))
        return fail_message(vm, "java/lang/IllegalStateException",
                            "ConcurrentLinkedQueue already initialized");
    struct clq *queue = calloc(1, sizeof(*queue));
    if (!queue)
        return fail_message(vm, "java/lang/OutOfMemoryError",
                            "ConcurrentLinkedQueue state");
    if (pthread_mutex_init(&queue->lock, NULL) != 0) {
        free(queue);
        return fail_message(vm, "java/lang/OutOfMemoryError",
                            "ConcurrentLinkedQueue mutex");
    }
    if (!attach_payload(vm, self, queue, clq_drop)) {
        clq_drop(queue);
        return fail_message(vm, "java/lang/IllegalStateException",
                            "ConcurrentLinkedQueue already initialized");
    }
    return true;
}

static bool queue_offer(struct dvm *vm, struct clq *queue, dvm_ref value,
                        union dvm_value *out)
{
    if (!value)
        return fail_message(vm, "java/lang/NullPointerException",
                            "ConcurrentLinkedQueue does not permit null elements");
    struct clq_node *node = calloc(1, sizeof(*node));
    if (!node)
        return fail_message(vm, "java/lang/OutOfMemoryError",
                            "ConcurrentLinkedQueue node");
    node->value = value;
    pthread_mutex_lock(&queue->lock);
    if (queue->tail)
        queue->tail->next = node;
    else
        queue->head = node;
    queue->tail = node;
    queue->size++;
    pthread_mutex_unlock(&queue->lock);
    out->i = 1;
    return true;
}

static bool queue_poll(struct dvm *vm, struct clq *queue, union dvm_value *out,
                       bool peek)
{
    (void)vm;
    pthread_mutex_lock(&queue->lock);
    struct clq_node *node = queue->head;
    dvm_ref value = node ? node->value : 0;
    /* peek only observes the head; removing it here made the following poll
     * return the second element (caught by the on-device selftest). */
    if (node && !peek) {
        queue->head = node->next;
        if (!queue->head) queue->tail = NULL;
        queue->size--;
        node->next = NULL;
    }
    pthread_mutex_unlock(&queue->lock);
    if (node && !peek) free(node);
    out->l = value;
    return true;
}

static bool queue_size(struct dvm *vm, struct clq *queue, union dvm_value *out)
{
    (void)vm;
    pthread_mutex_lock(&queue->lock);
    size_t size = queue->size;
    pthread_mutex_unlock(&queue->lock);
    out->i = (int32_t)size;
    return true;
}

static bool queue_is_empty(struct dvm *vm, struct clq *queue,
                           union dvm_value *out)
{
    (void)vm;
    pthread_mutex_lock(&queue->lock);
    bool empty = queue->size == 0;
    pthread_mutex_unlock(&queue->lock);
    out->i = empty ? 1 : 0;
    return true;
}

/* Copy the current FIFO sequence while holding the mutex.  equals() must not
 * be called under that mutex because it may execute guest code that uses the
 * queue again; the callers below compare outside the lock. */
static bool queue_snapshot(struct dvm *vm, struct clq *queue, dvm_ref **items,
                           size_t *count)
{
    (void)vm;
    *items = NULL;
    *count = 0;
    pthread_mutex_lock(&queue->lock);
    size_t size = queue->size;
    dvm_ref *copy = size ? malloc(size * sizeof(*copy)) : NULL;
    if (size && !copy) {
        pthread_mutex_unlock(&queue->lock);
        return false;
    }
    size_t used = 0;
    for (struct clq_node *node = queue->head; node && used < size;
         node = node->next)
        copy[used++] = node->value;
    pthread_mutex_unlock(&queue->lock);
    *items = copy;
    *count = used;
    return true;
}

static bool queue_contains(struct dvm *vm, struct clq *queue, dvm_ref value,
                           union dvm_value *out)
{
    if (!value) {
        out->i = 0;
        return true;
    }
    dvm_ref *items = NULL;
    size_t count = 0;
    if (!queue_snapshot(vm, queue, &items, &count))
        return fail_message(vm, "java/lang/OutOfMemoryError",
                            "ConcurrentLinkedQueue contains snapshot");
    bool found = false;
    for (size_t i = 0; i < count; ++i) {
        bool equal = java_basics_equal(vm, value, items[i]);
        if (dvm_exception(vm)) {
            free(items);
            return true;
        }
        if (equal) {
            found = true;
            break;
        }
    }
    free(items);
    out->i = found ? 1 : 0;
    return true;
}

/* Remove the first node whose value is exactly `value`.  This is used by the
 * iterator, whose remove() deletes the object it last returned. */
static bool queue_remove_reference(struct dvm *vm, dvm_ref self, dvm_ref value)
{
    struct clq *queue = payload(vm, self, clq_drop);
    if (!queue || !value) return false;
    bool removed = false;
    pthread_mutex_lock(&queue->lock);
    struct clq_node *previous = NULL;
    struct clq_node *node = queue->head;
    while (node) {
        if (node->value == value) {
            if (previous)
                previous->next = node->next;
            else
                queue->head = node->next;
            if (queue->tail == node) queue->tail = previous;
            queue->size--;
            free(node);
            removed = true;
            break;
        }
        previous = node;
        node = node->next;
    }
    pthread_mutex_unlock(&queue->lock);
    return removed;
}

static bool queue_remove(struct dvm *vm, dvm_ref self, struct clq *queue,
                         dvm_ref value, union dvm_value *out)
{
    if (!value) {
        out->i = 0;
        return true;
    }
    dvm_ref *items = NULL;
    size_t count = 0;
    if (!queue_snapshot(vm, queue, &items, &count))
        return fail_message(vm, "java/lang/OutOfMemoryError",
                            "ConcurrentLinkedQueue remove snapshot");
    dvm_ref matched = 0;
    for (size_t i = 0; i < count; ++i) {
        bool equal = java_basics_equal(vm, value, items[i]);
        if (dvm_exception(vm)) {
            free(items);
            return true;
        }
        if (equal) {
            matched = items[i];
            break;
        }
    }
    free(items);
    if (!matched) {
        out->i = 0;
        return true;
    }
    /* The guest VM serialises bytecode under its GIL, so the only matching
     * reference observed by the snapshot is still the FIFO-first candidate
     * when the mutex is reacquired. */
    out->i = queue_remove_reference(vm, self, matched) ? 1 : 0;
    return true;
}

static bool queue_clear(struct dvm *vm, struct clq *queue)
{
    (void)vm;
    pthread_mutex_lock(&queue->lock);
    struct clq_node *node = queue->head;
    queue->head = NULL;
    queue->tail = NULL;
    queue->size = 0;
    pthread_mutex_unlock(&queue->lock);
    while (node) {
        struct clq_node *next = node->next;
        free(node);
        node = next;
    }
    return true;
}

static bool queue_iterator(struct dvm *vm, dvm_ref self, struct clq *queue,
                           union dvm_value *out)
{
    dvm_ref *items = NULL;
    size_t count = 0;
    if (!queue_snapshot(vm, queue, &items, &count))
        return fail_message(vm, "java/lang/OutOfMemoryError",
                            "ConcurrentLinkedQueue iterator snapshot");
    struct dvm_class *iterator_class =
        dvm_find_class_or_external(vm, "java/util/Iterator");
    dvm_ref iterator_ref =
        iterator_class ? dvm_new_object(vm, iterator_class) : 0;
    if (!iterator_ref) {
        free(items);
        return fail_message(vm, "java/lang/OutOfMemoryError",
                            "ConcurrentLinkedQueue iterator object");
    }
    struct clq_iterator *iterator = calloc(1, sizeof(*iterator));
    if (!iterator) {
        free(items);
        return fail_message(vm, "java/lang/OutOfMemoryError",
                            "ConcurrentLinkedQueue iterator state");
    }
    iterator->queue = self;
    iterator->items = items;
    iterator->count = count;
    iterator->position = 0;
    iterator->last = 0;
    iterator->last_valid = false;
    iterator->removed_last = false;
    if (!attach_payload(vm, iterator_ref, iterator, iterator_drop)) {
        iterator_drop(iterator);
        return fail_message(vm, "java/lang/IllegalStateException",
                            "iterator object already initialized");
    }
    out->l = iterator_ref;
    return true;
}

static bool queue_to_array(struct dvm *vm, struct clq *queue,
                           union dvm_value *out)
{
    dvm_ref *items = NULL;
    size_t count = 0;
    if (!queue_snapshot(vm, queue, &items, &count))
        return fail_message(vm, "java/lang/OutOfMemoryError",
                            "ConcurrentLinkedQueue toArray snapshot");
    if (count > (size_t)0xFFFFFFFFu) {
        free(items);
        return fail_message(vm, "java/lang/OutOfMemoryError",
                            "ConcurrentLinkedQueue toArray length");
    }
    dvm_ref array = dvm_new_array(vm, 'L', "Ljava/lang/Object;",
                                  (uint32_t)count);
    if (!array) {
        free(items);
        return fail_message(vm, "java/lang/OutOfMemoryError",
                            "ConcurrentLinkedQueue toArray array");
    }
    if (count) {
        dvm_ref *target = dvm_array_data(vm, array);
        if (!target) {
            free(items);
            return fail_message(vm, "java/lang/IllegalStateException",
                                "ConcurrentLinkedQueue toArray storage");
        }
        memcpy(target, items, count * sizeof(*target));
    }
    free(items);
    out->l = array;
    return true;
}

static bool queue_call(struct dvm *vm, const char *method, const char *sig,
                       dvm_ref self, const union dvm_value *args, int nargs,
                       union dvm_value *out)
{
    bool init = is_call(method, sig, "<init>", "()V");
    bool offer = is_call(method, sig, "offer", "(Ljava/lang/Object;)Z");
    bool add = is_call(method, sig, "add", "(Ljava/lang/Object;)Z");
    bool poll = is_call(method, sig, "poll", "()Ljava/lang/Object;");
    bool peek = is_call(method, sig, "peek", "()Ljava/lang/Object;");
    bool size = is_call(method, sig, "size", "()I");
    bool empty = is_call(method, sig, "isEmpty", "()Z");
    bool contains = is_call(method, sig, "contains", "(Ljava/lang/Object;)Z");
    bool remove = is_call(method, sig, "remove", "(Ljava/lang/Object;)Z");
    bool clear = is_call(method, sig, "clear", "()V");
    bool iterate = is_call(method, sig, "iterator", "()Ljava/util/Iterator;");
    bool to_array = is_call(method, sig, "toArray", "()[Ljava/lang/Object;");
    if (!init && !offer && !add && !poll && !peek && !size && !empty &&
        !contains && !remove && !clear && !iterate && !to_array)
        return false;
    if (init) {
        if (!self) return false;
        return queue_init(vm, self);
    }
    if (!self) return false;
    struct clq *queue = payload(vm, self, clq_drop);
    if (!queue) return false;
    if (offer || add) {
        if (nargs < 1 || !args) return false;
        return queue_offer(vm, queue, args[0].l, out);
    }
    if (poll) return queue_poll(vm, queue, out, false);
    if (peek) return queue_poll(vm, queue, out, true);
    if (size) return queue_size(vm, queue, out);
    if (empty) return queue_is_empty(vm, queue, out);
    if (contains) {
        if (nargs < 1 || !args) return false;
        return queue_contains(vm, queue, args[0].l, out);
    }
    if (remove) {
        if (nargs < 1 || !args) return false;
        return queue_remove(vm, self, queue, args[0].l, out);
    }
    if (clear) return queue_clear(vm, queue);
    if (iterate) return queue_iterator(vm, self, queue, out);
    return queue_to_array(vm, queue, out);
}

/* ------------------------------------------------------------------ */
/* Weakly consistent iterator.  The element sequence is captured at
 * iterator() time, so later queue changes never throw
 * ConcurrentModificationException and never invalidate a node pointer. */

static bool iterator_call(struct dvm *vm, const char *method, const char *sig,
                          dvm_ref self, union dvm_value *out)
{
    if (!self) return false;
    struct clq_iterator *iterator = payload(vm, self, iterator_drop);
    if (!iterator) return false;
    if (is_call(method, sig, "hasNext", "()Z")) {
        out->i = iterator->position < iterator->count ? 1 : 0;
        return true;
    }
    if (is_call(method, sig, "next", "()Ljava/lang/Object;")) {
        if (iterator->position >= iterator->count)
            return fail_message(vm, "java/util/NoSuchElementException",
                                "iterator exhausted");
        iterator->last = iterator->items[iterator->position++];
        iterator->last_valid = true;
        iterator->removed_last = false;
        out->l = iterator->last;
        return true;
    }
    if (is_call(method, sig, "remove", "()V")) {
        if (!iterator->last_valid || iterator->removed_last)
            return fail_message(vm, "java/lang/IllegalStateException",
                                "remove without a preceding next");
        iterator->removed_last = true;
        queue_remove_reference(vm, iterator->queue, iterator->last);
        return true;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* AtomicReference.  The guest VM runs bytecode under a GIL; the builtins below
 * additionally give the value field the Java memory ordering (seq_cst for
 * get/set/getAndSet/compareAndSet, release for lazySet). */

static bool atomic_init(struct dvm *vm, dvm_ref self, dvm_ref value)
{
    if (payload(vm, self, atomic_drop))
        return fail_message(vm, "java/lang/IllegalStateException",
                            "AtomicReference already initialized");
    struct atomic_reference *state = calloc(1, sizeof(*state));
    if (!state)
        return fail_message(vm, "java/lang/OutOfMemoryError",
                            "AtomicReference state");
    __atomic_store_n(&state->value, value, __ATOMIC_SEQ_CST);
    if (!attach_payload(vm, self, state, atomic_drop)) {
        free(state);
        return fail_message(vm, "java/lang/IllegalStateException",
                            "AtomicReference already initialized");
    }
    return true;
}

/* String.valueOf(value): null prints "null", a String prints its own text,
 * and every other object goes through its real toString().  There is no
 * fabricated object identity here: an object with no usable toString() makes
 * the call fail closed with UnsupportedOperationException. */
static bool atomic_to_string(struct dvm *vm, struct atomic_reference *state,
                             union dvm_value *out)
{
    dvm_ref value = __atomic_load_n(&state->value, __ATOMIC_SEQ_CST);
    if (!value) {
        out->l = dvm_new_string(vm, "null");
        if (!out->l)
            return fail_message(vm, "java/lang/OutOfMemoryError",
                                "AtomicReference toString null");
        return true;
    }
    struct dvm_object *object = dvm__obj(vm, value);
    if (!object)
        return fail_message(vm, "java/lang/IllegalStateException",
                            "AtomicReference value handle");
    if (object->kind == DVM_OBJ_STRING) {
        out->l = value; /* String.toString() returns this */
        return true;
    }
    const char *class_name =
        object->cls && object->cls->name ? object->cls->name : NULL;
    if (!class_name)
        return fail_message(vm, "java/lang/IllegalStateException",
                            "AtomicReference value class");
    union dvm_value rendered = {0};
    bool handled = java_basics_call(vm, class_name, "toString",
                                    "()Ljava/lang/String;", value, NULL, 0,
                                    &rendered);
    if (dvm_exception(vm)) return true;
    if (!handled) {
        struct dvm_method *method =
            dvm_find_method(vm, object->cls, "toString",
                            "()Ljava/lang/String;");
        if (method && (method->has_code || method->builtin)) {
            if (!dvm_call(vm, method, value, NULL, 0, &rendered)) return true;
            handled = true;
        }
    }
    if (dvm_exception(vm)) return true;
    if (!handled &&
        object_runtime_call(vm, "java/lang/Object", "toString",
                            "()Ljava/lang/String;", value, NULL, 0, &rendered)) {
        if (dvm_exception(vm)) return true;
        handled = true;
    }
    if (dvm_exception(vm)) return true;
    if (!handled)
        return fail_message(vm, "java/lang/UnsupportedOperationException",
                            "AtomicReference value has no toString");
    if (!rendered.l) {
        out->l = dvm_new_string(vm, "null");
        if (!out->l)
            return fail_message(vm, "java/lang/OutOfMemoryError",
                                "AtomicReference toString null");
        return true;
    }
    struct dvm_object *text = dvm__obj(vm, rendered.l);
    if (!text || text->kind != DVM_OBJ_STRING)
        return fail_message(vm, "java/lang/ClassCastException",
                            "AtomicReference value toString did not return a String");
    out->l = rendered.l;
    return true;
}

static bool atomic_call(struct dvm *vm, const char *method, const char *sig,
                        dvm_ref self, const union dvm_value *args, int nargs,
                        union dvm_value *out)
{
    bool init = is_call(method, sig, "<init>", "()V");
    bool init_value = is_call(method, sig, "<init>",
                              "(Ljava/lang/Object;)V");
    bool get = is_call(method, sig, "get", "()Ljava/lang/Object;");
    bool set = is_call(method, sig, "set", "(Ljava/lang/Object;)V");
    bool get_and_set = is_call(method, sig, "getAndSet",
                               "(Ljava/lang/Object;)Ljava/lang/Object;");
    bool compare_and_set = is_call(method, sig, "compareAndSet",
                                   "(Ljava/lang/Object;Ljava/lang/Object;)Z");
    bool lazy_set = is_call(method, sig, "lazySet", "(Ljava/lang/Object;)V");
    bool to_string = is_call(method, sig, "toString",
                             "()Ljava/lang/String;");
    if (!init && !init_value && !get && !set && !get_and_set &&
        !compare_and_set && !lazy_set && !to_string)
        return false;
    if (init || init_value) {
        if (!self) return false;
        if (init_value && (nargs < 1 || !args)) return false;
        return atomic_init(vm, self, init_value ? args[0].l : 0);
    }
    if (!self) return false;
    struct atomic_reference *state = payload(vm, self, atomic_drop);
    if (!state) return false;
    if (get) {
        out->l = __atomic_load_n(&state->value, __ATOMIC_SEQ_CST);
        return true;
    }
    if (set || lazy_set) {
        if (nargs < 1 || !args) return false;
        if (set)
            __atomic_store_n(&state->value, args[0].l, __ATOMIC_SEQ_CST);
        else
            __atomic_store_n(&state->value, args[0].l, __ATOMIC_RELEASE);
        return true;
    }
    if (get_and_set) {
        if (nargs < 1 || !args) return false;
        out->l = __atomic_exchange_n(&state->value, args[0].l,
                                     __ATOMIC_SEQ_CST);
        return true;
    }
    if (compare_and_set) {
        if (nargs < 2 || !args) return false;
        dvm_ref expected = args[0].l;
        bool exchanged =
            __atomic_compare_exchange_n(&state->value, &expected, args[1].l, 0,
                                        __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
        out->i = exchanged ? 1 : 0;
        return true;
    }
    return atomic_to_string(vm, state, out);
}

/* ------------------------------------------------------------------ */
/* Semaphore.  Waiters block on a per-object mutex+condvar pair.  The wait
 * loop wakes at least every SEMAPHORE_POLL_MS so a VM stop can interrupt it
 * even when no release arrives, and it drops the VM GIL around the blocking
 * host wait exactly like latch_runtime.c does.  Timeouts are converted with
 * java_basics_timeunit_nanos and rounded up to whole milliseconds, matching
 * CountDownLatch.await(long, TimeUnit) in latch_runtime.c. */

static struct semaphore_state *semaphore_of(struct dvm *vm, dvm_ref self)
{
    return payload(vm, self, semaphore_drop);
}

static struct timespec semaphore_deadline_ms(uint64_t ms)
{
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += (time_t)(ms / 1000u);
    deadline.tv_nsec += (long)(ms % 1000u) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        ++deadline.tv_sec;
        deadline.tv_nsec -= 1000000000L;
    }
    return deadline;
}

static uint64_t semaphore_monotonic_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000u +
           (uint64_t)now.tv_nsec / 1000000u;
}

static bool semaphore_init(struct dvm *vm, dvm_ref self, int32_t permits,
                           bool fair)
{
    if (payload(vm, self, semaphore_drop))
        return fail_message(vm, "java/lang/IllegalStateException",
                            "Semaphore already initialized");
    if (permits < 0)
        return fail_message(vm, "java/lang/IllegalArgumentException",
                            "permits < 0");
    struct semaphore_state *semaphore = calloc(1, sizeof(*semaphore));
    if (!semaphore)
        return fail_message(vm, "java/lang/OutOfMemoryError",
                            "Semaphore state");
    if (pthread_mutex_init(&semaphore->lock, NULL) != 0) {
        free(semaphore);
        return fail_message(vm, "java/lang/OutOfMemoryError",
                            "Semaphore mutex");
    }
    if (pthread_cond_init(&semaphore->cond, NULL) != 0) {
        pthread_mutex_destroy(&semaphore->lock);
        free(semaphore);
        return fail_message(vm, "java/lang/OutOfMemoryError",
                            "Semaphore cond");
    }
    semaphore->permits = permits;
    semaphore->waiters = 0;
    semaphore->fair = fair;
    if (!attach_payload(vm, self, semaphore, semaphore_drop)) {
        semaphore_drop(semaphore);
        return fail_message(vm, "java/lang/IllegalStateException",
                            "Semaphore already initialized");
    }
    return true;
}

/* TimeUnit conversion shared with latch_runtime.c: a non-positive timeout is
 * "try once, do not wait" (0 milliseconds here), the nanosecond product
 * saturates at Long.MAX_VALUE, and milliseconds round up so a positive
 * timeout never collapses to an immediate poll. */
static bool semaphore_timeout_ms(struct dvm *vm, int64_t timeout,
                                 dvm_ref unit, int64_t *milliseconds)
{
    uint64_t nanos = 0;
    if (!java_basics_timeunit_nanos(vm, unit, &nanos)) {
        dvm__throw(vm, "java/lang/IllegalArgumentException", "TimeUnit");
        return false;
    }
    if (timeout <= 0) {
        *milliseconds = 0;
        return true;
    }
    uint64_t value = (uint64_t)timeout;
    uint64_t ms;
    if (value > ((uint64_t)INT64_MAX) / nanos) {
        ms = ((uint64_t)INT64_MAX) / 1000000u + 1u;
    } else {
        uint64_t total = value * nanos;
        ms = total / 1000000u + (total % 1000000u != 0);
    }
    if (!ms) ms = 1;
    *milliseconds = (int64_t)ms;
    return true;
}

/* timeout_ms < 0 waits until enough permits exist (or the VM stops);
 * timeout_ms == 0 tries once without waiting.  Returns true when handled;
 * out->i says whether the permits were acquired.  A VM stop raises
 * InterruptedException instead of blocking forever. */
static bool semaphore_await(struct dvm *vm, struct semaphore_state *semaphore,
                            int32_t permits, int64_t timeout_ms,
                            union dvm_value *out)
{
    bool timed = timeout_ms >= 0;
    uint64_t deadline =
        timed ? semaphore_monotonic_ms() + (uint64_t)timeout_ms : 0;
    bool counted = false;
    bool acquired = false;

    pthread_mutex_lock(&semaphore->lock);
    for (;;) {
        if (semaphore->permits >= permits) {
            semaphore->permits -= permits;
            acquired = true;
            break;
        }
        if (dvm_is_stopping(vm)) {
            pthread_mutex_unlock(&semaphore->lock);
            if (counted) {
                pthread_mutex_lock(&semaphore->lock);
                --semaphore->waiters;
                pthread_mutex_unlock(&semaphore->lock);
            }
            dvm__throw(vm, "java/lang/InterruptedException",
                       "Semaphore wait during VM shutdown");
            return true;
        }
        uint64_t slice = SEMAPHORE_POLL_MS;
        if (timed) {
            uint64_t now = semaphore_monotonic_ms();
            if (timeout_ms == 0 || now >= deadline) break;
            uint64_t remaining = deadline - now;
            if (remaining < slice) slice = remaining;
        }
        if (!counted) {
            ++semaphore->waiters;
            counted = true;
        }
        unsigned depth = dvm_gil_unlock_all(vm);
        struct timespec until = semaphore_deadline_ms(slice ? slice : 1);
        pthread_cond_timedwait(&semaphore->cond, &semaphore->lock, &until);
        pthread_mutex_unlock(&semaphore->lock);
        dvm_gil_relock(vm, depth);
        pthread_mutex_lock(&semaphore->lock);
    }
    if (counted) --semaphore->waiters;
    pthread_mutex_unlock(&semaphore->lock);
    if (out) out->i = acquired ? 1 : 0;
    return true;
}

/* Non-blocking attempt; used directly by tryAcquire() and for the
 * non-positive-timeout path of tryAcquire(long, TimeUnit). */
static bool semaphore_try_acquire_now(struct dvm *vm,
                                      struct semaphore_state *semaphore,
                                      int32_t permits, union dvm_value *out)
{
    (void)vm;
    pthread_mutex_lock(&semaphore->lock);
    bool acquired = semaphore->permits >= permits;
    if (acquired) semaphore->permits -= permits;
    pthread_mutex_unlock(&semaphore->lock);
    out->i = acquired ? 1 : 0;
    return true;
}

static bool semaphore_acquire(struct dvm *vm,
                              struct semaphore_state *semaphore,
                              int32_t permits, union dvm_value *out)
{
    if (permits <= 0)
        return fail_message(vm, "java/lang/IllegalArgumentException",
                            "permits <= 0");
    return semaphore_await(vm, semaphore, permits, -1, out);
}

static bool semaphore_try_acquire(struct dvm *vm,
                                  struct semaphore_state *semaphore,
                                  int32_t permits, union dvm_value *out)
{
    if (permits <= 0)
        return fail_message(vm, "java/lang/IllegalArgumentException",
                            "permits <= 0");
    return semaphore_try_acquire_now(vm, semaphore, permits, out);
}

static bool semaphore_try_acquire_timed(struct dvm *vm,
                                        struct semaphore_state *semaphore,
                                        int32_t permits, int64_t timeout,
                                        dvm_ref unit, union dvm_value *out)
{
    if (permits <= 0)
        return fail_message(vm, "java/lang/IllegalArgumentException",
                            "permits <= 0");
    int64_t timeout_ms = 0;
    if (!semaphore_timeout_ms(vm, timeout, unit, &timeout_ms)) return true;
    if (timeout_ms <= 0)
        return semaphore_try_acquire_now(vm, semaphore, permits, out);
    return semaphore_await(vm, semaphore, permits, timeout_ms, out);
}

static bool semaphore_release(struct dvm *vm,
                              struct semaphore_state *semaphore,
                              int32_t permits)
{
    if (permits <= 0)
        return fail_message(vm, "java/lang/IllegalArgumentException",
                            "permits <= 0");
    pthread_mutex_lock(&semaphore->lock);
    /* Java's AQS state is an int and wraps on overflow; reproduce that
     * explicitly instead of relying on signed-overflow undefined behavior. */
    semaphore->permits =
        (int32_t)((uint32_t)semaphore->permits + (uint32_t)permits);
    pthread_cond_broadcast(&semaphore->cond);
    pthread_mutex_unlock(&semaphore->lock);
    return true;
}

static bool semaphore_drain(struct dvm *vm, struct semaphore_state *semaphore,
                            union dvm_value *out)
{
    (void)vm;
    pthread_mutex_lock(&semaphore->lock);
    int32_t permits = semaphore->permits;
    semaphore->permits = 0;
    pthread_mutex_unlock(&semaphore->lock);
    out->i = permits;
    return true;
}

static bool semaphore_available(struct dvm *vm,
                                struct semaphore_state *semaphore,
                                union dvm_value *out)
{
    (void)vm;
    pthread_mutex_lock(&semaphore->lock);
    int32_t permits = semaphore->permits;
    pthread_mutex_unlock(&semaphore->lock);
    out->i = permits;
    return true;
}

static bool semaphore_queue_length(struct dvm *vm,
                                   struct semaphore_state *semaphore,
                                   union dvm_value *out)
{
    (void)vm;
    pthread_mutex_lock(&semaphore->lock);
    int32_t waiters = semaphore->waiters;
    pthread_mutex_unlock(&semaphore->lock);
    out->i = waiters;
    return true;
}

static bool semaphore_has_queued(struct dvm *vm,
                                 struct semaphore_state *semaphore,
                                 union dvm_value *out)
{
    (void)vm;
    pthread_mutex_lock(&semaphore->lock);
    bool queued = semaphore->waiters > 0;
    pthread_mutex_unlock(&semaphore->lock);
    out->i = queued ? 1 : 0;
    return true;
}

static bool semaphore_is_fair(struct dvm *vm,
                              struct semaphore_state *semaphore,
                              union dvm_value *out)
{
    (void)vm;
    pthread_mutex_lock(&semaphore->lock);
    bool fair = semaphore->fair;
    pthread_mutex_unlock(&semaphore->lock);
    out->i = fair ? 1 : 0;
    return true;
}

static bool semaphore_call(struct dvm *vm, const char *method, const char *sig,
                           dvm_ref self, const union dvm_value *args,
                           int nargs, union dvm_value *out)
{
    bool init = is_call(method, sig, "<init>", "(I)V");
    bool init_fair = is_call(method, sig, "<init>", "(IZ)V");
    bool acquire = is_call(method, sig, "acquire", "()V");
    bool acquire_n = is_call(method, sig, "acquire", "(I)V");
    bool release = is_call(method, sig, "release", "()V");
    bool release_n = is_call(method, sig, "release", "(I)V");
    bool try_acquire = is_call(method, sig, "tryAcquire", "()Z");
    bool try_acquire_timed = is_call(
        method, sig, "tryAcquire", "(JLjava/util/concurrent/TimeUnit;)Z");
    bool try_acquire_n_timed = is_call(
        method, sig, "tryAcquire", "(ILjava/util/concurrent/TimeUnit;)Z");
    bool drain = is_call(method, sig, "drainPermits", "()I");
    bool available = is_call(method, sig, "availablePermits", "()I");
    bool queue_length = is_call(method, sig, "getQueueLength", "()I");
    bool queued = is_call(method, sig, "hasQueuedThreads", "()Z");
    bool fair = is_call(method, sig, "isFair", "()Z");
    if (!init && !init_fair && !acquire && !acquire_n && !release &&
        !release_n && !try_acquire && !try_acquire_timed &&
        !try_acquire_n_timed && !drain && !available && !queue_length &&
        !queued && !fair)
        return false;
    if (init || init_fair) {
        if (!self || !args || nargs < (init_fair ? 2 : 1)) return false;
        return semaphore_init(vm, self, args[0].i,
                              init_fair ? args[1].i != 0 : false);
    }
    if (!self) return false;
    struct semaphore_state *semaphore = semaphore_of(vm, self);
    if (!semaphore)
        return fail_message(vm, "java/lang/IllegalStateException",
                            "Semaphore constructor not called");
    if (acquire) return semaphore_acquire(vm, semaphore, 1, out);
    if (acquire_n) {
        if (!args || nargs < 1) return false;
        return semaphore_acquire(vm, semaphore, args[0].i, out);
    }
    if (release) return semaphore_release(vm, semaphore, 1);
    if (release_n) {
        if (!args || nargs < 1) return false;
        return semaphore_release(vm, semaphore, args[0].i);
    }
    if (try_acquire)
        return semaphore_try_acquire(vm, semaphore, 1, out);
    if (try_acquire_timed) {
        if (!args || nargs < 2) return false;
        return semaphore_try_acquire_timed(vm, semaphore, 1, args[0].j,
                                           args[1].l, out);
    }
    if (try_acquire_n_timed) {
        if (!args || nargs < 3) return false;
        return semaphore_try_acquire_timed(vm, semaphore, args[0].i,
                                           args[1].j, args[2].l, out);
    }
    if (drain) return semaphore_drain(vm, semaphore, out);
    if (available) return semaphore_available(vm, semaphore, out);
    if (queue_length) return semaphore_queue_length(vm, semaphore, out);
    if (queued) return semaphore_has_queued(vm, semaphore, out);
    return semaphore_is_fair(vm, semaphore, out);
}

/* ------------------------------------------------------------------ */
/* Class registration */

static void declare_class(struct dvm *vm, const char *name, const char *super,
                          const char *const *interfaces, int interface_count,
                          bool is_interface)
{
    struct dvm_class *klass = dvm_find_class_or_external(vm, name);
    struct dvm_class *parent =
        super ? dvm_find_class_or_external(vm, super) : NULL;
    if (!klass || !klass->external) return;
    if (parent) klass->super = parent;
    klass->is_interface = is_interface;
    if (interface_count && !klass->ifaces) {
        klass->ifaces = calloc((size_t)interface_count, sizeof(*klass->ifaces));
        if (!klass->ifaces) return;
        klass->nifaces = interface_count;
        for (int i = 0; i < interface_count; ++i)
            klass->ifaces[i] = dvm_find_class_or_external(vm, interfaces[i]);
    }
}

static const char *const clq_interfaces[] = {
    "java/util/Queue",
    "java/util/Collection",
    "java/lang/Iterable"
};

void concurrent_runtime_vm_start(struct dvm *vm)
{
    if (!vm) return;
    active_vm = vm;
    declare_class(vm, "java/util/concurrent/ConcurrentLinkedQueue",
                  "java/lang/Object", clq_interfaces,
                  (int)(sizeof(clq_interfaces) / sizeof(clq_interfaces[0])),
                  false);
    declare_class(vm, "java/util/concurrent/atomic/AtomicReference",
                  "java/lang/Object", NULL, 0, false);
    declare_class(vm, "java/util/concurrent/Semaphore", "java/lang/Object",
                  NULL, 0, false);
    /* The iterator object is an instance of the Iterator interface; keep the
     * placeholder interface so instanceof/checkcast still see the contract. */
    declare_class(vm, "java/util/Iterator", NULL, NULL, 0, true);
}

void concurrent_runtime_vm_end(struct dvm *vm)
{
    if (vm && vm == active_vm) active_vm = NULL;
}

bool concurrent_runtime_call(struct dvm *vm, const char *class_name,
                             const char *method, const char *sig, dvm_ref self,
                             const union dvm_value *args, int nargs,
                             union dvm_value *out)
{
    if (!vm || vm != active_vm || !class_name || !method || !sig || !out)
        return false;
    if (nargs < 0 || (nargs > 0 && !args)) return false;
    struct dvm_object *receiver = self ? dvm__obj(vm, self) : NULL;
    if (receiver) {
        if (receiver->data_destroy == clq_drop)
            return queue_call(vm, method, sig, self, args, nargs, out);
        if (receiver->data_destroy == iterator_drop)
            return iterator_call(vm, method, sig, self, out);
        if (receiver->data_destroy == atomic_drop)
            return atomic_call(vm, method, sig, self, args, nargs, out);
        if (receiver->data_destroy == semaphore_drop)
            return semaphore_call(vm, method, sig, self, args, nargs, out);
    }
    if (!strcmp(class_name, "java/util/concurrent/ConcurrentLinkedQueue"))
        return queue_call(vm, method, sig, self, args, nargs, out);
    if (!strcmp(class_name, "java/util/concurrent/atomic/AtomicReference"))
        return atomic_call(vm, method, sig, self, args, nargs, out);
    if (!strcmp(class_name, "java/util/concurrent/Semaphore"))
        return semaphore_call(vm, method, sig, self, args, nargs, out);
    if (!strcmp(class_name, "java/util/Iterator"))
        return iterator_call(vm, method, sig, self, out);
    return false;
}

/* ------------------------------------------------------------------ */
/* Selftest */

static const char *test_exception_name(struct dvm *vm)
{
    dvm_ref exception = dvm_exception(vm);
    struct dvm_object *object = exception ? dvm__obj(vm, exception) : NULL;
    return object && object->cls ? object->cls->name : "0";
}

static bool test_expect_exception(struct dvm *vm, const char *type)
{
    dvm_ref exception = dvm_exception(vm);
    struct dvm_object *object = exception ? dvm__obj(vm, exception) : NULL;
    bool matches = object && object->cls && !strcmp(object->cls->name, type);
    if (exception) dvm_clear_exception(vm);
    return matches;
}

#define CLQ_CALL(method, sig, self, args, nargs, out) \
    concurrent_runtime_call(vm, "java/util/concurrent/ConcurrentLinkedQueue", \
                            (method), (sig), (self), (args), (nargs), (out))
#define IT_CALL(method, sig, self, args, nargs, out) \
    concurrent_runtime_call(vm, "java/util/Iterator", (method), (sig), (self), \
                            (args), (nargs), (out))
#define AR_CALL(method, sig, self, args, nargs, out) \
    concurrent_runtime_call(vm, "java/util/concurrent/atomic/AtomicReference", \
                            (method), (sig), (self), (args), (nargs), (out))
#define SEM_CALL(method, sig, self, args, nargs, out) \
    concurrent_runtime_call(vm, "java/util/concurrent/Semaphore", (method), \
                            (sig), (self), (args), (nargs), (out))

typedef struct {
    struct semaphore_state *semaphore;
    int delay_ms;
    int32_t permits;
    bool saw_waiter;
} semaphore_wake_arg;

/* Like latch_runtime.c's wake_latch, the worker touches only the payload
 * under its own mutex.  It never calls into the DVM, so no GIL is needed
 * while the main thread is blocked inside acquire(). */
static void *semaphore_wake_worker(void *pointer)
{
    semaphore_wake_arg *wake = pointer;
    struct timespec pause;
    pause.tv_sec = wake->delay_ms / 1000;
    pause.tv_nsec = (long)(wake->delay_ms % 1000) * 1000000L;
    nanosleep(&pause, NULL);
    pthread_mutex_lock(&wake->semaphore->lock);
    wake->saw_waiter = wake->semaphore->waiters > 0;
    wake->semaphore->permits = (int32_t)((uint32_t)wake->semaphore->permits +
                                         (uint32_t)wake->permits);
    pthread_cond_broadcast(&wake->semaphore->cond);
    pthread_mutex_unlock(&wake->semaphore->lock);
    return NULL;
}

#define CONCURRENT_CHECK(expr) do { \
    ++checks; \
    if (!(expr)) { \
        sdk_trace_fprintf(stderr, \
                          "CONCURRENT_RUNTIME_SELFTEST_FAIL line=%d expression=%s exception=%s\n", \
                          __LINE__, #expr, test_exception_name(vm)); \
        goto done; \
    } \
} while (0)

bool concurrent_runtime_selftest(struct dvm *vm)
{
    unsigned checks = 0;
    bool okay = false;
    struct dvm_class *queue_class = NULL;
    struct dvm_class *atomic_class = NULL;
    struct dvm_class *semaphore_class = NULL;
    dvm_ref queue = 0;
    dvm_ref atomic = 0;
    dvm_ref atomic2 = 0;
    dvm_ref a = 0, b = 0, c = 0;
    dvm_ref iterator = 0;
    dvm_ref iterator2 = 0;
    dvm_ref array = 0;
    dvm_ref sem = 0;
    dvm_ref sem_fair = 0;
    dvm_ref sem_uninit = 0;
    union dvm_value unit = {0};
    union dvm_value result = {0};
    union dvm_value args[3];

    if (!vm || vm != active_vm) {
        sdk_trace_fprintf(stderr,
                          "CONCURRENT_RUNTIME_SELFTEST_FAIL stage=entry vm_set=%d active_vm_set=%d same=%d\n",
                          vm ? 1 : 0, active_vm ? 1 : 0, vm == active_vm);
        return false;
    }
    if (dvm_exception(vm)) {
        sdk_trace_fprintf(stderr,
                          "CONCURRENT_RUNTIME_SELFTEST PRESTAGE exception=%s\n",
                          test_exception_name(vm));
        dvm_clear_exception(vm);
    }

    a = dvm_new_string(vm, "alpha");
    b = dvm_new_string(vm, "beta");
    c = dvm_new_string(vm, "gamma");
    CONCURRENT_CHECK(a && b && c);

    queue_class = dvm_find_class_or_external(
        vm, "java/util/concurrent/ConcurrentLinkedQueue");
    CONCURRENT_CHECK(queue_class != NULL);
    queue = dvm_new_object(vm, queue_class);
    CONCURRENT_CHECK(queue != 0);
    CONCURRENT_CHECK((CLQ_CALL("<init>", "()V", queue, NULL, 0, &result)));
    CONCURRENT_CHECK(!dvm_exception(vm));

    /* Empty queue contract. */
    CONCURRENT_CHECK((CLQ_CALL("isEmpty", "()Z", queue, NULL, 0, &result) &&
                      result.i == 1));
    CONCURRENT_CHECK((CLQ_CALL("size", "()I", queue, NULL, 0, &result) &&
                      result.i == 0));
    CONCURRENT_CHECK((CLQ_CALL("peek", "()Ljava/lang/Object;", queue, NULL, 0,
                               &result) &&
                      result.l == 0 && !dvm_exception(vm)));
    CONCURRENT_CHECK((CLQ_CALL("poll", "()Ljava/lang/Object;", queue, NULL, 0,
                               &result) &&
                      result.l == 0 && !dvm_exception(vm)));

    /* FIFO offer/add/poll/peek. */
    args[0].l = a;
    CONCURRENT_CHECK((CLQ_CALL("offer", "(Ljava/lang/Object;)Z", queue, args, 1,
                               &result) &&
                      result.i == 1 && !dvm_exception(vm)));
    args[0].l = b;
    CONCURRENT_CHECK((CLQ_CALL("add", "(Ljava/lang/Object;)Z", queue, args, 1,
                               &result) &&
                      result.i == 1 && !dvm_exception(vm)));
    CONCURRENT_CHECK((CLQ_CALL("size", "()I", queue, NULL, 0, &result) &&
                      result.i == 2));
    CONCURRENT_CHECK((CLQ_CALL("isEmpty", "()Z", queue, NULL, 0, &result) &&
                      result.i == 0));
    CONCURRENT_CHECK((CLQ_CALL("peek", "()Ljava/lang/Object;", queue, NULL, 0,
                               &result) &&
                      result.l == a && !dvm_exception(vm)));
    {
        bool poll_ok = CLQ_CALL("poll", "()Ljava/lang/Object;", queue, NULL, 0,
                                &result) && !dvm_exception(vm);
        if (!poll_ok || result.l != a)
            sdk_trace_fprintf(stderr,
                              "CONCURRENT_POLL a=%u got=%u ok=%d exception=%s\n",
                              (unsigned)a, (unsigned)result.l, poll_ok ? 1 : 0,
                              test_exception_name(vm));
        CONCURRENT_CHECK(poll_ok && result.l == a);
    }
    CONCURRENT_CHECK((CLQ_CALL("size", "()I", queue, NULL, 0, &result) &&
                      result.i == 1));
    CONCURRENT_CHECK((CLQ_CALL("peek", "()Ljava/lang/Object;", queue, NULL, 0,
                               &result) &&
                      result.l == b && !dvm_exception(vm)));

    /* contains/remove use equals(): same text in a different String ref. */
    {
        dvm_ref same_text = dvm_new_string(vm, "beta");
        CONCURRENT_CHECK(same_text != 0);
        args[0].l = same_text;
        CONCURRENT_CHECK((CLQ_CALL("contains", "(Ljava/lang/Object;)Z", queue,
                                   args, 1, &result) &&
                          result.i == 1 && !dvm_exception(vm)));
        args[0].l = c;
        CONCURRENT_CHECK((CLQ_CALL("contains", "(Ljava/lang/Object;)Z", queue,
                                   args, 1, &result) &&
                          result.i == 0 && !dvm_exception(vm)));
        args[0].l = 0;
        CONCURRENT_CHECK((CLQ_CALL("contains", "(Ljava/lang/Object;)Z", queue,
                                   args, 1, &result) &&
                          result.i == 0 && !dvm_exception(vm)));
        args[0].l = same_text;
        CONCURRENT_CHECK((CLQ_CALL("remove", "(Ljava/lang/Object;)Z", queue,
                                   args, 1, &result) &&
                          result.i == 1 && !dvm_exception(vm)));
        CONCURRENT_CHECK((CLQ_CALL("size", "()I", queue, NULL, 0, &result) &&
                          result.i == 0));
        args[0].l = same_text;
        CONCURRENT_CHECK((CLQ_CALL("remove", "(Ljava/lang/Object;)Z", queue,
                                   args, 1, &result) &&
                          result.i == 0 && !dvm_exception(vm)));
    }

    /* null offer/add are rejected with NullPointerException. */
    args[0].l = 0;
    CONCURRENT_CHECK((CLQ_CALL("offer", "(Ljava/lang/Object;)Z", queue, args, 1,
                               &result) &&
                      test_expect_exception(vm,
                                            "java/lang/NullPointerException")));
    args[0].l = 0;
    CONCURRENT_CHECK((CLQ_CALL("add", "(Ljava/lang/Object;)Z", queue, args, 1,
                               &result) &&
                      test_expect_exception(vm,
                                            "java/lang/NullPointerException")));

    /* iterator: FIFO order, weak consistency, NoSuchElementException. */
    args[0].l = a;
    CONCURRENT_CHECK((CLQ_CALL("offer", "(Ljava/lang/Object;)Z", queue, args, 1,
                               &result) &&
                      result.i == 1));
    args[0].l = b;
    CONCURRENT_CHECK((CLQ_CALL("offer", "(Ljava/lang/Object;)Z", queue, args, 1,
                               &result) &&
                      result.i == 1));
    CONCURRENT_CHECK((CLQ_CALL("iterator", "()Ljava/util/Iterator;", queue,
                               NULL, 0, &result) &&
                      result.l != 0 && !dvm_exception(vm)));
    iterator = result.l;
    /* Modifying the queue after iterator() must not throw CME. */
    args[0].l = c;
    CONCURRENT_CHECK((CLQ_CALL("offer", "(Ljava/lang/Object;)Z", queue, args, 1,
                               &result) &&
                      result.i == 1));
    CONCURRENT_CHECK((IT_CALL("hasNext", "()Z", iterator, NULL, 0, &result) &&
                      result.i == 1));
    CONCURRENT_CHECK((IT_CALL("next", "()Ljava/lang/Object;", iterator, NULL, 0,
                              &result) &&
                      result.l == a && !dvm_exception(vm)));
    CONCURRENT_CHECK((IT_CALL("hasNext", "()Z", iterator, NULL, 0, &result) &&
                      result.i == 1));
    CONCURRENT_CHECK((IT_CALL("next", "()Ljava/lang/Object;", iterator, NULL, 0,
                              &result) &&
                      result.l == b && !dvm_exception(vm)));
    CONCURRENT_CHECK((IT_CALL("hasNext", "()Z", iterator, NULL, 0, &result) &&
                      result.i == 0));
    CONCURRENT_CHECK((IT_CALL("next", "()Ljava/lang/Object;", iterator, NULL, 0,
                              &result) &&
                      test_expect_exception(vm,
                                            "java/util/NoSuchElementException")));
    CONCURRENT_CHECK((CLQ_CALL("size", "()I", queue, NULL, 0, &result) &&
                      result.i == 3));

    /* Iterator.remove deletes the last returned element for real. */
    CONCURRENT_CHECK((CLQ_CALL("clear", "()V", queue, NULL, 0, &result) &&
                      !dvm_exception(vm)));
    args[0].l = a;
    CONCURRENT_CHECK((CLQ_CALL("offer", "(Ljava/lang/Object;)Z", queue, args, 1,
                               &result) &&
                      result.i == 1));
    args[0].l = b;
    CONCURRENT_CHECK((CLQ_CALL("offer", "(Ljava/lang/Object;)Z", queue, args, 1,
                               &result) &&
                      result.i == 1));
    CONCURRENT_CHECK((CLQ_CALL("iterator", "()Ljava/util/Iterator;", queue,
                               NULL, 0, &result) &&
                      result.l != 0));
    iterator2 = result.l;
    CONCURRENT_CHECK((IT_CALL("remove", "()V", iterator2, NULL, 0, &result) &&
                      test_expect_exception(vm,
                                            "java/lang/IllegalStateException")));
    CONCURRENT_CHECK((IT_CALL("next", "()Ljava/lang/Object;", iterator2, NULL,
                              0, &result) &&
                      result.l == a && !dvm_exception(vm)));
    CONCURRENT_CHECK((IT_CALL("remove", "()V", iterator2, NULL, 0, &result) &&
                      !dvm_exception(vm)));
    args[0].l = a;
    CONCURRENT_CHECK((CLQ_CALL("contains", "(Ljava/lang/Object;)Z", queue,
                               args, 1, &result) &&
                      result.i == 0 && !dvm_exception(vm)));
    args[0].l = b;
    CONCURRENT_CHECK((CLQ_CALL("contains", "(Ljava/lang/Object;)Z", queue,
                               args, 1, &result) &&
                      result.i == 1 && !dvm_exception(vm)));
    CONCURRENT_CHECK((IT_CALL("remove", "()V", iterator2, NULL, 0, &result) &&
                      test_expect_exception(vm,
                                            "java/lang/IllegalStateException")));

    /* toArray returns an Object[] in FIFO order. */
    {
        dvm_ref *items = NULL;
        CONCURRENT_CHECK((CLQ_CALL("toArray", "()[Ljava/lang/Object;", queue,
                                   NULL, 0, &result) &&
                          result.l != 0 && !dvm_exception(vm)));
        array = result.l;
        CONCURRENT_CHECK(dvm_array_length(vm, array) == 1);
        items = dvm_array_data(vm, array);
        CONCURRENT_CHECK(items != NULL && items[0] == b);
    }

    /* clear leaves an empty queue. */
    CONCURRENT_CHECK((CLQ_CALL("clear", "()V", queue, NULL, 0, &result) &&
                      !dvm_exception(vm)));
    CONCURRENT_CHECK((CLQ_CALL("isEmpty", "()Z", queue, NULL, 0, &result) &&
                      result.i == 1));
    CONCURRENT_CHECK((CLQ_CALL("size", "()I", queue, NULL, 0, &result) &&
                      result.i == 0));

    /* AtomicReference: default null, set/get, getAndSet, compareAndSet,
     * lazySet, toString, null value round-trip. */
    atomic_class = dvm_find_class_or_external(
        vm, "java/util/concurrent/atomic/AtomicReference");
    CONCURRENT_CHECK(atomic_class != NULL);
    atomic = dvm_new_object(vm, atomic_class);
    CONCURRENT_CHECK(atomic != 0);
    CONCURRENT_CHECK((AR_CALL("<init>", "()V", atomic, NULL, 0, &result)));
    CONCURRENT_CHECK(!dvm_exception(vm));
    CONCURRENT_CHECK((AR_CALL("get", "()Ljava/lang/Object;", atomic, NULL, 0,
                              &result) &&
                      result.l == 0 && !dvm_exception(vm)));

    args[0].l = a;
    CONCURRENT_CHECK((AR_CALL("set", "(Ljava/lang/Object;)V", atomic, args, 1,
                              &result) &&
                      !dvm_exception(vm)));
    CONCURRENT_CHECK((AR_CALL("get", "()Ljava/lang/Object;", atomic, NULL, 0,
                              &result) &&
                      result.l == a && !dvm_exception(vm)));

    args[0].l = b;
    CONCURRENT_CHECK((AR_CALL("getAndSet",
                              "(Ljava/lang/Object;)Ljava/lang/Object;", atomic,
                              args, 1, &result) &&
                      result.l == a && !dvm_exception(vm)));
    CONCURRENT_CHECK((AR_CALL("get", "()Ljava/lang/Object;", atomic, NULL, 0,
                              &result) &&
                      result.l == b && !dvm_exception(vm)));

    args[0].l = b;
    args[1].l = c;
    CONCURRENT_CHECK((AR_CALL("compareAndSet",
                              "(Ljava/lang/Object;Ljava/lang/Object;)Z", atomic,
                              args, 2, &result) &&
                      result.i == 1 && !dvm_exception(vm)));
    CONCURRENT_CHECK((AR_CALL("get", "()Ljava/lang/Object;", atomic, NULL, 0,
                              &result) &&
                      result.l == c && !dvm_exception(vm)));

    /* A failed compareAndSet leaves the value untouched. */
    args[0].l = a;
    args[1].l = b;
    CONCURRENT_CHECK((AR_CALL("compareAndSet",
                              "(Ljava/lang/Object;Ljava/lang/Object;)Z", atomic,
                              args, 2, &result) &&
                      result.i == 0 && !dvm_exception(vm)));
    CONCURRENT_CHECK((AR_CALL("get", "()Ljava/lang/Object;", atomic, NULL, 0,
                              &result) &&
                      result.l == c && !dvm_exception(vm)));

    args[0].l = b;
    CONCURRENT_CHECK((AR_CALL("lazySet", "(Ljava/lang/Object;)V", atomic, args,
                              1, &result) &&
                      !dvm_exception(vm)));
    CONCURRENT_CHECK((AR_CALL("get", "()Ljava/lang/Object;", atomic, NULL, 0,
                              &result) &&
                      result.l == b && !dvm_exception(vm)));
    CONCURRENT_CHECK((AR_CALL("toString", "()Ljava/lang/String;", atomic, NULL,
                              0, &result) &&
                      result.l != 0 && !dvm_exception(vm) &&
                      dvm_string_utf8(vm, result.l) &&
                      !strcmp(dvm_string_utf8(vm, result.l), "beta")));

    /* null is a legal AtomicReference value. */
    args[0].l = 0;
    CONCURRENT_CHECK((AR_CALL("set", "(Ljava/lang/Object;)V", atomic, args, 1,
                              &result) &&
                      !dvm_exception(vm)));
    CONCURRENT_CHECK((AR_CALL("get", "()Ljava/lang/Object;", atomic, NULL, 0,
                              &result) &&
                      result.l == 0 && !dvm_exception(vm)));
    CONCURRENT_CHECK((AR_CALL("toString", "()Ljava/lang/String;", atomic, NULL,
                              0, &result) &&
                      result.l != 0 && !dvm_exception(vm) &&
                      dvm_string_utf8(vm, result.l) &&
                      !strcmp(dvm_string_utf8(vm, result.l), "null")));

    /* The Object constructor stores its argument, including null. */
    atomic2 = dvm_new_object(vm, atomic_class);
    CONCURRENT_CHECK(atomic2 != 0);
    args[0].l = a;
    CONCURRENT_CHECK((AR_CALL("<init>", "(Ljava/lang/Object;)V", atomic2, args,
                              1, &result) &&
                      !dvm_exception(vm)));
    CONCURRENT_CHECK((AR_CALL("get", "()Ljava/lang/Object;", atomic2, NULL, 0,
                              &result) &&
                      result.l == a && !dvm_exception(vm)));

    /* toString executes the value's real toString for a non-String object. */
    {
        struct dvm_class *object_class =
            dvm_find_class_or_external(vm, "java/lang/Object");
        dvm_ref plain = object_class ? dvm_new_object(vm, object_class) : 0;
        struct dvm_object *text = NULL;
        CONCURRENT_CHECK(plain != 0);
        args[0].l = plain;
        CONCURRENT_CHECK((AR_CALL("set", "(Ljava/lang/Object;)V", atomic2,
                                  args, 1, &result) &&
                          !dvm_exception(vm)));
        CONCURRENT_CHECK((AR_CALL("toString", "()Ljava/lang/String;", atomic2,
                                  NULL, 0, &result) &&
                          result.l != 0 && !dvm_exception(vm)));
        text = dvm__obj(vm, result.l);
        CONCURRENT_CHECK(text && text->kind == DVM_OBJ_STRING && text->utf8 &&
                         text->utf8_len > 0);
    }

    /* Semaphore: initial permits, non-blocking tryAcquire, release, drain. */
    semaphore_class = dvm_find_class_or_external(
        vm, "java/util/concurrent/Semaphore");
    CONCURRENT_CHECK(semaphore_class != NULL);
    sem = dvm_new_object(vm, semaphore_class);
    CONCURRENT_CHECK(sem != 0);
    args[0].i = 2;
    CONCURRENT_CHECK((SEM_CALL("<init>", "(I)V", sem, args, 1, &result) &&
                      !dvm_exception(vm)));
    CONCURRENT_CHECK((SEM_CALL("availablePermits", "()I", sem, NULL, 0,
                               &result) && result.i == 2));
    CONCURRENT_CHECK((SEM_CALL("isFair", "()Z", sem, NULL, 0, &result) &&
                      result.i == 0));
    CONCURRENT_CHECK((SEM_CALL("getQueueLength", "()I", sem, NULL, 0,
                               &result) && result.i == 0));
    CONCURRENT_CHECK((SEM_CALL("hasQueuedThreads", "()Z", sem, NULL, 0,
                               &result) && result.i == 0));
    CONCURRENT_CHECK((SEM_CALL("acquire", "()V", sem, NULL, 0, &result) &&
                      !dvm_exception(vm)));
    CONCURRENT_CHECK((SEM_CALL("availablePermits", "()I", sem, NULL, 0,
                               &result) && result.i == 1));
    CONCURRENT_CHECK((SEM_CALL("tryAcquire", "()Z", sem, NULL, 0, &result) &&
                      result.i == 1 && !dvm_exception(vm)));
    CONCURRENT_CHECK((SEM_CALL("availablePermits", "()I", sem, NULL, 0,
                               &result) && result.i == 0));
    CONCURRENT_CHECK((SEM_CALL("tryAcquire", "()Z", sem, NULL, 0, &result) &&
                      result.i == 0 && !dvm_exception(vm)));
    CONCURRENT_CHECK((SEM_CALL("release", "()V", sem, NULL, 0, &result) &&
                      !dvm_exception(vm)));
    CONCURRENT_CHECK((SEM_CALL("availablePermits", "()I", sem, NULL, 0,
                               &result) && result.i == 1));
    args[0].i = 2;
    CONCURRENT_CHECK((SEM_CALL("release", "(I)V", sem, args, 1, &result) &&
                      !dvm_exception(vm)));
    CONCURRENT_CHECK((SEM_CALL("availablePermits", "()I", sem, NULL, 0,
                               &result) && result.i == 3));
    CONCURRENT_CHECK((SEM_CALL("drainPermits", "()I", sem, NULL, 0, &result) &&
                      result.i == 3 && !dvm_exception(vm)));
    CONCURRENT_CHECK((SEM_CALL("availablePermits", "()I", sem, NULL, 0,
                               &result) && result.i == 0));

    /* acquire(int) takes a whole batch, and a non-positive batch count is
     * rejected before any wait. */
    args[0].i = 2;
    CONCURRENT_CHECK((SEM_CALL("release", "(I)V", sem, args, 1, &result) &&
                      !dvm_exception(vm)));
    args[0].i = 2;
    CONCURRENT_CHECK((SEM_CALL("acquire", "(I)V", sem, args, 1, &result) &&
                      !dvm_exception(vm)));
    CONCURRENT_CHECK((SEM_CALL("availablePermits", "()I", sem, NULL, 0,
                               &result) && result.i == 0));
    args[0].i = 0;
    CONCURRENT_CHECK((SEM_CALL("acquire", "(I)V", sem, args, 1, &result) &&
                      test_expect_exception(
                          vm, "java/lang/IllegalArgumentException")));
    args[0].i = -1;
    CONCURRENT_CHECK((SEM_CALL("release", "(I)V", sem, args, 1, &result) &&
                      test_expect_exception(
                          vm, "java/lang/IllegalArgumentException")));

    /* A negative initial count throws IAE and leaves the object
     * uninitialized; methods then throw ISE, exactly like latch. */
    sem_uninit = dvm_new_object(vm, semaphore_class);
    CONCURRENT_CHECK(sem_uninit != 0);
    args[0].i = -1;
    CONCURRENT_CHECK((SEM_CALL("<init>", "(I)V", sem_uninit, args, 1,
                               &result) &&
                      test_expect_exception(
                          vm, "java/lang/IllegalArgumentException")));
    CONCURRENT_CHECK((SEM_CALL("availablePermits", "()I", sem_uninit, NULL, 0,
                               &result) &&
                      test_expect_exception(
                          vm, "java/lang/IllegalStateException")));
    CONCURRENT_CHECK((SEM_CALL("acquire", "()V", sem_uninit, NULL, 0,
                               &result) &&
                      test_expect_exception(
                          vm, "java/lang/IllegalStateException")));

    /* fair is recorded verbatim for isFair(); dispatch remains non-fair. */
    sem_fair = dvm_new_object(vm, semaphore_class);
    CONCURRENT_CHECK(sem_fair != 0);
    args[0].i = 1;
    args[1].i = 1;
    CONCURRENT_CHECK((SEM_CALL("<init>", "(IZ)V", sem_fair, args, 2,
                               &result) &&
                      !dvm_exception(vm)));
    CONCURRENT_CHECK((SEM_CALL("isFair", "()Z", sem_fair, NULL, 0, &result) &&
                      result.i == 1));
    CONCURRENT_CHECK((SEM_CALL("availablePermits", "()I", sem_fair, NULL, 0,
                               &result) && result.i == 1));

    CONCURRENT_CHECK(java_basics_static(vm, "java/util/concurrent/TimeUnit",
                                       "MILLISECONDS",
                                       "Ljava/util/concurrent/TimeUnit;",
                                       &unit) &&
                     unit.l != 0);

    /* permits=0: a positive timeout waits and times out; zero and negative
     * timeouts try once and return false without throwing. */
    args[0].j = 10;
    args[1].l = unit.l;
    CONCURRENT_CHECK((SEM_CALL("tryAcquire",
                               "(JLjava/util/concurrent/TimeUnit;)Z", sem,
                               args, 2, &result) &&
                      result.i == 0 && !dvm_exception(vm)));
    args[0].j = 0;
    args[1].l = unit.l;
    CONCURRENT_CHECK((SEM_CALL("tryAcquire",
                               "(JLjava/util/concurrent/TimeUnit;)Z", sem,
                               args, 2, &result) &&
                      result.i == 0 && !dvm_exception(vm)));
    args[0].j = -1;
    args[1].l = unit.l;
    CONCURRENT_CHECK((SEM_CALL("tryAcquire",
                               "(JLjava/util/concurrent/TimeUnit;)Z", sem,
                               args, 2, &result) &&
                      result.i == 0 && !dvm_exception(vm)));

    /* tryAcquire(int, TimeUnit) waits for the whole batch. */
    args[0].i = 2;
    args[1].j = 10;
    args[2].l = unit.l;
    CONCURRENT_CHECK((SEM_CALL("tryAcquire",
                               "(ILjava/util/concurrent/TimeUnit;)Z", sem,
                               args, 3, &result) &&
                      result.i == 0 && !dvm_exception(vm)));
    args[0].i = 2;
    CONCURRENT_CHECK((SEM_CALL("release", "(I)V", sem, args, 1, &result) &&
                      !dvm_exception(vm)));
    args[0].i = 2;
    args[1].j = 10;
    args[2].l = unit.l;
    CONCURRENT_CHECK((SEM_CALL("tryAcquire",
                               "(ILjava/util/concurrent/TimeUnit;)Z", sem,
                               args, 3, &result) &&
                      result.i == 1 && !dvm_exception(vm)));
    CONCURRENT_CHECK((SEM_CALL("availablePermits", "()I", sem, NULL, 0,
                               &result) && result.i == 0));
    args[0].i = -1;
    args[1].j = 0;
    args[2].l = unit.l;
    CONCURRENT_CHECK((SEM_CALL("tryAcquire",
                               "(ILjava/util/concurrent/TimeUnit;)Z", sem,
                               args, 3, &result) &&
                      test_expect_exception(
                          vm, "java/lang/IllegalArgumentException")));

    /* Cross-thread wakeup: acquire() really blocks (the worker observes
     * waiters>0 under the payload mutex) and a release after 100ms wakes it. */
    {
        struct semaphore_state *state = semaphore_of(vm, sem);
        semaphore_wake_arg wake = {0};
        pthread_t worker;
        bool created = false;
        bool acquire_ok = false;
        CONCURRENT_CHECK(state != NULL);
        wake.semaphore = state;
        wake.delay_ms = 100;
        wake.permits = 1;
        wake.saw_waiter = false;
        created =
            pthread_create(&worker, NULL, semaphore_wake_worker, &wake) == 0;
        if (created)
            acquire_ok = SEM_CALL("acquire", "()V", sem, NULL, 0, &result);
        if (created) pthread_join(worker, NULL);
        CONCURRENT_CHECK(created);
        CONCURRENT_CHECK(acquire_ok && !dvm_exception(vm));
        CONCURRENT_CHECK(wake.saw_waiter);
        CONCURRENT_CHECK((SEM_CALL("availablePermits", "()I", sem, NULL, 0,
                                   &result) && result.i == 0));
        CONCURRENT_CHECK((SEM_CALL("hasQueuedThreads", "()Z", sem, NULL, 0,
                                   &result) && result.i == 0));
    }

    okay = true;
done:
    if (okay) {
        sdk_trace_fprintf(
            stderr,
            "CONCURRENT_RUNTIME_SELFTEST PASS checks=%u fifo=1 null_rejected=1 "
            "equals=1 iterator=1 iterator_remove=1 toarray=1 atomic=1 "
            "semaphore=1\n",
            checks);
    } else {
        sdk_trace_fprintf(stderr, "CONCURRENT_RUNTIME_SELFTEST FAIL\n");
        if (dvm_exception(vm)) dvm_clear_exception(vm);
    }
    return okay;
}
