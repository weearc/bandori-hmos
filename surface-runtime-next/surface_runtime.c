/*
 * Guest android.view.Surface identity, trusted binding registry and the
 * server side of ANativeWindow_fromSurface/toSurface (see native_window_jni.c
 * for the JNI entry points).
 *
 * Binding lifetime is the core invariant here:
 *   - A binding owns exactly one window_runtime reference.
 *   - surface_runtime_unbind() / surface_runtime_vm_end() release it exactly
 *     once; the payload destructor never touches the window because the
 *     registry is the sole owner of that reference.
 *   - Every record carries this module's VM generation, not just the (reused)
 *     VM pointer; a new VM with the same address cannot see old bindings.
 */
#include "surface_runtime.h"
#include "gc_runtime.h"
#include "window_runtime.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int sdk_trace_fprintf(FILE *, const char *, ...);

#define SURFACE_PAYLOAD_MAGIC 0x53555246u /* 'SURF' */

struct surface_payload {
    uint32_t magic;
    bool host_owned; /* created by the trusted host/factory path */
};

struct surface_vm {
    struct dvm *vm;
    uint64_t serial;
    dvm_ref host_surface;
    bool used;
};

struct surface_binding {
    struct dvm *vm;
    uint64_t serial;
    dvm_ref object_ref;
    void *window; /* window_runtime token; holds one reference */
    uint64_t window_generation;
    bool alive;
};

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct surface_vm g_vms[SURFACE_RUNTIME_MAX_VMS];
static struct surface_binding g_bindings[SURFACE_RUNTIME_MAX_BINDINGS];
static bool surface_gc_roots(struct dvm *vm,dvm_gc_visit_fn visit,void *user,void *context){
    if(!vm||!visit||context!=g_vms||!dvm_gil_held(vm))return false;
    if(pthread_mutex_trylock(&g_lock))return false;
    for(unsigned i=0;i<SURFACE_RUNTIME_MAX_VMS;i++){
        struct surface_vm *record=&g_vms[i];
        if(!record->used||record->vm!=vm)continue;
        /* Current host Surface and live native-window leases own these
         * identities. Historical/unbound table slots are not roots. */
        if(record->host_surface)visit(&record->host_surface,user);
        for(unsigned j=0;j<SURFACE_RUNTIME_MAX_BINDINGS;j++){
            struct surface_binding *binding=&g_bindings[j];
            if(binding->alive&&binding->vm==vm&&binding->serial==record->serial&&binding->object_ref)
                visit(&binding->object_ref,user);
        }
    }
    pthread_mutex_unlock(&g_lock);return true;
}
static bool surface_gc_ensure(struct dvm *vm){
    if(!vm)return false;
    if(dvm_gc_root_provider_add(vm,"surface-runtime/host",DVM_GC_ROOT_STRONG,surface_gc_roots,g_vms))return true;
    if(!dvm_exception(vm))dvm__throw(vm,"java/lang/OutOfMemoryError","Surface host GC roots");return false;
}
/* Our own VM generation counter.  dvm_runtime_install() runs before dvm_create
 * assigns vm->serial, so the DVM serial is still 0 at vm_start; this counter
 * gives every VM instance a stable generation from the first call onward. */
static uint64_t g_surface_vm_serial;

static bool is_call(const char *method, const char *sig,
                    const char *want_method, const char *want_sig)
{
    return method && sig && !strcmp(method, want_method) && !strcmp(sig, want_sig);
}

static void surface_payload_drop(void *data)
{
    free(data);
}

static bool surface_payload_attach(struct dvm *vm, dvm_ref ref, bool host_owned)
{
    struct dvm_object *object = dvm__obj(vm, ref);
    struct surface_payload *payload;
    if (!object || !object->cls || !object->cls->name ||
        strcmp(object->cls->name,"android/view/Surface") || object->data) return false;
    payload = calloc(1, sizeof(*payload));
    if (!payload) {
        dvm__throw(vm, "java/lang/OutOfMemoryError", "Surface payload");
        return false;
    }
    payload->magic = SURFACE_PAYLOAD_MAGIC;
    payload->host_owned = host_owned;
    object->data = payload;
    object->data_destroy = surface_payload_drop;
    return true;
}

static struct surface_payload *surface_payload_of(struct dvm *vm, dvm_ref ref)
{
    struct dvm_object *object = dvm__obj(vm, ref);
    struct surface_payload *payload;
    if (!object || object->data_destroy != surface_payload_drop) return NULL;
    payload = object->data;
    if (!payload || payload->magic != SURFACE_PAYLOAD_MAGIC) return NULL;
    return payload;
}

bool surface_runtime_is_surface(struct dvm *vm, dvm_ref ref)
{
    return surface_payload_of(vm, ref) != NULL;
}

/* Caller must hold g_lock.  The address alone is enough here because vm_start
 * clears any earlier generation at the same address, so at most one live
 * record can point at a given VM allocation. */
static struct surface_vm *find_vm_locked(struct dvm *vm)
{
    for (unsigned i = 0; i < SURFACE_RUNTIME_MAX_VMS; ++i) {
        struct surface_vm *record = &g_vms[i];
        if (record->used && record->vm == vm) return record;
    }
    return NULL;
}

/* Caller must hold g_lock. */
static struct surface_binding *find_binding_locked(struct dvm *vm,
                                                   dvm_ref ref)
{
    struct surface_vm *record = find_vm_locked(vm);
    if (!record) return NULL;
    for (unsigned i = 0; i < SURFACE_RUNTIME_MAX_BINDINGS; ++i) {
        struct surface_binding *binding = &g_bindings[i];
        if (binding->alive && binding->vm == vm && binding->serial == record->serial &&
            binding->object_ref == ref)
            return binding;
    }
    return NULL;
}

void surface_runtime_vm_start(struct dvm *vm)
{
    struct surface_vm *slot = NULL;
    void *stale_windows[SURFACE_RUNTIME_MAX_BINDINGS];
    size_t stale_count = 0;
    if (!vm) return;
    (void)dvm_find_class_or_external(vm, "android/view/Surface");
    pthread_mutex_lock(&g_lock);
    /* vm_start is called exactly once by the runtime installer for each VM.
     * Never compare our generation counter against the unrelated DVM serial. */
    if (!slot) {
        /* A reused malloc address with a different serial is an earlier VM
         * generation that did not reach vm_end (abnormal path).  Release its
         * stale bindings as a backstop; normal destruction already did. */
        for (unsigned i = 0; i < SURFACE_RUNTIME_MAX_VMS; ++i) {
            struct surface_vm *record = &g_vms[i];
            if (record->used && record->vm == vm) {
                for (unsigned j = 0; j < SURFACE_RUNTIME_MAX_BINDINGS; ++j) {
                    struct surface_binding *binding = &g_bindings[j];
                    if (binding->alive && binding->vm == vm &&
                        binding->serial == record->serial) {
                        binding->alive = false;
                        if (stale_count < SURFACE_RUNTIME_MAX_BINDINGS)
                            stale_windows[stale_count++] = binding->window;
                        binding->window = NULL;
                    }
                }
                record->used = false;
                record->host_surface = 0;
            }
        }
        for (unsigned i = 0; i < SURFACE_RUNTIME_MAX_VMS; ++i) {
            if (!g_vms[i].used) {
                slot = &g_vms[i];
                break;
            }
        }
    }
    if (slot) {
        slot->vm = vm;
        slot->serial = ++g_surface_vm_serial;
        slot->host_surface = 0;
        slot->used = true;
    }
    pthread_mutex_unlock(&g_lock);
    for (size_t i = 0; i < stale_count; ++i)
        window_runtime_release(stale_windows[i]);
    if (stale_count)
        sdk_trace_fprintf(stderr,
            "SURFACE_STALE_VM_DETACH vm=%p new_serial=%llu released=%zu\n",
            (void *)vm, slot ? (unsigned long long)slot->serial : 0, stale_count);
    sdk_trace_fprintf(stderr, "SURFACE_RUNTIME_VM_START vm=%p serial=%llu slot=%d\n",
                      (void *)vm, slot ? (unsigned long long)slot->serial : 0,
                      slot != NULL);
}

void surface_runtime_vm_end(struct dvm *vm)
{
    void *released[SURFACE_RUNTIME_MAX_BINDINGS];
    size_t released_count = 0;
    if (!vm) return;
    dvm_gc_root_provider_remove(vm,"surface-runtime/host",g_vms);
    pthread_mutex_lock(&g_lock);
    for (unsigned i = 0; i < SURFACE_RUNTIME_MAX_VMS; ++i) {
        if (g_vms[i].used && g_vms[i].vm == vm) {
            g_vms[i].used = false;
            g_vms[i].host_surface = 0;
        }
    }
    for (unsigned i = 0; i < SURFACE_RUNTIME_MAX_BINDINGS; ++i) {
        struct surface_binding *binding = &g_bindings[i];
        if (binding->alive && binding->vm == vm) {
            binding->alive = false;
            if (released_count < SURFACE_RUNTIME_MAX_BINDINGS)
                released[released_count++] = binding->window;
            binding->window = NULL;
        }
    }
    pthread_mutex_unlock(&g_lock);
    for (size_t i = 0; i < released_count; ++i)
        window_runtime_release(released[i]);
    sdk_trace_fprintf(stderr,
        "SURFACE_RUNTIME_VM_END vm=%p serial=%llu released=%zu\n",
        (void *)vm, (unsigned long long)vm->serial, released_count);
}

dvm_ref surface_runtime_host_surface(struct dvm *vm)
{
    if(!surface_gc_ensure(vm))return 0;
    dvm_ref existing = 0;
    dvm_ref created = 0;
    struct dvm_class *klass;
    pthread_mutex_lock(&g_lock);
    {
        struct surface_vm *record = find_vm_locked(vm);
        if (record) existing = record->host_surface;
    }
    pthread_mutex_unlock(&g_lock);
    if (existing && surface_runtime_is_surface(vm, existing)) return existing;

    klass = dvm_find_class_or_external(vm, "android/view/Surface");
    if (!klass) return 0;
    created = dvm_new_object(vm, klass);
    if (!created || dvm_exception(vm)) return 0;
    if (!surface_payload_attach(vm, created, true)) return 0;

    pthread_mutex_lock(&g_lock);
    {
        struct surface_vm *record = find_vm_locked(vm);
        if (record && !record->host_surface) record->host_surface = created;
        else if (record && record->host_surface) existing = record->host_surface;
    }
    pthread_mutex_unlock(&g_lock);
    if (existing) {
        /* Another thread published first; keep its object (no GC in the DVM,
         * the extra object stays inert and unbound) and continue with the
         * published one so the lazy bind still applies to it. */
        created = existing;
    }

    /* Lazy explicit bind: only the trusted factory path may turn the host
     * Surface into a valid window carrier.  Failure is reported and leaves the
     * object unbound (isValid false), never a fabricated window. */
    if (!surface_runtime_lookup_window(vm, created, NULL, NULL)) {
        void *window = window_runtime_acquire_current();
        if (window) {
            /* surface_runtime_bind() acquires the reference the binding owns;
             * the acquire_current() reference is temporary and must be
             * dropped either way, so the binding owns exactly one lease. */
            bool bound = surface_runtime_bind(vm, created, window, 0);
            window_runtime_release(window);
            if (!bound)
                sdk_trace_fprintf(stderr,
                    "SURFACE_HOST_BIND_FAIL vm=%p serial=%llu\n", (void *)vm,
                    (unsigned long long)vm->serial);
        } else {
            sdk_trace_fprintf(stderr,
                "SURFACE_HOST_WINDOW_UNAVAILABLE vm=%p serial=%llu\n", (void *)vm,
                (unsigned long long)vm->serial);
        }
    }
    return created;
}

bool surface_runtime_bind(struct dvm *vm, dvm_ref ref, void *window,
                          uint64_t window_generation)
{
    if(!surface_gc_ensure(vm))return false;
    struct surface_binding *slot = NULL;
    struct surface_vm *record;
    if (!vm || !ref || !window) return false;
    if (!surface_runtime_is_surface(vm, ref)) return false;
    pthread_mutex_lock(&g_lock);
    record = find_vm_locked(vm);
    if (!record) {
        pthread_mutex_unlock(&g_lock);
        return false;
    }
    if (find_binding_locked(vm, ref)) {
        pthread_mutex_unlock(&g_lock);
        return false; /* already bound: never acquire a second lease */
    }
    for (unsigned i = 0; i < SURFACE_RUNTIME_MAX_BINDINGS; ++i) {
        if (!g_bindings[i].alive) {
            slot = &g_bindings[i];
            break;
        }
    }
    if (!slot) {
        pthread_mutex_unlock(&g_lock);
        return false;
    }
    uint64_t actual_generation=0;
    if(!window_runtime_acquire_alive(window,&actual_generation)) {
        pthread_mutex_unlock(&g_lock);return false;
    }
    if(window_generation&&window_generation!=actual_generation) {
        pthread_mutex_unlock(&g_lock);window_runtime_release(window);return false;
    }
    slot->vm = vm;
    slot->serial = record->serial;
    slot->object_ref = ref;
    slot->window = window;
    slot->window_generation = actual_generation;
    slot->alive = true;
    uint64_t serial_for_log = record->serial;
    pthread_mutex_unlock(&g_lock);
    sdk_trace_fprintf(stderr,
        "SURFACE_BIND vm=%p serial=%llu ref=%u window=%p generation=%llu\n",
        (void *)vm, (unsigned long long)serial_for_log, ref, window,
        (unsigned long long)actual_generation);
    return true;
}

dvm_ref surface_runtime_new_surface_unbound(struct dvm *vm)
{
    if (!vm) return 0;
    dvm_ref ref = dvm_new_object(vm, dvm_find_class_or_external(vm, "android/view/Surface"));
    if (!ref || dvm_exception(vm) || !surface_payload_attach(vm, ref, true)) return 0;
    return ref;
}
dvm_ref surface_runtime_new_surface_bound(struct dvm *vm, void *window,
                                          uint64_t window_generation)
{
    dvm_ref ref;
    if (!vm || !window) return 0;
    ref = dvm_new_object(vm, dvm_find_class_or_external(vm, "android/view/Surface"));
    if (!ref || dvm_exception(vm)) return 0;
    if (!surface_payload_attach(vm, ref, true)) return 0;
    if (!surface_runtime_bind(vm, ref, window, window_generation)) return 0;
    return ref;
}

bool surface_runtime_lookup_window(struct dvm *vm, dvm_ref ref,
                                   void **window, uint64_t *window_generation)
{
    struct surface_binding *binding;
    void *candidate=NULL;uint64_t generation=0;
    if(window)*window=NULL;
    if(window_generation)*window_generation=0;
    if (!vm || !ref) return false;
    pthread_mutex_lock(&g_lock);
    binding = find_binding_locked(vm, ref);
    if (binding) {
        candidate=binding->window;generation=binding->window_generation;
    }
    pthread_mutex_unlock(&g_lock);
    if(!candidate||!window_runtime_acquire_alive(candidate,NULL))return false;
    window_runtime_release(candidate);
    if(window)*window=candidate;
    if(window_generation)*window_generation=generation;
    return true;
}

bool surface_runtime_find_by_window(struct dvm *vm, void *window, dvm_ref *out_ref,
                                    uint64_t *window_generation)
{
    struct surface_vm *record;
    dvm_ref found_ref = 0;
    uint64_t found_generation = 0;
    bool found = false;
    if (!vm || !window) return false;
    pthread_mutex_lock(&g_lock);
    record = find_vm_locked(vm);
    if (record) {
        for (unsigned i = 0; i < SURFACE_RUNTIME_MAX_BINDINGS; ++i) {
            struct surface_binding *binding = &g_bindings[i];
            if (binding->alive && binding->vm == vm &&
                binding->serial == record->serial && binding->window == window) {
                found_ref = binding->object_ref;
                found_generation = binding->window_generation;
                found = true;
                break;
            }
        }
    }
    pthread_mutex_unlock(&g_lock);
    if (found) {
        if (out_ref) *out_ref = found_ref;
        if (window_generation) *window_generation = found_generation;
    }
    return found;
}

bool surface_runtime_unbind(struct dvm *vm, dvm_ref ref)
{
    void *window = NULL;
    bool released = false;
    if (!vm || !ref) return false;
    pthread_mutex_lock(&g_lock);
    {
        struct surface_binding *binding = find_binding_locked(vm, ref);
        if (binding) {
            binding->alive = false;
            window = binding->window;
            binding->window = NULL;
            released = true;
        }
    }
    pthread_mutex_unlock(&g_lock);
    if (!released) return false;
    window_runtime_release(window);
    sdk_trace_fprintf(stderr,
        "SURFACE_UNBIND vm=%p serial=%llu ref=%u window=%p\n", (void *)vm,
        (unsigned long long)vm->serial, ref, window);
    return true;
}

uint32_t surface_runtime_binding_count(struct dvm *vm)
{
    struct surface_vm *record;
    uint32_t count = 0;
    if (!vm) return 0;
    pthread_mutex_lock(&g_lock);
    record = find_vm_locked(vm);
    if (record) {
        for (unsigned i = 0; i < SURFACE_RUNTIME_MAX_BINDINGS; ++i) {
            if (g_bindings[i].alive && g_bindings[i].vm == vm &&
                g_bindings[i].serial == record->serial)
                ++count;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return count;
}

bool surface_runtime_call(struct dvm *vm, const char *cls,
                          const char *method, const char *sig,
                          dvm_ref self, const union dvm_value *args,
                          int nargs, union dvm_value *out)
{
    (void)args;
    if (!vm || !cls || strcmp(cls, "android/view/Surface")) return false;
    if (is_call(method, sig, "<init>", "()V") && nargs == 0) {
        /* Plain construction succeeds as an identity, but never binds and is
         * never valid.  Payload creation failure surfaces as OOM. */
        if (!surface_runtime_is_surface(vm, self) &&
            !surface_payload_attach(vm, self, false))
            return true;
        return true;
    }
    if (is_call(method, sig, "isValid", "()Z") && nargs == 0) {
        out->i = surface_runtime_lookup_window(vm, self, NULL, NULL) ? 1 : 0;
        return true;
    }
    if (is_call(method, sig, "release", "()V") && nargs == 0) {
        (void)surface_runtime_unbind(vm, self);
        return true;
    }
    return false;
}

bool surface_runtime_selftest(struct dvm *vm)
{
    unsigned checks = 0;
    bool okay = true;
    union dvm_value out = {0};
    dvm_ref plain = 0, object = 0, host = 0;
    void *window = NULL;
    uint64_t generation = 0;

    if (!vm) return false;
    sdk_trace_fprintf(stderr, "SURFACE_RUNTIME_SELFTEST begin serial=%llu\n",
                      (unsigned long long)vm->serial);

    /* 1. A plain constructed Surface is a real identity but unbound. */
    plain = dvm_new_object(vm, dvm_find_class_or_external(vm, "android/view/Surface"));
    okay = okay && plain != 0;
    if (plain) {
        okay = okay && surface_runtime_call(vm, "android/view/Surface", "<init>",
                                            "()V", plain, NULL, 0, &out);
        okay = okay && surface_runtime_is_surface(vm, plain);
        okay = okay && !surface_runtime_lookup_window(vm, plain, NULL, NULL);
        out.i = -1;
        okay = okay && surface_runtime_call(vm, "android/view/Surface", "isValid",
                                            "()Z", plain, NULL, 0, &out) && out.i == 0;
        ++checks;
    }

    /* 2. A different class is rejected by identity, not by name luck. */
    object = dvm_new_object(vm, dvm_find_class_or_external(vm, "java/lang/Object"));
    okay = okay && object != 0 && !surface_runtime_is_surface(vm, object);
    ++checks;

    /* Foreign pointers must never become valid Surface bindings. */
    okay = okay && !surface_runtime_bind(vm, plain, (void *)(uintptr_t)0x5245u, 7);
    okay = okay && !surface_runtime_lookup_window(vm, plain, NULL, NULL);
    okay = okay && !surface_runtime_unbind(vm, plain);
    ++checks;

    /* Real-window round trip, duplicate/mismatched generation rejection and
     * exact reference ownership. No fake window in the production registry. */
    window=window_runtime_acquire_current();
    if(window) {
        void *found=NULL;
        okay=okay&&surface_runtime_bind(vm,plain,window,0);
        okay=okay&&surface_runtime_lookup_window(vm,plain,&found,&generation)&&found==window&&generation>0;
        int before=window_runtime_ref_count(window);
        okay=okay&&!surface_runtime_bind(vm,plain,window,0)&&window_runtime_ref_count(window)==before;
        okay=okay&&surface_runtime_unbind(vm,plain)&&window_runtime_ref_count(window)==before-1;
        okay=okay&&!surface_runtime_unbind(vm,plain);
        okay=okay&&!surface_runtime_bind(vm,plain,window,generation+1)&&window_runtime_ref_count(window)==1;
        okay=okay&&surface_runtime_bind(vm,plain,window,0)&&surface_runtime_unbind(vm,plain);
        window_runtime_release(window);window=NULL;
    } else sdk_trace_fprintf(stderr,"SURFACE_RUNTIME_SELFTEST roundtrip=not_run reason=no_host_window\n");
    ++checks;

    /* 5. Trusted host Surface.  If the real host window is available, the
     * lazy bind must hold exactly one window reference that is returned on
     * unbind; otherwise the object stays unbound and invalid. */
    host = surface_runtime_host_surface(vm);
    okay = okay && host != 0 && surface_runtime_is_surface(vm, host);
    if (host && surface_runtime_lookup_window(vm, host, &window, &generation)) {
        int32_t refs = window_runtime_ref_count(window);
        okay = okay && refs > 0;
        okay = okay && surface_runtime_unbind(vm, host);
        okay = okay && window_runtime_ref_count(window) <= 0;
        sdk_trace_fprintf(stderr,
            "SURFACE_RUNTIME_SELFTEST real_window=1 refs=%d generation=%llu\n",
            refs, (unsigned long long)generation);
    } else {
        sdk_trace_fprintf(stderr,
            "SURFACE_RUNTIME_SELFTEST real_window=unavailable (identity only)\n");
    }
    ++checks;

    sdk_trace_fprintf(stderr,
        "SURFACE_RUNTIME_SELFTEST %s checks=%u identity=1 unbound_invalid=1 "
        "registry=1 serial_scope=1\n",
        okay ? "PASS" : "FAIL", checks);
    return okay;
}


int surface_gc_payload(struct dvm *v,struct dvm_object *o,dvm_gc_visit_fn visit,void *u){
 (void)v;(void)visit;(void)u;return o->data_destroy==surface_payload_drop?DVM_GC_PAYLOAD_ATOMIC:DVM_GC_PAYLOAD_UNKNOWN;
}
