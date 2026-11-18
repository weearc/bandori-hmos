#include "native_scratch.h"
/*
 * 自有 JNI 1.6 桥主体。
 *
 * 本文件只实现桥自身；不修改 dvm.c，不接触任何原厂 .so。
 * 关键约定：
 *   - JNIEnv * 指向 per-thread 的 jni_thread_env，其首字段是函数表指针；
 *   - JavaVM 值本身是 JNIInvokeInterface_ 表的地址（标准 JNI C ABI）；
 *   - jobject/jclass 是 64 位 token，不是 dvm_ref；
 *   - 所有进入 DVM 的槽都会在需要时获取 GIL；
 *   - 调原生函数前 dvm_gil_unlock_all，返回后 dvm_gil_relock；
 *   - 未实现槽由 jni_abi_stubs.h 的 typed stub 兜底，设置 pending
 *     UnsatisfiedLinkError，不伪造成功。
 *
 * 说明：本文件包含 dvm/dvm_internal.h 只为读取类/方法/字段结构与少量
 * 内部辅助函数（monitor、精确字段访问）。这是候选构建内部的只读依赖，
 * 不改变 DVM 源文件。
 */

#include "jni_bridge.h"
#include "jni_abi_stubs.h"
#include "native_loader.h"

#include "dvm/dex.h"
#include "dvm/dvm_internal.h"
#include "gc_runtime.h"
#include "gc_operation.h"
#include "network_runtime.h"
#include "perf_attr.h"
#include "leak_scope.h"
/* reflection-runtime-next 提供（构建基线中的 reflection_runtime.h 是旧副本，
 * 这里显式声明以绑定 FromReflected* 的反射句柄还原）。 */
bool reflection_runtime_method_handle(struct dvm *, dvm_ref, struct dvm_method **);
bool reflection_runtime_field_handle(struct dvm *, dvm_ref, struct dvm_field **,
                                     struct dvm_class **, bool *);
bool reflection_runtime_method_object(struct dvm *, struct dvm_class *,
                                      struct dvm_method *, dvm_ref *);

#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <unistd.h>
#include <sys/syscall.h>

#ifdef LUNARIA_UNITY_PLAYER_PROBE
/* Diagnostic only: no new declarations, dispatch, references or path rewrites. */
static void probe_jni_lookup(const char *kind,const char *cls,const char *name,
                             const char *sig,int success,unsigned pending) {
    static atomic_uint count, vm_count, exception_count;
    atomic_uint *budget=!strcmp(kind,"vm")?&vm_count:!strcmp(kind,"exception")?&exception_count:&count;
    unsigned limit=budget==&count?4096:16;
    bool declaration_failure = !success &&
        (!strcmp(kind,"class") || !strcmp(kind,"method") || !strcmp(kind,"static_method") ||
         !strcmp(kind,"field") || !strcmp(kind,"static_field"));
    if(!declaration_failure && atomic_fetch_add(budget,1)>=limit)return;
    int saved=errno;
    FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt","a");
    if(f){fprintf(f,"JNI_LOOKUP kind=%s class=%s name=%s sig=%s success=%d pending=%u\n",
                  kind,cls?cls:"?",name?name:"?",sig?sig:"?",success,pending);fclose(f);}
    errno=saved;
}
#endif

#define JNI_BRIDGE_MAGIC       0x4a4e4942u /* 'JNIB' */
#define JNI_THREAD_MAGIC       0x4a544852u /* 'JTHR' */
#define JNI_VM_MAGIC           0x4a564d48u /* 'JVMH' */
#define JNI_METHOD_ID_MAGIC    0x4a4d4944u /* 'JMID' */
#define JNI_FIELD_ID_MAGIC     0x4a464944u /* 'JFID' */

#define JNI_MAX_BRIDGES        8
#define JNI_METHOD_ID_CAPACITY 4096u
#define JNI_FIELD_ID_CAPACITY  4096u
#define JNI_MAX_THREAD_FRAMES  JNI_HANDLE_MAX_FRAMES
#define JNI_MAX_ARRAY_CACHE    16
#define JNI_NATIVE_MAX_ARGS    16
#define JNI_NATIVE_MAX_WORDS   (JNI_NATIVE_MAX_ARGS + 2)
#define JNI_ERROR_MAX          512
#define JNI_LIMITED_NAME_MAX   80

struct jni_method_id_slot {
    uint32_t magic;
    bool live;
    bool is_static;
    struct dvm_method *method;
    struct dvm_class *decl;
};
typedef struct jni_method_id_slot jni_method_id_slot;

struct jni_field_id_slot {
    uint32_t magic;
    bool live;
    bool is_static;
    struct dvm_field *field;
    struct dvm_class *decl;
};
typedef struct jni_field_id_slot jni_field_id_slot;

struct jni_string_loan {struct jni_string_loan *next;jstring token;bool modified;};
struct jni_utf_cache {
    struct jni_utf_cache *next; /* stable nodes: outstanding native pointers never move */
    bool used;
    bool prepared_only; /* 尚未借给原生调用者的异常编码，可安全淘汰。 */
    struct dvm *vm;
    dvm_gc_handle retention;
    uint64_t identity;
    dvm_ref ref;
    struct jni_string_loan *loans;
    unsigned modified_borrows, utf16_borrows;
    jstring str;
    char *modified;
    size_t modified_len;
    uint16_t *utf16;
    jsize utf16_len;
};

struct jni_array_cache {
    bool used;
    jarray array;
    jni_kind kind;
    void *elems;
    size_t bytes;
    struct dvm *vm;
    dvm_gc_handle retention;
    uint64_t identity;
    dvm_ref ref;
};

struct jni_critical_borrow {
    struct jni_critical_borrow *next;
    jarray array;
    void *pointer;
    dvm_gc_handle retention;
    dvm_ref ref;
    uint64_t identity;
};


struct jni_thread_env {
    const struct JNINativeInterface_ *functions; /* 必须位于 offset 0 */
    struct jni_bridge *bridge;
    uint32_t magic;
    bool daemon;
    uint32_t frames[JNI_MAX_THREAD_FRAMES];
    int frame_depth;
    dvm_ref pending;
    bool pending_pinned;
    struct jni_utf_cache *strings;
    struct jni_array_cache arrays[JNI_MAX_ARRAY_CACHE];
    struct jni_critical_borrow *critical;
    struct jni_thread_env *next;
};

struct jni_vm_handle {
    struct JNIInvokeInterface_ table; /* 必须位于 offset 0 */
    struct jni_bridge *bridge;
    uint32_t magic;
};

struct jni_bridge {
    uint32_t magic;
    bool active;
    bool hooks_installed;
    struct dvm *vm;
    uint8_t vm_tag;
    struct dvm_hooks saved_hooks;
    struct jni_handle_table handles;
    bool gc_strong_registered,gc_weak_registered;
    struct jni_method_id_slot *methods;
    uint32_t method_capacity;
    uint32_t method_used;
    struct jni_field_id_slot *fields;
    uint32_t field_capacity;
    uint32_t field_used;
    struct jni_native_state *natives;
    struct JNINativeInterface_ env_table;
    struct jni_vm_handle vm_handle;
    JavaVM vm_value;
    _Atomic uint32_t next_frame;
    pthread_mutex_t threads_mu;
    struct jni_thread_env *threads;
    uint32_t thread_count;
    _Atomic uint32_t limited_count;
    int limited_slot;
    char limited_name[JNI_LIMITED_NAME_MAX];
    pthread_mutex_t error_mu;
    char last_error[JNI_ERROR_MAX];
};

static struct jni_bridge *g_bridges[JNI_MAX_BRIDGES];
static pthread_mutex_t g_registry_mu = PTHREAD_MUTEX_INITIALIZER;
static _Atomic uint32_t g_vm_tag_counter;

static _Thread_local struct jni_thread_env *g_thread_env;

/* ------------------------------------------------------------------ */
/* 前向声明                                                            */
/* ------------------------------------------------------------------ */

static bool bridge_attach_thread(struct jni_bridge *b, bool daemon,
                                 struct jni_thread_env **out);
static bool bridge_detach_thread(struct jni_bridge *b);
static struct jni_thread_env *thread_env_from_env(JNIEnv *env);
static struct jni_thread_env *thread_ensure(struct jni_bridge *b);
static void memory_sample(struct jni_bridge *b);
static bool enter_slot_named(JNIEnv *env, struct jni_thread_env **out_t,
                       bool *out_owned,const char *caller);
#define enter_slot(e,t,o) enter_slot_named(e,t,o,__func__)
static void leave_slot(struct jni_thread_env *t, bool owned);
static jobject new_local_obj(struct jni_thread_env *t, dvm_ref ref);
static jobject new_local_class(struct jni_thread_env *t, struct dvm_class *cls);
static bool resolve_object_any(struct jni_bridge *b, jobject token,
                               dvm_ref *out_ref);
static bool token_to_dvm_ref(struct jni_thread_env *t, jobject token,
                             dvm_ref *out_ref);
static bool move_dvm_exception_to_pending(struct jni_thread_env *t);
static bool bridge_set_pending_new(struct jni_bridge *b,
                                   const char *class_name,
                                   const char *message);
static void bridge_fill_env_table(struct jni_bridge *b);
static void bridge_fill_vm_table(struct jni_bridge *b);
static bool gc_resolve_token(struct jni_bridge *,jobject,uint32_t *,void **,jni_ref_kind *);

static jint bridge_vm_destroy(JavaVM *vm);
static jint bridge_vm_attach(JavaVM *vm, void **penv, void *args);
static jint bridge_vm_detach(JavaVM *vm);
static jint bridge_vm_get_env(JavaVM *vm, void **penv, jint version);
static jint bridge_vm_attach_daemon(JavaVM *vm, void **penv, void *args);

/* ------------------------------------------------------------------ */
/* 注册表与基础诊断                                                    */
/* ------------------------------------------------------------------ */

static struct jni_bridge *bridge_from_any(void *p)
{
    struct jni_bridge *b = (struct jni_bridge *)p;
    if (!b || b->magic != JNI_BRIDGE_MAGIC || !b->active) return NULL;
    return b;
}

static bool registry_add(struct jni_bridge *b)
{
    bool ok = false;
    pthread_mutex_lock(&g_registry_mu);
    for (int i = 0; i < JNI_MAX_BRIDGES; ++i) {
        if (!g_bridges[i]) {
            g_bridges[i] = b;
            ok = true;
            break;
        }
    }
    pthread_mutex_unlock(&g_registry_mu);
    return ok;
}

static void registry_remove(struct jni_bridge *b)
{
    pthread_mutex_lock(&g_registry_mu);
    for (int i = 0; i < JNI_MAX_BRIDGES; ++i) {
        if (g_bridges[i] == b) g_bridges[i] = NULL;
    }
    pthread_mutex_unlock(&g_registry_mu);
}

struct jni_bridge *jni_bridge_for_vm(struct dvm *vm)
{
    struct jni_bridge *found = NULL;
    pthread_mutex_lock(&g_registry_mu);
    for (int i = 0; i < JNI_MAX_BRIDGES; ++i) {
        struct jni_bridge *b = g_bridges[i];
        if (b && b->vm == vm && b->active) {
            found = b;
            break;
        }
    }
    pthread_mutex_unlock(&g_registry_mu);
    return found;
}

void jni_bridge_set_last_error(struct jni_bridge *b, const char *fmt, ...)
{
    if (!b || !fmt) return;
    va_list ap;
    va_start(ap, fmt);
    pthread_mutex_lock(&b->error_mu);
    vsnprintf(b->last_error, sizeof(b->last_error), fmt, ap);
    b->last_error[sizeof(b->last_error) - 1] = '\0';
    pthread_mutex_unlock(&b->error_mu);
    va_end(ap);
}

const char *jni_bridge_last_error(struct jni_bridge *b)
{
    return b ? b->last_error : "";
}

void jni_bridge_note_limited(struct jni_bridge *b, int slot, const char *name)
{
    if (!b) return;
    if(leak_scope_current)leak_scope_cancel(b->vm,LEAK_UNKNOWN_JNI);
    atomic_fetch_add(&b->limited_count, 1u);
    pthread_mutex_lock(&b->error_mu);
    b->limited_slot = slot;
    snprintf(b->limited_name, sizeof(b->limited_name), "%s",
             name ? name : "?");
    pthread_mutex_unlock(&b->error_mu);
}

uint32_t jni_bridge_limited_count(struct jni_bridge *b)
{
    return b ? atomic_load(&b->limited_count) : 0;
}

/* ------------------------------------------------------------------ */
/* 帧、线程环境与缓存                                                  */
/* ------------------------------------------------------------------ */

static uint32_t next_frame_id(struct jni_bridge *b)
{
    uint32_t id = atomic_fetch_add(&b->next_frame, 1u);
    if (id == 0) id = atomic_fetch_add(&b->next_frame, 1u);
    return id;
}

static uint32_t current_frame_id(struct jni_thread_env *t)
{
    if (!t || t->frame_depth <= 0) return 0;
    return t->frames[t->frame_depth - 1];
}

static dvm_gc_handle jni_gc_retain(struct jni_thread_env *t,dvm_ref ref,unsigned flags)
{
    dvm_gc_handle h=dvm_gc_handle_new(t->bridge->vm,ref,flags);
    if(!h&&!t->pending&&!move_dvm_exception_to_pending(t))
        bridge_set_pending_new(t->bridge,"java/lang/OutOfMemoryError","JNI GC retention failed");
    return h;
}

static jobject new_local_obj(struct jni_thread_env *t, dvm_ref ref)
{
    if (!t) return jni_token_null();
    if (ref == 0) return jni_token_null();
    uint32_t frame = current_frame_id(t);
    if (frame == 0) return jni_token_null();
    jobject token = jni_handle_new_local_in_frame(&t->bridge->handles, ref,
                                                  frame);
    if (jni_token_is_null(token))
        bridge_set_pending_new(t->bridge, "java/lang/OutOfMemoryError",
                               "JNI local reference table exhausted");
    return token;
}

static jobject new_local_class(struct jni_thread_env *t, struct dvm_class *cls)
{
    if (!t || !cls) return jni_token_null();
    uint32_t frame = current_frame_id(t);
    if (frame == 0) return jni_token_null();
    jobject token = jni_handle_new_class_in_frame(&t->bridge->handles, cls,
                                                  frame);
    if (jni_token_is_null(token))
        bridge_set_pending_new(t->bridge, "java/lang/OutOfMemoryError",
                               "JNI class reference table exhausted");
    return token;
}

static void clear_utf_cache(struct jni_utf_cache *c)
{
    if (!c) return;
    if(c->retention)dvm_gc_handle_drop(c->vm,c->retention);
    while(c->loans){struct jni_string_loan *l=c->loans;c->loans=l->next;free(l);}
    free(c->modified);
    free(c->utf16);
    struct jni_utf_cache *next = c->next;
    memset(c, 0, sizeof(*c));
    c->next = next;
}

static void clear_array_cache(struct jni_array_cache *c)
{
    if (!c) return;
    if(c->retention)dvm_gc_handle_drop(c->vm,c->retention);
    free(c->elems);
    memset(c, 0, sizeof(*c));
}

static void free_thread_caches(struct jni_thread_env *t)
{
    if (!t) return;
    while (t->strings) {
        struct jni_utf_cache *c = t->strings;
        t->strings = c->next;
        clear_utf_cache(c);
        free(c);
    }
    for (int i = 0; i < JNI_MAX_ARRAY_CACHE; ++i)
        clear_array_cache(&t->arrays[i]);
    while(t->critical){struct jni_critical_borrow *c=t->critical;t->critical=c->next;
        dvm_gc_contract_note(GC_CONTRACT_LOAN); /* unmatched acquisition at detach */
        (void)dvm_gc_unpin_checked(t->bridge->vm,c->ref);
        dvm_gc_handle_drop(t->bridge->vm,c->retention);free(c);}
}

static void cleanup_thread_locked(struct jni_thread_env *t)
{
    if (!t) return;
    struct jni_bridge *b = t->bridge;
    for (int i = t->frame_depth - 1; i >= 0; --i) {
        if (t->frames[i])
            jni_handle_pop_frame_slots(&b->handles, t->frames[i]);
    }
    t->frame_depth = 0;
    if (t->pending && t->pending_pinned)
        dvm_unpin(b->vm, t->pending);
    t->pending = 0;
    t->pending_pinned = false;
    free_thread_caches(t);
}

static struct jni_utf_cache *utf_cache_find(struct jni_thread_env *t,
                                            jstring str)
{
    if (!t) return NULL;
    dvm_ref ref=0;if(!resolve_object_any(t->bridge,str,&ref)||!ref)return NULL;
    struct dvm_object *o=dvm__obj(t->bridge->vm,ref);if(!o)return NULL;
    for (struct jni_utf_cache *c = t->strings; c; c = c->next) {
        if (c->used && c->ref==ref&&c->identity==o->identity_generation) return c;
    }
    return NULL;
}

static struct jni_utf_cache *utf_cache_create(struct jni_thread_env *t,
                                              jstring str)
{
 /* GC-STEP6: this operation owns all raw diagnostic/fixture borrows. */
 struct gc_operation gc6_operation __attribute__((cleanup(gc_operation_end)));
 if(!gc_operation_begin(t?t->bridge->vm:NULL,&gc6_operation))return NULL;

    dvm_ref ref=0;
    if(!t||!resolve_object_any(t->bridge,str,&ref)||!ref){
        if(t)bridge_set_pending_new(t->bridge,"java/lang/IllegalArgumentException","invalid JNI string identity");
        return NULL;
    }
    if(!gc_operation_admit(t->bridge->vm,ref))return NULL;
    struct jni_utf_cache *found = utf_cache_find(t, str);
    if (found) return found;
    /* Reuse only nodes with no outstanding Get*Chars loans. A fixed number
     * of simultaneous legal JNI loans is not a VM memory exhaustion event. */
    for (struct jni_utf_cache *c = t->strings; c; c = c->next) {
        if (!c->modified_borrows && !c->utf16_borrows) {
            clear_utf_cache(c);
            c->used = true;
            c->str = str;
            c->vm=t->bridge->vm;c->ref=ref;c->identity=dvm__obj(t->bridge->vm,ref)->identity_generation;
            c->retention=jni_gc_retain(t,ref,DVM_GC_HANDLE_STRONG);
            if(!c->retention){clear_utf_cache(c);return NULL;}
            return c;
        }
    }
    struct jni_utf_cache *c = calloc(1, sizeof(*c));
    if (!c) {
        bridge_set_pending_new(t->bridge, "java/lang/OutOfMemoryError",
                               "JNI string borrow cache allocation failed");
        return NULL;
    }
    c->next = t->strings;
    t->strings = c;
    c->used = true;
    c->str = str;
    c->vm=t->bridge->vm;c->ref=ref;c->identity=dvm__obj(t->bridge->vm,ref)->identity_generation;
    c->retention=jni_gc_retain(t,ref,DVM_GC_HANDLE_STRONG);
    if(!c->retention){clear_utf_cache(c);return NULL;}
    return c;
}

static struct jni_array_cache *array_cache_find(struct jni_thread_env *t,
                                                jarray array, const void *elems)
{
    if (!t) return NULL;
    for (int i = 0; i < JNI_MAX_ARRAY_CACHE; ++i) {
        struct jni_array_cache *c = &t->arrays[i];
        if (c->used && c->elems == elems){
            dvm_ref ref=0;
            if(c->array==array || (resolve_object_any(t->bridge,array,&ref)&&ref==c->ref))return c;
        }
    }
    return NULL;
}

static struct jni_array_cache *array_cache_store(struct jni_thread_env *t,
                                                 jarray array, jni_kind kind,
                                                 void *elems, size_t bytes)
{
 /* GC-STEP6: this operation owns all raw diagnostic/fixture borrows. */
 struct gc_operation gc6_operation __attribute__((cleanup(gc_operation_end)));
 if(!gc_operation_begin(t?t->bridge->vm:NULL,&gc6_operation))return NULL;

    if (!t) return NULL;
    dvm_ref ref=0;if(!resolve_object_any(t->bridge,array,&ref)||!ref)return NULL;
    if(!gc_operation_admit(t->bridge->vm,ref))return NULL;
    for (int i = 0; i < JNI_MAX_ARRAY_CACHE; ++i) {
        struct jni_array_cache *c = &t->arrays[i];
        if (!c->used) {
            dvm_gc_handle retention=jni_gc_retain(t,ref,DVM_GC_HANDLE_STRONG);
            if(!retention)return NULL;
            c->used = true;
            c->array = array;
            c->kind = kind;
            c->elems = elems;
            c->bytes = bytes;
            c->vm=t->bridge->vm;c->ref=ref;c->identity=dvm__obj(t->bridge->vm,ref)->identity_generation;c->retention=retention;
            return c;
        }
    }
    bridge_set_pending_new(t->bridge, "java/lang/OutOfMemoryError",
                           "JNI array cache exhausted");
    return NULL;
}

static bool bridge_attach_thread(struct jni_bridge *b, bool daemon,
                                 struct jni_thread_env **out)
{
    if (!b || !b->active) return false;
    struct jni_thread_env *existing = g_thread_env;
    if (existing) {
        if (existing->bridge == b) {
            if (out) *out = existing;
            return true;
        }
        jni_bridge_set_last_error(b,
            "当前线程已 attach 到另一个 JNI 桥，拒绝混用");
        return false;
    }
    struct jni_thread_env *t = calloc(1, sizeof(*t));
    if (!t) return false;
    t->functions = &b->env_table;
    t->bridge = b;
    t->magic = JNI_THREAD_MAGIC;
    t->daemon = daemon;
    uint32_t base = next_frame_id(b);
    if (base == 0) {
        free(t);
        return false;
    }
    t->frames[0] = base;
    t->frame_depth = 1;

    /* Root enumeration owns the GIL. Publish attached-thread roots under the
     * same lock, including AttachCurrentThread from an external native thread. */
    bool publish_owned=!dvm_gil_held(b->vm);
    if(publish_owned)dvm_gil_acquire(b->vm);
    pthread_mutex_lock(&b->threads_mu);
    t->next = b->threads;
    b->threads = t;
    ++b->thread_count;
    pthread_mutex_unlock(&b->threads_mu);
    g_thread_env = t;
    if (out) *out = t;
    if(publish_owned)dvm_gil_release(b->vm);
    return true;
}

static bool bridge_detach_thread(struct jni_bridge *b)
{
    struct jni_thread_env *t = g_thread_env;
    if (!b || !t || t->bridge != b) return false;
    bool owned = false;
    if (!dvm_gil_held(b->vm)) {
        dvm_gil_acquire(b->vm);
        owned = true;
    }
    pthread_mutex_lock(&b->threads_mu);
    struct jni_thread_env **pp = &b->threads;
    while (*pp && *pp != t) pp = &(*pp)->next;
    if (*pp == t) {
        *pp = t->next;
        if (b->thread_count > 0) --b->thread_count;
    }
    pthread_mutex_unlock(&b->threads_mu);
    g_thread_env = NULL;
    cleanup_thread_locked(t);
    t->magic = 0;
    free(t);
    if (owned) dvm_gil_release(b->vm);
    return true;
}

static struct jni_thread_env *thread_env_from_env(JNIEnv *env)
{
    struct jni_thread_env *t = (struct jni_thread_env *)(void *)env;
    if (!t || t->magic != JNI_THREAD_MAGIC) return NULL;
    struct jni_bridge *b = t->bridge;
    if (!b || b->magic != JNI_BRIDGE_MAGIC || !b->active) return NULL;
    return t;
}

static struct jni_thread_env *thread_ensure(struct jni_bridge *b)
{
    if (!b) return NULL;
    struct jni_thread_env *t = g_thread_env;
    if (t && t->bridge == b) return t;
    if (bridge_attach_thread(b, false, &t)) return t;
    return NULL;
}

static bool enter_slot_named(JNIEnv *env, struct jni_thread_env **out_t,
                       bool *out_owned,const char *caller)
{
    struct jni_thread_env *t = thread_env_from_env(env);
    if (!t || !out_t || !out_owned) return false;
    if(leak_scope_current)leak_scope_jni(t->bridge->vm,caller);
    perf_jni_enter(caller);
    bool owned = false;
    if (!dvm_gil_held(t->bridge->vm)) {
        extern void dvm_gil_jni_thread(void);
        dvm_gil_jni_thread();
        uint64_t perf_wait=perf_now();
        dvm_gil_acquire(t->bridge->vm);
        perf_gil_wait_done(perf_wait);
        owned = true;
    }
    *out_t = t;
    *out_owned = owned;
    memory_sample(t->bridge);
    return true;
}

/* Observe only while holding the GIL and a live bridge. No guest calls, roots,
 * allocation, collection or budget changes. T0 is the first JNI entry. */
static void memory_sample(struct jni_bridge *b){
 /* Full-heap diagnostics call dvm__obj for every object inside a JNI native
  * scope. This registers the whole heap as temporary borrows and makes the
  * linear duplicate lookup quadratic. Use bounded counters by default. The
  * old diagnostic remains an explicit, runtime-reversible opt-in. */
 static uint64_t control_poll,lite_last;static int full_heap,controls_created;
 uint64_t poll_now=perf_now();
 if(!controls_created){
  controls_created=1;FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/jni-heap-diagnostics","wx");
  if(f){fputs("0\n",f);fclose(f);}
 }
 if(poll_now-control_poll>=1000000000ull){
  control_poll=poll_now;FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/jni-heap-diagnostics","r");int n=0;if(f){if(fscanf(f,"%d",&n)!=1)n=0;fclose(f);}full_heap=n==1;
 }
 if(!full_heap){
  if(poll_now-lite_last>=10000000000ull){
   lite_last=poll_now;extern size_t vm_arena_used(void),vm_arena_denied(void);
   FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/jni-memory-lite.csv","a");
   if(f){fprintf(f,"pid=%d mono_ns=%llu arena_used=%zu arena_denied=%zu jni_refs=%u full_heap_scan=0\n",getpid(),(unsigned long long)poll_now,vm_arena_used(),vm_arena_denied(),b->handles.live_count);fclose(f);}
  }
  return;
 }
 /* A full heap diagnostic at JNI entry can take longer than its sampling
  * period and repeatedly hold the GIL throughout the first nativeRender.
  * Keep the diagnostic available between frames; never scan inside one. */
 if(perf_frame_in_progress())return;
 static struct jni_bridge *owner;static uint8_t owner_tag;static uint64_t start,last;static unsigned sample;
 struct timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);uint64_t now=(uint64_t)ts.tv_sec*1000+ts.tv_nsec/1000000;
 if(owner!=b||owner_tag!=b->handles.vm_tag){owner=b;owner_tag=b->handles.vm_tag;start=now;last=0;sample=0;}
 if(last&&now-last<10000)return;last=now;
 int saved_errno=errno;struct dvm *v=b->vm;
 dvm_gc_observe(v,"/data/storage/el2/base/haps/entry/files/sdkdex/gc.csv","periodic_storage_snapshot");
 extern size_t vm_arena_used(void),vm_arena_peak(void),vm_arena_denied(void),vm_arena_limit(void),collections_storage_bytes(struct dvm *);
 uint32_t objects=0,pins=0,kinds[5]={0},messages=0,vsync=0,methods=0,arrays=0,longs=0,iterators=0;
 struct {struct dvm_class *cls;uint32_t count;} top[2048]={0};unsigned classes=0,overflow=0;
 for(dvm_ref r=1;r<=v->heap_size;r++){struct dvm_object *o=dvm__obj(v,r);if(!o)continue;objects++;pins+=o->pins;const char *name=o->cls?o->cls->name:"";messages+=!strcmp(name,"android/os/Message");vsync+=!strcmp(name,"sdktrace/NativeVSyncDispatch");methods+=!strcmp(name,"java/lang/reflect/Method");arrays+=!strcmp(name,"[Ljava/lang/Object;");longs+=!strcmp(name,"java/lang/Long");iterators+=strstr(name,"Iterator")!=NULL||strstr(name,"$Itr")!=NULL;unsigned j=0;for(;j<classes&&top[j].cls!=o->cls;j++);if(j==classes){if(classes==2048){overflow++;continue;}top[classes++].cls=o->cls;}top[j].count++;}
 for(uint32_t i=0;i<b->handles.capacity;i++){jni_handle_slot *s=&b->handles.slots[i];if(s->live&&s->kind<5)kinds[s->kind]++;}
 size_t rss=0;FILE *stat=fopen("/proc/self/status","r");if(stat){char line[160];while(fgets(line,sizeof line,stat)){size_t kb;if(sscanf(line,"VmRSS: %zu kB",&kb)==1)rss=kb*1024;}fclose(stat);}
 const char *path="/data/storage/el2/base/haps/entry/files/sdkdex/vm-memory.csv";
 FILE *f=fopen(path,sample?"a":"w");
 if(f){if(!sample)fprintf(f,"pid,epoch,elapsed_ms,objects,heap_high_water,arena_used,arena_peak,arena_limit,arena_denied,jni_refs,jni_local,jni_global,jni_weak,jni_class,object_pins,collection_storage_bytes,rss_bytes,messages,vsync_dispatch,reflection_methods,object_arrays,long_boxes,iterators\n");fprintf(f,"%d,%lld,%llu,%u,%u,%zu,%zu,%zu,%zu,%u,%u,%u,%u,%u,%u,%zu,%zu,%u,%u,%u,%u,%u,%u\n",getpid(),(long long)time(NULL),(unsigned long long)(now-start),objects,v->heap_size,vm_arena_used(),vm_arena_peak(),vm_arena_limit(),vm_arena_denied(),b->handles.live_count,kinds[1],kinds[2],kinds[3],kinds[4],pins,collections_storage_bytes(v),rss,messages,vsync,methods,arrays,longs,iterators);fclose(f);}
 f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt","a");if(f){fprintf(f,"ENGINE_LOG_VM_MEMORY pid=%d elapsed_ms=%llu objects=%u arena=%zu denied=%zu jni_refs=%u collection_core_bytes=%zu classes_overflow=%u gc_count=0\n",getpid(),(unsigned long long)(now-start),objects,vm_arena_used(),vm_arena_denied(),b->handles.live_count,collections_storage_bytes(v),overflow);for(unsigned k=0;k<10&&k<classes;k++){unsigned best=k;for(unsigned j=k+1;j<classes;j++)if(top[j].count>top[best].count)best=j;if(best!=k){struct dvm_class *c=top[k].cls;uint32_t n=top[k].count;top[k]=top[best];top[best].cls=c;top[best].count=n;}fprintf(f,"ENGINE_LOG_VM_MEMORY_TOP pid=%d elapsed_ms=%llu rank=%u class=%s count=%u\n",getpid(),(unsigned long long)(now-start),k+1,top[k].cls?top[k].cls->name:"?",top[k].count);}fclose(f);}
 sample++;errno=saved_errno;
}

static void leave_slot(struct jni_thread_env *t, bool owned)
{
    if (owned && t && t->bridge) dvm_gil_release(t->bridge->vm);
    perf_jni_leave();
}

static void probe_array_event(const char *operation, char kind, jobject token,
                              const void *data, size_t length,
                              unsigned pending, int frame_depth)
{
#ifdef LUNARIA_UNITY_PLAYER_PROBE
    static atomic_uint count;
    if (kind != 'B' || atomic_fetch_add(&count, 1) >= 256) return;
    int saved_errno = errno;
    FILE *file = fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt", "a");
    if (file) {
        fprintf(file, "JNI_BYTE_ARRAY op=%s token=%p data=%p length=%zu pending=%u frames=%d thread=%llu\n",
                operation, (void *)token, data, length, pending, frame_depth,
                (unsigned long long)(uintptr_t)pthread_self());
        fclose(file);
    }
    errno = saved_errno;
#else
    (void)operation; (void)kind; (void)token; (void)data;
    (void)length; (void)pending; (void)frame_depth;
#endif
}

/* ------------------------------------------------------------------ */
/* 对象、类与异常辅助                                                  */
/* ------------------------------------------------------------------ */

/* jni_handles validates guest identity at every object resolution, including
 * internal promotion and weak/global aliases. */
static bool gc_resolve_token(struct jni_bridge *b,jobject token,uint32_t *ref,void **cls,jni_ref_kind *kind)
{
    return b&&jni_handle_resolve_any(&b->handles,token,ref,cls,kind);
}

static bool jni_gc_cleared_weak(struct jni_bridge *b,jobject token)
{
    uint32_t ref=0;void *cls=NULL;jni_ref_kind kind=JNI_REF_NONE;
    return !jni_token_is_null(token)&&gc_resolve_token(b,token,&ref,&cls,&kind)&&
           kind==JNI_REF_WEAK&&!ref&&!cls;
}

static bool jni_gc_strong_roots(struct dvm *vm,dvm_gc_visit_fn visit,void *user,void *context)
{
    struct jni_bridge *b=context;if(!b||b->vm!=vm||!visit)return false;
    for(uint32_t i=0;i<b->handles.capacity;i++){
        jni_handle_slot *s=&b->handles.slots[i];
        if(!s->live||s->kind==JNI_REF_WEAK||!s->object_ref)continue;
        struct dvm_object *o=dvm__obj(vm,s->object_ref);
        if(!o||s->object_identity!=o->identity_generation)return false;
        visit(&s->object_ref,user);
    }
    /* All thread mutations and visitor calls are serialized by the VM GIL.
     * No threads_mu is held across visitor callbacks. */
    for(struct jni_thread_env *t=b->threads;t;t=t->next)if(t->pending)visit(&t->pending,user);
    return true;
}

static bool jni_gc_weak_roots(struct dvm *vm,dvm_gc_visit_fn visit,void *user,void *context)
{
    struct jni_bridge *b=context;if(!b||b->vm!=vm||!visit)return false;
    for(uint32_t i=0;i<b->handles.capacity;i++){
        jni_handle_slot *s=&b->handles.slots[i];if(!s->live||s->kind!=JNI_REF_WEAK||!s->object_ref)continue;
        struct dvm_object *o=dvm__obj(vm,s->object_ref);
        if(!o||s->object_identity!=o->identity_generation){s->object_ref=0;continue;}
        visit(&s->object_ref,user);
    }
    return true;
}

static bool resolve_object_any(struct jni_bridge *b, jobject token,
                               dvm_ref *out_ref)
{
    if (!b) return false;
    if (jni_token_is_null(token)) {
        if (out_ref) *out_ref = 0;
        return true;
    }
    uint32_t ref = 0;
    void *cls = NULL;
    jni_ref_kind kind = JNI_REF_NONE;
    if (!gc_resolve_token(b, token, &ref, &cls, &kind))
        return false;
    if (cls) return false;
    if (out_ref) *out_ref = ref;
    return true;
}

static bool resolve_class_token(struct jni_bridge *b, jclass token,
                                struct dvm_class **out_cls)
{
    if (!b) return false;
    if (jni_token_is_null(token)) return false;
    uint32_t ref = 0;
    void *cls = NULL;
    jni_ref_kind kind = JNI_REF_NONE;
    if (!gc_resolve_token(b, token, &ref, &cls, &kind))
        return false;
    if (cls) {
        if (out_cls) *out_cls = (struct dvm_class *)cls;
        return true;
    }
    if (ref == 0) return false;
    struct dvm_object *o = dvm__obj(b->vm, ref);
    if (!o || o->kind != DVM_OBJ_CLASS || !o->klass) return false;
    if (out_cls) *out_cls = o->klass;
    return true;
}

bool jni_bridge_resolve_object(struct jni_bridge *b, jobject token,
                               jni_ref_kind accept, dvm_ref *out_ref)
{
    if (!b) return false;
    if (jni_token_is_null(token)) {
        if (out_ref) *out_ref = 0;
        return true;
    }
    uint32_t ref = 0;
    void *cls = NULL;
    jni_ref_kind actual = JNI_REF_NONE;
    if (!gc_resolve_token(b, token, &ref, &cls, &actual))
        return false;
    if (cls) return false;
    if (accept != JNI_REF_NONE && actual != accept) return false;
    if (out_ref) *out_ref = ref;
    return true;
}

bool jni_bridge_resolve_class(struct jni_bridge *b, jclass cls,
                              struct dvm_class **out_cls)
{
    return resolve_class_token(b, cls, out_cls);
}

/* ------------------------------------------------------------------ */
/* 受控 env 入口                                                       */
/* ------------------------------------------------------------------ */

struct jni_bridge *jni_bridge_for_env(JNIEnv *env)
{
    struct jni_thread_env *t;
    if (!env) return NULL;
    t = g_thread_env;
    /* Address comparison only: a foreign or arbitrary pointer must never be
     * dereferenced to probe its magic. */
    if (!t || (void *)env != (void *)t) return NULL;
    if (!t->bridge || t->bridge->magic != JNI_BRIDGE_MAGIC || !t->bridge->active)
        return NULL;
    return t->bridge;
}

bool jni_bridge_resolve_object_for_env(JNIEnv *env, jobject token,
                                       jni_ref_kind accept, dvm_ref *out_ref,
                                       struct jni_bridge **out_bridge)
{
    struct jni_bridge *b = jni_bridge_for_env(env);
    if (!b) return false;
    if (!jni_bridge_resolve_object(b, token, accept, out_ref)) return false;
    if (out_bridge) *out_bridge = b;
    return true;
}

jobject jni_bridge_new_local_object_for_env(JNIEnv *env, dvm_ref ref)
{
    struct jni_thread_env *t;
    if (!env || !ref) return jni_token_null();
    t = g_thread_env;
    if (!t || (void *)env != (void *)t) return jni_token_null();
    if (!t->bridge || t->bridge->magic != JNI_BRIDGE_MAGIC || !t->bridge->active)
        return jni_token_null();
    if (!dvm__obj(t->bridge->vm, ref)) return jni_token_null();
    return new_local_obj(t, ref);
}

bool jni_bridge_class_name(struct jni_bridge *b, jclass cls, char *out,
                           size_t out_size)
{
    struct dvm_class *c = NULL;
    if (!out || out_size == 0) return false;
    out[0] = '\0';
    if (!resolve_class_token(b, cls, &c) || !c) return false;
    const char *name = dvm_class_name(c);
    if (!name) return false;
    snprintf(out, out_size, "%s", name);
    return true;
}

static bool token_to_dvm_ref(struct jni_thread_env *t, jobject token,
                             dvm_ref *out_ref)
{
    if (!t || !out_ref) return false;
    if (jni_token_is_null(token)) {
        *out_ref = 0;
        return true;
    }
    uint32_t ref = 0;
    void *cls = NULL;
    jni_ref_kind kind = JNI_REF_NONE;
    if (!jni_handle_resolve_any(&t->bridge->handles, token, &ref, &cls, &kind))
        return false;
    if (cls) {
        *out_ref = dvm_class_object(t->bridge->vm, (struct dvm_class *)cls);
        return *out_ref != 0;
    }
    *out_ref = ref;
    return true;
}

static bool move_dvm_exception_to_pending(struct jni_thread_env *t)
{
    if (!t) return false;
    struct dvm *vm = t->bridge->vm;
    dvm_ref e = dvm_exception(vm);
    if (!e) return false;
    dvm_clear_exception(vm);
    if (t->pending && t->pending_pinned) dvm_unpin(vm, t->pending);
    t->pending = e;
    dvm_pin(vm, e);
    t->pending_pinned = true;
    return true;
}

static bool bridge_set_pending_new(struct jni_bridge *b,
                                   const char *class_name,
                                   const char *message)
{
    if (!b) return false;
    bool owned = false;
    if (!dvm_gil_held(b->vm)) {
        dvm_gil_acquire(b->vm);
        owned = true;
    }
    struct jni_thread_env *t = thread_ensure(b);
    bool ok = false;
    if (t) {
        dvm__throw(b->vm, class_name ? class_name : "java/lang/RuntimeException",
                   "%s", message ? message : (class_name ? class_name : ""));
        dvm_ref e = dvm_exception(b->vm);
        if (e) {
            dvm_clear_exception(b->vm);
            if (t->pending && t->pending_pinned) dvm_unpin(b->vm, t->pending);
            t->pending = e;
            dvm_pin(b->vm, e);
            t->pending_pinned = true;
            ok = true;
        }
    }
    if (owned) dvm_gil_release(b->vm);
    return ok;
}

bool jni_bridge_pending(struct jni_bridge *b)
{
    if (!b) return false;
    struct jni_thread_env *t = thread_ensure(b);
    return t && t->pending != 0;
}

bool jni_bridge_throw_new(struct jni_bridge *b, const char *class_name,
                          const char *message)
{
    return bridge_set_pending_new(b, class_name, message);
}

static jvalue zero_value(void)
{
    jvalue v;
    memset(&v, 0, sizeof(v));
    return v;
}

/* ------------------------------------------------------------------ */
/* UTF-8 / modified UTF-8 / UTF-16 转换                                */
/* ------------------------------------------------------------------ */

static bool decode_utf8_one(const unsigned char *p, size_t len, size_t *used,
                            uint32_t *cp)
{
    if (!p || len == 0 || !used || !cp) return false;
    unsigned char c = p[0];
    if (c < 0x80) {
        *cp = c;
        *used = 1;
        return true;
    }
    if (c >= 0xc2 && c <= 0xdf && len >= 2 &&
        (p[1] & 0xc0) == 0x80) {
        *cp = ((uint32_t)(c & 0x1f) << 6) | (uint32_t)(p[1] & 0x3f);
        *used = 2;
        return true;
    }
    if (c >= 0xe0 && c <= 0xef && len >= 3 &&
        (p[1] & 0xc0) == 0x80 && (p[2] & 0xc0) == 0x80) {
        uint32_t v = ((uint32_t)(c & 0x0f) << 12) |
                     ((uint32_t)(p[1] & 0x3f) << 6) |
                     (uint32_t)(p[2] & 0x3f);
        if (v < 0x800 || (v >= 0xd800 && v <= 0xdfff)) {
            *cp = 0xfffd;
            *used = 1;
            return false;
        }
        *cp = v;
        *used = 3;
        return true;
    }
    if (c >= 0xf0 && c <= 0xf4 && len >= 4 &&
        (p[1] & 0xc0) == 0x80 && (p[2] & 0xc0) == 0x80 &&
        (p[3] & 0xc0) == 0x80) {
        uint32_t v = ((uint32_t)(c & 0x07) << 18) |
                     ((uint32_t)(p[1] & 0x3f) << 12) |
                     ((uint32_t)(p[2] & 0x3f) << 6) |
                     (uint32_t)(p[3] & 0x3f);
        if (v < 0x10000 || v > 0x10ffff) {
            *cp = 0xfffd;
            *used = 1;
            return false;
        }
        *cp = v;
        *used = 4;
        return true;
    }
    *cp = 0xfffd;
    *used = 1;
    return false;
}

static bool append_utf8_cp(char *out, size_t out_size, size_t *pos,
                           uint32_t cp)
{
    if (!out || !pos || *pos > out_size) return false;
    unsigned char tmp[4];
    size_t n = 0;
    if (cp <= 0x7f) {
        tmp[n++] = (unsigned char)cp;
    } else if (cp <= 0x7ff) {
        tmp[n++] = (unsigned char)(0xc0 | (cp >> 6));
        tmp[n++] = (unsigned char)(0x80 | (cp & 0x3f));
    } else if (cp <= 0xffff) {
        tmp[n++] = (unsigned char)(0xe0 | (cp >> 12));
        tmp[n++] = (unsigned char)(0x80 | ((cp >> 6) & 0x3f));
        tmp[n++] = (unsigned char)(0x80 | (cp & 0x3f));
    } else {
        tmp[n++] = (unsigned char)(0xf0 | (cp >> 18));
        tmp[n++] = (unsigned char)(0x80 | ((cp >> 12) & 0x3f));
        tmp[n++] = (unsigned char)(0x80 | ((cp >> 6) & 0x3f));
        tmp[n++] = (unsigned char)(0x80 | (cp & 0x3f));
    }
    if (*pos + n + 1 > out_size) return false;
    memcpy(out + *pos, tmp, n);
    *pos += n;
    out[*pos] = '\0';
    return true;
}

static uint32_t utf16_units_from_utf8(const char *utf8, size_t len)
{
    uint32_t units = 0;
    size_t i = 0;
    while (i < len) {
        size_t used = 0;
        uint32_t cp = 0;
        (void)decode_utf8_one((const unsigned char *)utf8 + i, len - i,
                              &used, &cp);
        if (used == 0) used = 1;
        i += used;
        if (cp > 0xffff) units += 2;
        else ++units;
    }
    return units;
}

static uint16_t *utf8_to_utf16_alloc(const char *utf8, size_t len,
                                     jsize *out_len)
{
    uint32_t units = utf16_units_from_utf8(utf8, len);
    uint16_t *out = calloc((size_t)units + 1u, sizeof(uint16_t));
    if (!out) return NULL;
    size_t i = 0;
    uint32_t n = 0;
    while (i < len) {
        size_t used = 0;
        uint32_t cp = 0;
        (void)decode_utf8_one((const unsigned char *)utf8 + i, len - i,
                              &used, &cp);
        if (used == 0) used = 1;
        i += used;
        if (cp > 0x10ffff) cp = 0xfffd;
        if (cp > 0xffff) {
            cp -= 0x10000;
            out[n++] = (uint16_t)(0xd800 | (cp >> 10));
            out[n++] = (uint16_t)(0xdc00 | (cp & 0x3ff));
        } else {
            out[n++] = (uint16_t)cp;
        }
    }
    if (out_len) *out_len = (jsize)n;
    return out;
}

static char *utf16_to_utf8_alloc(const uint16_t *units, size_t n,
                                 size_t *out_len, uint32_t *out_units)
{
    if (n > UINT32_MAX / 4u) return NULL;
    char *out = native_scratch_acquire(n * 4u + 4u);
    if (!out) return NULL;
    size_t pos = 0;
    size_t i = 0;
    uint32_t produced = 0;
    while (i < n) {
        uint32_t cp = units[i++];
        if (cp >= 0xd800 && cp <= 0xdbff && i < n &&
            units[i] >= 0xdc00 && units[i] <= 0xdfff) {
            uint32_t lo = units[i++];
            cp = 0x10000u + ((cp - 0xd800u) << 10) + (lo - 0xdc00u);
        }
        if (!append_utf8_cp(out, n * 4u + 4u, &pos, cp)) {
            native_scratch_release(out);
            return NULL;
        }
        ++produced;
    }
    if (out_len) *out_len = pos;
    if (out_units) *out_units = produced;
    return out;
}

static char *utf8_to_modified_alloc(const char *utf8, size_t len,
                                    size_t *out_len)
{
    if (!utf8 && len) return NULL;
    if (len > (SIZE_MAX - 16u) / 3u) return NULL;
    char *out = calloc(len * 3u + 16u, 1);
    if (!out) return NULL;
    size_t pos = 0;
    size_t i = 0;
    while (i < len) {
        size_t used = 0;
        uint32_t cp = 0;
        (void)decode_utf8_one((const unsigned char *)utf8 + i, len - i,
                              &used, &cp);
        if (used == 0) used = 1;
        i += used;
        if (cp == 0) {
            if (pos + 3u > len * 3u + 16u) {
                free(out);
                return NULL;
            }
            out[pos++] = (char)0xc0;
            out[pos++] = (char)0x80;
        } else if (cp > 0xffff) {
            uint32_t v = cp - 0x10000u;
            uint32_t hi = 0xd800u | (v >> 10);
            uint32_t lo = 0xdc00u | (v & 0x3ffu);
            if (!append_utf8_cp(out, len * 3u + 16u, &pos, hi) ||
                !append_utf8_cp(out, len * 3u + 16u, &pos, lo)) {
                free(out);
                return NULL;
            }
        } else if (!append_utf8_cp(out, len * 3u + 16u, &pos, cp)) {
            free(out);
            return NULL;
        }
    }
    out[pos] = '\0';
    if (out_len) *out_len = pos;
    return out;
}

static char *modified_to_utf8_alloc(const char *in, size_t len,
                                    size_t *out_len)
{
    if (!in && len) return NULL;
    char *out = native_scratch_acquire(len + 8u);
    if (!out) return NULL;
    size_t pos = 0;
    size_t i = 0;
    while (i < len) {
        unsigned char c = (unsigned char)in[i];
        uint32_t cp = 0;
        size_t used = 1;
        if (c == 0xc0 && i + 1 < len && (unsigned char)in[i + 1] == 0x80) {
            cp = 0;
            used = 2;
        } else {
            size_t u = 0;
            (void)decode_utf8_one((const unsigned char *)in + i, len - i,
                                  &u, &cp);
            used = u ? u : 1;
        }
        i += used;
        if (cp >= 0xd800 && cp <= 0xdbff && i < len) {
            size_t u = 0;
            uint32_t lo = 0;
            unsigned char c2 = (unsigned char)in[i];
            if (c2 >= 0xe0 && c2 <= 0xef &&
                decode_utf8_one((const unsigned char *)in + i, len - i,
                                &u, &lo) &&
                lo >= 0xdc00 && lo <= 0xdfff) {
                cp = 0x10000u + ((cp - 0xd800u) << 10) + (lo - 0xdc00u);
                i += u;
            }
        }
        if (!append_utf8_cp(out, len + 8u, &pos, cp)) {
            native_scratch_release(out);
            return NULL;
        }
    }
    out[pos] = '\0';
    if (out_len) *out_len = pos;
    return out;
}

static char *utf16_to_modified_alloc(const uint16_t *units, size_t n,
                                     size_t *out_len)
{
    if (!units && n) return NULL;
    if (n > (SIZE_MAX - 16u) / 3u) return NULL;
    char *out = native_scratch_acquire(n * 3u + 16u);
    if (!out) return NULL;
    size_t pos = 0;
    for (size_t i = 0; i < n; ++i) {
        uint32_t cp = units[i];
        if (cp == 0) {
            out[pos++] = (char)0xc0;
            out[pos++] = (char)0x80;
        } else if (!append_utf8_cp(out, n * 3u + 16u, &pos, cp)) {
            native_scratch_release(out);
            return NULL;
        }
    }
    out[pos] = '\0';
    if (out_len) *out_len = pos;
    return out;
}

static bool string_bytes(struct jni_bridge *b, jobject str,
                         const char **out_bytes, size_t *out_len)
{
    dvm_ref ref = 0;
    if (!resolve_object_any(b, str, &ref) || ref == 0) return false;
    struct dvm_object *o = dvm__obj(b->vm, ref);
    if (!o || o->kind != DVM_OBJ_STRING) return false;
    const char *bytes = dvm_string_utf8(b->vm, ref);
    if (!bytes) return false;
    if (out_bytes) *out_bytes = bytes;
    if (out_len) *out_len = dvm_string_utf8_length(b->vm, ref);
    return true;
}

static struct jni_utf_cache *string_modified_cache(struct jni_thread_env *t,
                                                   jstring str)
{
    if (!t) return NULL;
    struct jni_utf_cache *c = utf_cache_create(t, str);
    if (!c) return NULL;
    if (c->modified) return c;
    const char *bytes = NULL;
    size_t len = 0;
    if (!string_bytes(t->bridge, str, &bytes, &len)) {
        bridge_set_pending_new(t->bridge, "java/lang/NullPointerException",
                               "invalid jstring");
        return NULL;
    }
    c->modified = utf8_to_modified_alloc(bytes, len, &c->modified_len);
    if (!c->modified) {
        bridge_set_pending_new(t->bridge, "java/lang/OutOfMemoryError",
                               "GetStringUTFChars allocation failed");
        return NULL;
    }
    return c;
}

static struct jni_utf_cache *string_utf16_cache(struct jni_thread_env *t,
                                                jstring str)
{
    if (!t) return NULL;
    struct jni_utf_cache *c = utf_cache_create(t, str);
    if (!c) return NULL;
    if (c->utf16) return c;
    const char *bytes = NULL;
    size_t len = 0;
    if (!string_bytes(t->bridge, str, &bytes, &len)) {
        bridge_set_pending_new(t->bridge, "java/lang/NullPointerException",
                               "invalid jstring");
        return NULL;
    }
    c->utf16 = utf8_to_utf16_alloc(bytes, len, &c->utf16_len);
    if (!c->utf16) {
        bridge_set_pending_new(t->bridge, "java/lang/OutOfMemoryError",
                               "GetStringChars allocation failed");
        return NULL;
    }
    return c;
}

/* ------------------------------------------------------------------ */
/* 数组辅助                                                            */
/* ------------------------------------------------------------------ */

static bool array_info(struct jni_bridge *b, jarray array, char want_kind,
                       void **out_data, uint32_t *out_len)
{
    dvm_ref ref = 0;
    if (!resolve_object_any(b, array, &ref) || ref == 0) return false;
    struct dvm_object *o = dvm__obj(b->vm, ref);
    if (!o || o->kind != DVM_OBJ_ARRAY) return false;
    if (want_kind != 0 && o->elem_kind != want_kind) return false;
    if (out_data) *out_data = dvm_array_data(b->vm, ref);
    if (out_len) *out_len = dvm_array_length(b->vm, ref);
    return true;
}

static jarray new_prim_array(struct jni_thread_env *t, jsize len, char kind,
                             const char *elem_desc)
{
    if (!t) return jni_token_null();
    if (len < 0) {
        bridge_set_pending_new(t->bridge, "java/lang/NegativeArraySizeException",
                               "negative array length");
        return jni_token_null();
    }
    /* The slot table supplies the full array descriptor; the DVM accepts
     * the component descriptor and adds its own leading '['. */
    dvm_ref ref = dvm_new_array(t->bridge->vm, kind, elem_desc + 1,
                                (uint32_t)len);
    if (ref == 0) {
        if (!move_dvm_exception_to_pending(t))
            bridge_set_pending_new(t->bridge, "java/lang/OutOfMemoryError",
                                   "array allocation failed");
        return jni_token_null();
    }
    return new_local_obj(t, ref);
}

static jarray new_object_array(struct jni_thread_env *t, jsize len,
                               jclass clazz, jobject init)
{
    if (!t) return jni_token_null();
    if (len < 0) {
        bridge_set_pending_new(t->bridge, "java/lang/NegativeArraySizeException",
                               "negative array length");
        return jni_token_null();
    }
    struct dvm_class *elem = NULL;
    if (!resolve_class_token(t->bridge, clazz, &elem) || !elem) {
        bridge_set_pending_new(t->bridge, "java/lang/NullPointerException",
                               "NewObjectArray element class is null");
        return jni_token_null();
    }
    dvm_ref init_ref = 0;
    if (!token_to_dvm_ref(t, init, &init_ref)) {
        bridge_set_pending_new(t->bridge, "java/lang/IllegalArgumentException",
                               "NewObjectArray initial element is invalid");
        return jni_token_null();
    }
    dvm_ref ref = dvm_new_array(t->bridge->vm, 'L', elem->desc, (uint32_t)len);
    if (ref == 0) {
        if (!move_dvm_exception_to_pending(t))
            bridge_set_pending_new(t->bridge, "java/lang/OutOfMemoryError",
                                   "object array allocation failed");
        return jni_token_null();
    }
    dvm_ref *data = (dvm_ref *)dvm_array_data(t->bridge->vm, ref);
    if (data && len > 0) {
        for (jsize i = 0; i < len; ++i)
            GC_STORE_REF(t->bridge->vm,&data[i],init_ref,"JNI_NewObjectArray");
    }
    return new_local_obj(t, ref);
}

/* ------------------------------------------------------------------ */
/* 函数表填充                                                          */
/* ------------------------------------------------------------------ */

static void bridge_fill_vm_table(struct jni_bridge *b)
{
    memset(&b->vm_handle, 0, sizeof(b->vm_handle));
    b->vm_handle.table.DestroyJavaVM = bridge_vm_destroy;
    b->vm_handle.table.AttachCurrentThread = bridge_vm_attach;
    b->vm_handle.table.DetachCurrentThread = bridge_vm_detach;
    b->vm_handle.table.GetEnv = bridge_vm_get_env;
    b->vm_handle.table.AttachCurrentThreadAsDaemon = bridge_vm_attach_daemon;
    b->vm_handle.bridge = b;
    b->vm_handle.magic = JNI_VM_MAGIC;
    b->vm_value = (JavaVM)(void *)&b->vm_handle.table;
}

static struct jni_bridge *bridge_from_vm(JavaVM *vm)
{
    if (!vm) return NULL;
    /* C JNI 语义：JavaVM *vm 指向一个 JavaVM 值（即指向 invoke 表）。
     * C++ 客户端经 JavaVM_::GetEnv(this) 传入的 this 也是同一块首字段为
     * functions 指针的存储；两种 ABI 下读第一个指针都得到 invoke 表地址。 */
    JavaVM value = *vm;
    if (!value) return NULL;
    struct jni_vm_handle *h = (struct jni_vm_handle *)(void *)value;
    if (h->magic != JNI_VM_MAGIC) return NULL;
    return bridge_from_any(h->bridge);
}

static jint bridge_vm_destroy(JavaVM *vm)
{
    struct jni_bridge *b = bridge_from_vm(vm);
    extern void dvm_lifetime_note(struct dvm *,const char *) __attribute__((weak));
    if(dvm_lifetime_note)dvm_lifetime_note(b?b->vm:NULL,"JNI.DestroyJavaVM");
    if (b) jni_bridge_set_last_error(b,
        "DestroyJavaVM 未实现：DVM 生命周期由宿主负责");
    return JNI_ERR;
}

static jint bridge_vm_attach_common(JavaVM *vm, void **penv, void *args,
                                    bool daemon)
{
    struct jni_bridge *b = bridge_from_vm(vm);
    (void)args;
    if (!b) return JNI_ERR;
    struct jni_thread_env *t = NULL;
    if (!bridge_attach_thread(b, daemon, &t)) return JNI_ERR;
    if (penv) *penv = (void *)t;
    return JNI_OK;
}

static jint bridge_vm_attach(JavaVM *vm, void **penv, void *args)
{
    return bridge_vm_attach_common(vm, penv, args, false);
}

static jint bridge_vm_attach_daemon(JavaVM *vm, void **penv, void *args)
{
    return bridge_vm_attach_common(vm, penv, args, true);
}

static jint bridge_vm_detach(JavaVM *vm)
{
    struct jni_bridge *b = bridge_from_vm(vm);
    if (!b) return JNI_ERR;
    return bridge_detach_thread(b) ? JNI_OK : JNI_ERR;
}

static jint bridge_vm_get_env(JavaVM *vm, void **penv, jint version)
{
    struct jni_bridge *b = bridge_from_vm(vm);
    if (!b) {
#ifdef LUNARIA_UNITY_PLAYER_PROBE
        probe_jni_lookup("vm_failure","JavaVM","GetEnv","invalid_bridge",0,0);
#endif
        return JNI_ERR;
    }
    if (version > JNI_VERSION_1_6) {
#ifdef LUNARIA_UNITY_PLAYER_PROBE
        probe_jni_lookup("vm_failure","JavaVM","GetEnv","unsupported_version",0,0);
#endif
        return JNI_EVERSION;
    }
    struct jni_thread_env *t = g_thread_env;
    if (!t || t->bridge != b) {
#ifdef LUNARIA_UNITY_PLAYER_PROBE
        probe_jni_lookup("vm_failure","JavaVM","GetEnv",t?"bridge_mismatch":"detached",0,t?t->pending:0);
#endif
        if (penv) *penv = NULL;
        return JNI_EDETACHED;
    }
    if (penv) *penv = (void *)t;
#ifdef LUNARIA_UNITY_PLAYER_PROBE
    probe_jni_lookup("vm","JavaVM","GetEnv","attached",1,t->pending);
#endif
    return JNI_OK;
}

/* 其余槽实现在下方；这里只做真实槽覆盖，stubs 先铺底。 */
static void bridge_install_real_slots(struct jni_bridge *b);

static void bridge_fill_env_table(struct jni_bridge *b)
{
    memset(&b->env_table, 0, sizeof(b->env_table));
    jni_abi_stubs_install(&b->env_table);
    bridge_install_real_slots(b);
}

/* ------------------------------------------------------------------ */
/* 生命周期                                                            */
/* ------------------------------------------------------------------ */

/* 句柄表的 local/global 引用 pin 回调。weak/class 不触发 pin。 */
static bool bridge_pin_cb(void *ctx, uint32_t object_ref)
{
    struct jni_bridge *b = (struct jni_bridge *)ctx;
    if (!b || b->magic != JNI_BRIDGE_MAGIC || object_ref == 0) return false;
    return dvm_gc_pin_checked(b->vm, object_ref);
}

static uint64_t bridge_identity_cb(void *ctx,uint32_t object_ref)
{
    struct jni_bridge *b=ctx;
    struct dvm_object *o=b?dvm__obj(b->vm,object_ref):NULL;
    return o?o->identity_generation:0;
}

static void bridge_unpin_cb(void *ctx, uint32_t object_ref)
{
    /* shutdown 时 b->active 已为 false，仍必须完成 unpin。 */
    struct jni_bridge *b = (struct jni_bridge *)ctx;
    if (b && b->magic == JNI_BRIDGE_MAGIC && object_ref)
        dvm_unpin(b->vm, object_ref);
}

static void bridge_free(struct jni_bridge *b)
{
    if (!b) return;
    if(b->gc_strong_registered)dvm_gc_root_provider_remove(b->vm,"jni.strong-and-pending",b);
    if(b->gc_weak_registered)dvm_gc_root_provider_remove(b->vm,"jni.weak",b);
    if (b->natives) {
        jni_native_state_destroy(b->natives);
        b->natives = NULL;
    }
    jni_handles_shutdown(&b->handles);
    free(b->methods);
    free(b->fields);
    b->methods = NULL;
    b->fields = NULL;
    pthread_mutex_destroy(&b->threads_mu);
    pthread_mutex_destroy(&b->error_mu);
    b->magic = 0;
    free(b);
}

bool jni_bridge_init(struct dvm *vm)
{
    if (!vm) return false;
    if (jni_bridge_for_vm(vm)) return true;
    if (jni_abi_check() != -1) return false;

    struct jni_bridge *b = calloc(1, sizeof(*b));
    if (!b) return false;
    b->magic = JNI_BRIDGE_MAGIC;
    b->vm = vm;
    b->active = true;
    b->method_capacity = JNI_METHOD_ID_CAPACITY;
    b->field_capacity = JNI_FIELD_ID_CAPACITY;
    b->methods = calloc(b->method_capacity, sizeof(*b->methods));
    b->fields = calloc(b->field_capacity, sizeof(*b->fields));
    if (!b->methods || !b->fields) {
        free(b->methods);
        free(b->fields);
        free(b);
        return false;
    }
    pthread_mutex_init(&b->threads_mu, NULL);
    pthread_mutex_init(&b->error_mu, NULL);
    atomic_init(&b->next_frame, 1u);
    atomic_init(&b->limited_count, 0u);

    uint32_t tag = atomic_fetch_add(&g_vm_tag_counter, 1u) + 1u;
    if ((tag & 0xffu) == 0) tag = 1u;
    b->vm_tag = (uint8_t)(tag & 0xffu);

    jni_handle_callbacks cb;
    memset(&cb, 0, sizeof(cb));
    cb.ctx = b;
    cb.pin = bridge_pin_cb;
    cb.unpin = bridge_unpin_cb;
    cb.identity = bridge_identity_cb;
    if (!jni_handles_init(&b->handles, JNI_HANDLE_DEFAULT_CAPACITY,
                          b->vm_tag, &cb)) {
        free(b->methods);
        free(b->fields);
        pthread_mutex_destroy(&b->threads_mu);
        pthread_mutex_destroy(&b->error_mu);
        free(b);
        return false;
    }

    /* 句柄表的 pin 回调直接调用 DVM，避免在 jni_handles 中依赖 dvm.h。 */
    b->natives = jni_native_state_create(b);
    if (!b->natives) {
        jni_handles_shutdown(&b->handles);
        free(b->methods);
        free(b->fields);
        pthread_mutex_destroy(&b->threads_mu);
        pthread_mutex_destroy(&b->error_mu);
        free(b);
        return false;
    }

    bridge_fill_vm_table(b);
    bridge_fill_env_table(b);
    if (!registry_add(b)) {
        jni_native_state_destroy(b->natives);
        jni_handles_shutdown(&b->handles);
        free(b->methods);
        free(b->fields);
        pthread_mutex_destroy(&b->threads_mu);
        pthread_mutex_destroy(&b->error_mu);
        free(b);
        return false;
    }
    if (!bridge_attach_thread(b, false, NULL)) {
        registry_remove(b);
        jni_native_state_destroy(b->natives);
        jni_handles_shutdown(&b->handles);
        free(b->methods);
        free(b->fields);
        pthread_mutex_destroy(&b->threads_mu);
        pthread_mutex_destroy(&b->error_mu);
        free(b);
        return false;
    }
    b->gc_strong_registered=dvm_gc_root_provider_add(vm,"jni.strong-and-pending",DVM_GC_ROOT_STRONG,jni_gc_strong_roots,b);
    if(b->gc_strong_registered)b->gc_weak_registered=dvm_gc_root_provider_add(vm,"jni.weak",DVM_GC_ROOT_WEAK,jni_gc_weak_roots,b);
    if(!b->gc_strong_registered||!b->gc_weak_registered){jni_bridge_shutdown(vm);return false;}
    return true;
}

bool jni_bridge_install_hooks(struct dvm *vm)
{
    struct jni_bridge *b = jni_bridge_for_vm(vm);
    if (!b) return false;
    if (b->hooks_installed) return true;
    b->saved_hooks = vm->hooks;
    vm->hooks.call_native = jni_bridge_call_native;
    vm->hooks.take_pending_exception = jni_bridge_take_pending_exception;
    vm->hooks.load_library = jni_bridge_load_library;
    b->hooks_installed = true;
    return true;
}

void jni_bridge_shutdown(struct dvm *vm)
{
    extern void dvm_lifetime_note(struct dvm *,const char *) __attribute__((weak));
    if(dvm_lifetime_note)dvm_lifetime_note(vm,"jni_bridge_shutdown");
    struct jni_bridge *b = jni_bridge_for_vm(vm);
    if (!b) return;
    bool owned = false;
    if (!dvm_gil_held(vm)) {
        dvm_gil_acquire(vm);
        owned = true;
    }
    if (b->hooks_installed && vm->hooks.call_native == jni_bridge_call_native) {
        vm->hooks = b->saved_hooks;
        b->hooks_installed = false;
    }

    pthread_mutex_lock(&b->threads_mu);
    struct jni_thread_env *t = b->threads;
    b->threads = NULL;
    b->thread_count = 0;
    pthread_mutex_unlock(&b->threads_mu);
    while (t) {
        struct jni_thread_env *next = t->next;
        if (g_thread_env == t) g_thread_env = NULL;
        cleanup_thread_locked(t);
        t->magic = 0;
        free(t);
        t = next;
    }

    b->active = false;
    registry_remove(b);
    bridge_free(b);
    if (owned) dvm_gil_release(vm);
}

JNIEnv *jni_bridge_env(struct jni_bridge *b)
{
    if (!b) return NULL;
    struct jni_thread_env *t = thread_ensure(b);
    return t ? (JNIEnv *)(void *)t : NULL;
}

JavaVM *jni_bridge_java_vm(struct jni_bridge *b)
{
    /* 返回 JavaVM 值所在存储的地址；b->vm_value 的值是 invoke 表地址。 */
    return b ? &b->vm_value : NULL;
}

struct dvm *jni_bridge_vm(struct jni_bridge *b)
{
    return b ? b->vm : NULL;
}

bool jni_bridge_attach_current(struct jni_bridge *b, bool daemon, JNIEnv **out)
{
    struct jni_thread_env *t = NULL;
    if (!bridge_attach_thread(b, daemon, &t)) return false;
    if (out) *out = (JNIEnv *)(void *)t;
    return true;
}

bool jni_bridge_detach_current(struct jni_bridge *b)
{
    return bridge_detach_thread(b);
}

bool jni_bridge_thread_is_attached(struct jni_bridge *b)
{
    return b && g_thread_env && g_thread_env->bridge == b;
}

unsigned jni_bridge_gil_drop(struct jni_bridge *b)
{
    if (!b || !dvm_gil_held(b->vm)) return 0;
    return dvm_gil_unlock_all(b->vm);
}

void jni_bridge_gil_restore(struct jni_bridge *b, unsigned depth)
{
    if (b && depth) dvm_gil_relock(b->vm, depth);
}

/* ------------------------------------------------------------------ */
/* 引用/ID/字段查找辅助                                                */
/* ------------------------------------------------------------------ */

static jobject thread_new_local_from_token(struct jni_thread_env *t,
                                           jobject token)
{
    if (!t || jni_token_is_null(token)) return jni_token_null();
    uint32_t ref = 0;
    void *cls = NULL;
    jni_ref_kind kind = JNI_REF_NONE;
    if (!jni_handle_resolve_any(&t->bridge->handles, token, &ref, &cls, &kind))
        return jni_token_null();
    if (cls) return new_local_class(t, (struct dvm_class *)cls);
    return new_local_obj(t, ref);
}

static jni_method_id_slot *method_id_slot(struct jni_bridge *b, jmethodID id)
{
    if (!b || !b->methods || !id) return NULL;
    uintptr_t p = (uintptr_t)id;
    uintptr_t base = (uintptr_t)b->methods;
    if (p < base) return NULL;
    size_t off = (size_t)(p - base);
    if (off % sizeof(*b->methods) != 0) return NULL;
    uint32_t index = (uint32_t)(off / sizeof(*b->methods));
    if (index >= b->method_capacity) return NULL;
    jni_method_id_slot *slot = &b->methods[index];
    if (slot->magic != JNI_METHOD_ID_MAGIC || !slot->live) return NULL;
    return slot;
}

static jni_field_id_slot *field_id_slot(struct jni_bridge *b, jfieldID id)
{
    if (!b || !b->fields || !id) return NULL;
    uintptr_t p = (uintptr_t)id;
    uintptr_t base = (uintptr_t)b->fields;
    if (p < base) return NULL;
    size_t off = (size_t)(p - base);
    if (off % sizeof(*b->fields) != 0) return NULL;
    uint32_t index = (uint32_t)(off / sizeof(*b->fields));
    if (index >= b->field_capacity) return NULL;
    jni_field_id_slot *slot = &b->fields[index];
    if (slot->magic != JNI_FIELD_ID_MAGIC || !slot->live) return NULL;
    return slot;
}

/* External static constants that native code reaches through
 * GetStaticFieldID/GetStaticIntField.  External classes have no DEX field
 * table, so field_lookup_common synthesizes an ID from these descriptors and
 * field_read_value returns the real Android constant.  Unknown external
 * fields still fail closed with NoSuchFieldError.  MODE_PRIVATE is 0 on
 * Android and is the mode Unity's PlayerPrefs upgrade passes to
 * getSharedPreferences. */
static struct dvm_field g_external_context_mode_private = {
    .name = "MODE_PRIVATE",
    .type = "I",
    .access = 0x19, /* ACC_PUBLIC | ACC_STATIC | ACC_FINAL */
    .cls = NULL,
    .slot = 0,
    .width = 1,
    .kind = 'I',
};

/* Environment.MEDIA_MOUNTED 是 Android 的公开静态常量 "mounted"；引擎在
 * 读取外部存储状态后用它与 getExternalStorageState() 的结果比较。外部类没有
 * DEX 字段表，因此与 MODE_PRIVATE 一样在这里合成真实常量。 */
static struct dvm_field g_external_environment_media_mounted = {
    .name = "MEDIA_MOUNTED",
    .type = "Ljava/lang/String;",
    .access = 0x19,
    .cls = NULL,
    .slot = 0,
    .width = 1,
    .kind = 'L',
};

static struct dvm_field g_external_context_location_service = {
    .name = "LOCATION_SERVICE",
    .type = "Ljava/lang/String;",
    .access = 0x19,
    .width = 1,
    .kind = 'L',
};

/* Android Context.DISPLAY_SERVICE is the public string "display"; the engine
 * resolves it during display/window initialization. */
static struct dvm_field g_external_context_display_service = {
    .name = "DISPLAY_SERVICE",
    .type = "Ljava/lang/String;",
    .access = 0x19,
    .width = 1,
    .kind = 'L',
};

/* Context.AUDIO_SERVICE / MEDIA_ROUTER_SERVICE are the public strings "audio"
 * and "media_router".  The engine reads them before HFPStatus/audio setup;
 * getSystemService already returns a real AudioManager object for "audio". */
static struct dvm_field g_external_context_audio_service = {
    .name = "AUDIO_SERVICE",
    .type = "Ljava/lang/String;",
    .access = 0x19,
    .width = 1,
    .kind = 'L',
};
static struct dvm_field g_external_context_media_router_service = {
    .name = "MEDIA_ROUTER_SERVICE",
    .type = "Ljava/lang/String;",
    .access = 0x19,
    .width = 1,
    .kind = 'L',
};

extern bool content_runtime_constant(struct dvm *,const char *,const char *,const char *,union dvm_value *) __attribute__((weak));
static struct dvm_field settings_constants[24];
static unsigned settings_constant_count;
static bool external_static_constant(struct dvm_class *c, const char *name,
                                     const char *type, struct dvm_field **field)
{
    if (!c || !c->external || !name || !type || !field) return false;
    if(content_runtime_constant&&content_runtime_constant(NULL,c->name,name,type,NULL)){
        for(unsigned i=0;i<settings_constant_count;i++)if(!strcmp(settings_constants[i].name,name)){*field=&settings_constants[i];return true;}
        if(settings_constant_count>=24)return false;
        struct dvm_field *f=&settings_constants[settings_constant_count];
        f->name=strdup(name);f->type=strdup(type);f->kind='L';f->access=0x0008;
        if(!f->name||!f->type){free((void *)f->name);free((void *)f->type);memset(f,0,sizeof *f);return false;}
        settings_constant_count++;*field=f;return true;
    }
    if (!strcmp(c->name, "android/content/Context") &&
        !strcmp(name, "MODE_PRIVATE") && !strcmp(type, "I")) {
        *field = &g_external_context_mode_private;
        return true;
    }
    if (!strcmp(c->name, "android/os/Environment") &&
        !strcmp(name, "MEDIA_MOUNTED") && !strcmp(type, "Ljava/lang/String;")) {
        *field = &g_external_environment_media_mounted;
        return true;
    }
    if (!strcmp(c->name, "android/content/Context") &&
        !strcmp(name, "LOCATION_SERVICE") && !strcmp(type, "Ljava/lang/String;")) {
        *field = &g_external_context_location_service;
        return true;
    }
    if (!strcmp(c->name, "android/content/Context") &&
        !strcmp(name, "DISPLAY_SERVICE") && !strcmp(type, "Ljava/lang/String;")) {
        *field = &g_external_context_display_service;
        return true;
    }
    if (!strcmp(c->name, "android/content/Context") &&
        !strcmp(name, "AUDIO_SERVICE") && !strcmp(type, "Ljava/lang/String;")) {
        *field = &g_external_context_audio_service;
        return true;
    }
    if (!strcmp(c->name, "android/content/Context") &&
        !strcmp(name, "MEDIA_ROUTER_SERVICE") && !strcmp(type, "Ljava/lang/String;")) {
        *field = &g_external_context_media_router_service;
        return true;
    }
    return false;
}

static jmethodID method_id_alloc(struct jni_bridge *b, struct dvm_method *m,
                                 bool is_static)
{
    if (!b || !m) return NULL;
    /* Native dispatch tables retain jmethodID as method identity. JNI lookups
     * and FromReflectedMethod must return the same live ID for that method. */
    for (uint32_t i = 0; i < b->method_used; ++i) {
        jni_method_id_slot *known = &b->methods[i];
        if (known->live && known->magic == JNI_METHOD_ID_MAGIC &&
            known->method == m && known->is_static == is_static)
            return (jmethodID)(void *)known;
    }
    if (b->method_used >= b->method_capacity) return NULL;
    jni_method_id_slot *slot = &b->methods[b->method_used++];
    memset(slot, 0, sizeof(*slot));
    slot->magic = JNI_METHOD_ID_MAGIC;
    slot->live = true;
    slot->is_static = is_static;
    slot->method = m;
    slot->decl = m->cls;
    return (jmethodID)(void *)slot;
}

static jfieldID field_id_alloc(struct jni_bridge *b, struct dvm_field *f,
                               struct dvm_class *decl, bool is_static)
{
    if (!b || !f) return NULL;
    if (b->field_used >= b->field_capacity) return NULL;
    jni_field_id_slot *slot = &b->fields[b->field_used++];
    memset(slot, 0, sizeof(*slot));
    slot->magic = JNI_FIELD_ID_MAGIC;
    slot->live = true;
    slot->is_static = is_static;
    slot->field = f;
    slot->decl = decl;
    return (jfieldID)(void *)slot;
}

static struct dvm_field *find_field_list(struct dvm_field *fields, int count,
                                         const char *name, const char *type)
{
    if (!fields || count <= 0) return NULL;
    for (int i = 0; i < count; ++i) {
        struct dvm_field *f = &fields[i];
        if (f->name && f->type && strcmp(f->name, name) == 0 &&
            strcmp(f->type, type) == 0)
            return f;
    }
    return NULL;
}

static struct dvm_field *find_instance_field_rec(struct dvm_class *c,
                                                 const char *name,
                                                 const char *type,
                                                 struct dvm_class **decl)
{
    for (int depth = 0; c && depth < 64; ++depth, c = c->super) {
        struct dvm_field *f = find_field_list(c->ifields, c->nifields,
                                              name, type);
        if (f) {
            if (decl) *decl = c;
            return f;
        }
    }
    return NULL;
}

static struct dvm_field *find_static_field_rec(struct dvm_class *c,
                                               const char *name,
                                               const char *type,
                                               struct dvm_class **decl,
                                               int depth)
{
    if (!c || depth > 64) return NULL;
    struct dvm_field *f = find_field_list(c->sfields, c->nsfields, name, type);
    if (f) {
        if (decl) *decl = c;
        return f;
    }
    for (int i = 0; i < c->nifaces; ++i) {
        f = find_static_field_rec(c->ifaces[i], name, type, decl, depth + 1);
        if (f) return f;
    }
    return find_static_field_rec(c->super, name, type, decl, depth + 1);
}

static bool field_is_ref(char kind)
{
    return kind == 'L' || kind == '[';
}

static bool field_read_value(struct jni_thread_env *t, jni_field_id_slot *fs,
                             dvm_ref obj, union dvm_value *out)
{
    struct dvm *vm = t->bridge->vm;
    memset(out, 0, sizeof(*out));
    if (!fs || !fs->field) return false;
    if(fs->is_static&&content_runtime_constant&&fs->decl&&content_runtime_constant(vm,fs->decl->name,fs->field->name,fs->field->type,out))return true;
    /* Synthetic external constant: no class storage and no <clinit>; the
     * value is fixed by the Android platform contract. */
    if (fs->is_static && fs->field == &g_external_context_mode_private) {
        out->i = 0;
        return true;
    }
    if (fs->is_static && fs->field == &g_external_environment_media_mounted) {
        out->l = dvm_new_string(vm, "mounted");
        return out->l != 0;
    }
    if (fs->is_static && fs->field == &g_external_context_location_service) {
        out->l = dvm_new_string(vm, "location");
        return out->l != 0;
    }
    if (fs->is_static && fs->field == &g_external_context_display_service) {
        out->l = dvm_new_string(vm, "display");
        return out->l != 0;
    }
    if (fs->is_static && fs->field == &g_external_context_audio_service) {
        out->l = dvm_new_string(vm, "audio");
        return out->l != 0;
    }
    if (fs->is_static && fs->field == &g_external_context_media_router_service) {
        out->l = dvm_new_string(vm, "media_router");
        return out->l != 0;
    }
    if (fs->is_static) {
        if (!dvm_init_class(vm, fs->decl)) {
            if (!move_dvm_exception_to_pending(t))
                bridge_set_pending_new(t->bridge, "java/lang/ExceptionInInitializerError",
                                       "static field class initialization failed");
            return false;
        }
        if (dvm_get_static(vm, fs->decl, fs->field->name, fs->field->type, out)) {
#ifdef LUNARIA_UNITY_PLAYER_PROBE
            if (!strcmp(fs->decl->name, "android/os/Build") && fs->field->kind == 'L') {
                const char *text = out->l ? dvm_string_utf8(vm, out->l) : NULL;
                int saved_errno = errno;
                FILE *log = fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt", "a");
                if (log) {
                    fprintf(log, "JNI_BUILD_STATIC name=%s ref=%u value=%s pending=%u\n",
                            fs->field->name, (unsigned)out->l, text ? text : "<null>",
                            (unsigned)t->pending);
                    fclose(log);
                }
                errno = saved_errno;
            }
            /* UnityPlayer.currentActivity 的实际对象类：托管层后续用它做
             * ReflectionHelper 反射；类必须解析成 Activity 子类而不是 Object。 */
            if (fs->decl->name && !strcmp(fs->decl->name, "com/unity3d/player/UnityPlayer") &&
                fs->field->name && !strcmp(fs->field->name, "currentActivity")) {
                struct dvm_class *rc = out->l ? dvm_object_class(vm, out->l) : NULL;
                int saved_errno = errno;
                FILE *log = fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt", "a");
                if (log) {
                    fprintf(log, "UNITY_CURRENT_ACTIVITY ref=%u class=%s pending=%u\n",
                            (unsigned)out->l, rc && rc->name ? rc->name : "<null>",
                            (unsigned)t->pending);
                    fclose(log);
                }
                errno = saved_errno;
            }
#endif
            return true;
        }
    } else {
        if (!obj) {
            bridge_set_pending_new(t->bridge, "java/lang/NullPointerException",
                                   "field access on null object");
            return false;
        }
        if (dvm__get_field_from(vm, obj, fs->decl, fs->field->name,
                                fs->field->type, out))
            return true;
    }
    if (!move_dvm_exception_to_pending(t))
        bridge_set_pending_new(t->bridge, "java/lang/NoSuchFieldError",
                               fs->field->name ? fs->field->name : "field");
    return false;
}

static bool field_write_value(struct jni_thread_env *t, jni_field_id_slot *fs,
                              dvm_ref obj, union dvm_value val)
{
    struct dvm *vm = t->bridge->vm;
    if (!fs || !fs->field) return false;
    if (fs->is_static) {
        if (!dvm_init_class(vm, fs->decl)) {
            if (!move_dvm_exception_to_pending(t))
                bridge_set_pending_new(t->bridge, "java/lang/ExceptionInInitializerError",
                                       "static field class initialization failed");
            return false;
        }
        if (dvm_set_static(vm, fs->decl, fs->field->name, fs->field->type, val))
            return true;
    } else {
        if (!obj) {
            bridge_set_pending_new(t->bridge, "java/lang/NullPointerException",
                                   "field access on null object");
            return false;
        }
        if (dvm__set_field_from(vm, obj, fs->decl, fs->field->name,
                                fs->field->type, val))
            return true;
    }
    if (!move_dvm_exception_to_pending(t))
        bridge_set_pending_new(t->bridge, "java/lang/NoSuchFieldError",
                               fs->field->name ? fs->field->name : "field");
    return false;
}

static bool object_arg(struct jni_thread_env *t, jobject token, dvm_ref *out,
                       const char *what)
{
    if (!token_to_dvm_ref(t, token, out)) {
        bridge_set_pending_new(t->bridge, "java/lang/IllegalArgumentException",
                               what ? what : "invalid JNI object reference");
        return false;
    }
    if (*out == 0) {
        bridge_set_pending_new(t->bridge, "java/lang/NullPointerException",
                               what ? what : "null object reference");
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* 基础槽：版本、类、异常、引用、帧                                    */
/* ------------------------------------------------------------------ */

static jint JNICALL bridge_GetVersion(JNIEnv *env)
{
    (void)env;
    return JNI_VERSION_1_6;
}

static jclass JNICALL bridge_FindClass(JNIEnv *env, const char *name)
{
    struct jni_thread_env *t = NULL;
    bool owned = false;
    if (!enter_slot(env, &t, &owned)) return NULL;
    jclass result = NULL;
    if (!name) {
        bridge_set_pending_new(t->bridge, "java/lang/NullPointerException",
                               "FindClass name is null");
        goto done;
    }
    if (!dvm_class_exists(t->bridge->vm, name)) {
        char msg[256];
        snprintf(msg, sizeof(msg), "class not found: %s", name);
        bridge_set_pending_new(t->bridge, "java/lang/NoClassDefFoundError", msg);
        goto done;
    }
    struct dvm_class *cls = dvm_find_class_or_external(t->bridge->vm, name);
    /* dvm_class_exists() 已排除了 APK/平台都没有的应用类；平台命名空间里
     * 尚未建模的类会以 external/synthesized 占位类返回，这是 ThrowNew 等
     * 路径需要的合法结果，不能在这里一概拒绝。 */
    if (!cls) {
        char msg[256];
        snprintf(msg, sizeof(msg), "class not found: %s", name);
        bridge_set_pending_new(t->bridge, "java/lang/NoClassDefFoundError", msg);
        goto done;
    }
    result = new_local_class(t, cls);
done:
#ifdef LUNARIA_UNITY_PLAYER_PROBE
    probe_jni_lookup("class",name,"FindClass","",result!=NULL,t->pending);
#endif
    leave_slot(t, owned);
    return result;
}

static jclass JNICALL bridge_GetSuperclass(JNIEnv *env, jclass sub)
{
    struct jni_thread_env *t = NULL;
    bool owned = false;
    if (!enter_slot(env, &t, &owned)) return NULL;
    jclass result = NULL;
    struct dvm_class *c = NULL;
    if (!resolve_class_token(t->bridge, sub, &c) || !c) {
        bridge_set_pending_new(t->bridge, "java/lang/NullPointerException",
                               "GetSuperclass null class");
        goto done;
    }
    if (c->super) result = new_local_class(t, c->super);
done:
    leave_slot(t, owned);
    return result;
}

static jboolean JNICALL bridge_IsAssignableFrom(JNIEnv *env, jclass sub,
                                                jclass sup)
{
    struct jni_thread_env *t = NULL;
    bool owned = false;
    if (!enter_slot(env, &t, &owned)) return JNI_FALSE;
    jboolean result = JNI_FALSE;
    struct dvm_class *cs = NULL, *cp = NULL;
    if (!resolve_class_token(t->bridge, sub, &cs) ||
        !resolve_class_token(t->bridge, sup, &cp) || !cs || !cp) {
        bridge_set_pending_new(t->bridge, "java/lang/NullPointerException",
                               "IsAssignableFrom null class");
        goto done;
    }
    result = dvm__class_assignable(t->bridge->vm, cs, cp)
             ? JNI_TRUE : JNI_FALSE;
done:
    leave_slot(t, owned);
    return result;
}

/* JNI FromReflectedMethod/FromReflectedField：把 java.lang.reflect.Method /
 * Field 对象还原成 jmethodID/jfieldID。句柄来自反射运行时持有的真实
 * dvm_method/dvm_field；对象不是对应反射类型时按 JNI 契约抛
 * IllegalArgumentException，不编造句柄。 */
static jmethodID JNICALL bridge_FromReflectedMethod(JNIEnv *env,
                                                    jobject method)
{
    struct jni_thread_env *t = NULL;
    bool owned = false;
    if (!enter_slot(env, &t, &owned)) return NULL;
    jmethodID result = NULL;
    dvm_ref ref = 0;
    struct dvm_method *m = NULL;
    if (token_to_dvm_ref(t, method, &ref) && ref &&
        reflection_runtime_method_handle(t->bridge->vm, ref, &m) && m) {
        result = method_id_alloc(t->bridge, m, dvm_method_is_static(m));
    }
    if (!result && !jni_token_is_null(method))
        bridge_set_pending_new(t->bridge, "java/lang/IllegalArgumentException",
                               "FromReflectedMethod invalid Method");
    leave_slot(t, owned);
    return result;
}

static jfieldID JNICALL bridge_FromReflectedField(JNIEnv *env, jobject field)
{
    struct jni_thread_env *t = NULL;
    bool owned = false;
    if (!enter_slot(env, &t, &owned)) return NULL;
    jfieldID result = NULL;
    dvm_ref ref = 0;
    struct dvm_field *f = NULL;
    struct dvm_class *owner = NULL;
    bool is_static = false;
    if (token_to_dvm_ref(t, field, &ref) && ref &&
        reflection_runtime_field_handle(t->bridge->vm, ref, &f, &owner,
                                        &is_static) && f) {
        result = field_id_alloc(t->bridge, f, owner, is_static);
    }
    if (!result && !jni_token_is_null(field))
        bridge_set_pending_new(t->bridge, "java/lang/IllegalArgumentException",
                               "FromReflectedField invalid Field");
    leave_slot(t, owned);
    return result;
}

/* JNI ToReflectedMethod（本桥定制表的 slot 9）：把 jmethodID 还原成
 * java.lang.reflect.Method 对象。句柄本身携带真实 dvm_method 与声明类；
 * isStatic 参数与句柄不符时按 JNI 契约属未定义行为，这里不因此伪造失败，
 * 返回真实 Method。72pz 实测引擎在 worker 分派路径调用该槽，stub 直接
 * 抛 UnsatisfiedLinkError 打断 nativeRender 的帧回调链。 */
static jobject JNICALL bridge_ToReflectedMethod(JNIEnv *env, jclass cls,
                                                jmethodID methodID,
                                                jboolean isStatic)
{
    struct jni_thread_env *t = NULL;
    bool owned = false;
    if (!enter_slot(env, &t, &owned)) return NULL;
    jobject result = NULL;
    jni_method_id_slot *slot = method_id_slot(t->bridge, methodID);
    struct dvm_class *clazz = NULL;
    if (slot && slot->method && resolve_class_token(t->bridge, cls, &clazz) && clazz &&
        slot->is_static == (isStatic != JNI_FALSE) &&
        dvm__class_assignable(t->bridge->vm, clazz, slot->decl)) {
        dvm_ref ref = 0;
        if (reflection_runtime_method_object(t->bridge->vm, slot->decl,
                                             slot->method, &ref) && ref) {
            result = new_local_obj(t, ref);
        }
    }
    if (!result && !move_dvm_exception_to_pending(t))
        bridge_set_pending_new(t->bridge, "java/lang/IllegalArgumentException",
                               "ToReflectedMethod invalid methodID");
    leave_slot(t, owned);
    return result;
}

static jint JNICALL bridge_Throw(JNIEnv *env, jthrowable obj)
{
    struct jni_thread_env *t = NULL;
    bool owned = false;
    if (!enter_slot(env, &t, &owned)) return JNI_ERR;
    jint result = JNI_ERR;
    uint32_t ref = 0;
    if (!resolve_object_any(t->bridge, obj, &ref) || ref == 0) {
        bridge_set_pending_new(t->bridge, "java/lang/NullPointerException",
                               "Throw null or invalid throwable");
        goto done;
    }
    struct dvm_class *throwable = dvm_find_class(t->bridge->vm,
                                                 "java/lang/Throwable");
    struct dvm_object *o = dvm__obj(t->bridge->vm, ref);
    if (!o || (throwable && !dvm__class_assignable(t->bridge->vm, o->cls,
                                                   throwable))) {
        bridge_set_pending_new(t->bridge, "java/lang/IllegalArgumentException",
                               "Throw argument is not a Throwable");
        goto done;
    }
    if (t->pending && t->pending_pinned) dvm_unpin(t->bridge->vm, t->pending);
    t->pending = ref;
    dvm_pin(t->bridge->vm, ref);
    t->pending_pinned = true;
    result = JNI_OK;
done:
    leave_slot(t, owned);
    return result;
}

static jint JNICALL bridge_ThrowNew(JNIEnv *env, jclass clazz, const char *msg)
{
    struct jni_thread_env *t = NULL;
    bool owned = false;
    if (!enter_slot(env, &t, &owned)) return JNI_ERR;
    jint result = JNI_ERR;
    struct dvm_class *c = NULL;
    if (!resolve_class_token(t->bridge, clazz, &c) || !c) {
        bridge_set_pending_new(t->bridge, "java/lang/NullPointerException",
                               "ThrowNew null class");
        goto done;
    }
    struct dvm_class *throwable = dvm_find_class(t->bridge->vm,
                                                 "java/lang/Throwable");
    if (throwable && !dvm__class_assignable(t->bridge->vm, c, throwable)) {
        bridge_set_pending_new(t->bridge, "java/lang/IllegalArgumentException",
                               "ThrowNew class is not a Throwable");
        goto done;
    }
    result = bridge_set_pending_new(t->bridge, c->name,
                                    msg ? msg : "") ? JNI_OK : JNI_ERR;
done:
    leave_slot(t, owned);
    return result;
}

#ifdef LUNARIA_UNITY_PLAYER_PROBE
extern int sdk_trace_fprintf(FILE *,const char *,...);
/* Caller has entered the slot and owns the DVM GIL; no handle or bridge
 * mutex is held. Describe both exception channels before any mutation. */
static void diagnostic_primary_exception(struct jni_thread_env *t,const char *where)
{
    static _Thread_local int tracing;
    if(!t || tracing)return;
    tracing=1;
    struct dvm *vm=t->bridge->vm;
    dvm_ref pending=t->pending, vm_exception=dvm_exception(vm);
    char a[1024]="-", b[1024]="-";
    if(pending)dvm_describe_exception(vm,pending,a,sizeof a);
    if(vm_exception)dvm_describe_exception(vm,vm_exception,b,sizeof b);
    sdk_trace_fprintf(stderr,"JNI_PRIMARY_EXCEPTION where=%s pending=%u pending_text=%s vm_exception=%u vm_text=%s\n",
                     where,pending,a,vm_exception,b);
    tracing=0;
}
#endif

static jthrowable JNICALL bridge_ExceptionOccurred(JNIEnv *env)
{
    struct jni_thread_env *t = NULL;
    bool owned = false;
    if (!enter_slot(env, &t, &owned)) return NULL;
#ifdef LUNARIA_UNITY_PLAYER_PROBE
    diagnostic_primary_exception(t,"ExceptionOccurred");
#endif
    jthrowable result = NULL;
    if (t->pending) result = new_local_obj(t, t->pending);
    leave_slot(t, owned);
    return result;
}

static void JNICALL bridge_ExceptionDescribe(JNIEnv *env)
{
    struct jni_thread_env *t = NULL;
    bool owned = false;
    if (!enter_slot(env, &t, &owned)) return;
#ifdef LUNARIA_UNITY_PLAYER_PROBE
    diagnostic_primary_exception(t,"ExceptionDescribe");
#endif
    if (t->pending) {
        char buf[512];
        dvm_describe_exception(t->bridge->vm, t->pending, buf, sizeof(buf));
        fprintf(stderr, "[jni] pending exception: %s\n", buf);
    }
    leave_slot(t, owned);
}

static void JNICALL bridge_ExceptionClear(JNIEnv *env)
{
    struct jni_thread_env *t = NULL;
    bool owned = false;
    if (!enter_slot(env, &t, &owned)) return;
#ifdef LUNARIA_UNITY_PLAYER_PROBE
    diagnostic_primary_exception(t,"ExceptionClear");
#endif
    if (t->pending && t->pending_pinned)
        dvm_unpin(t->bridge->vm, t->pending);
    t->pending = 0;
    t->pending_pinned = false;
    leave_slot(t, owned);
}

static void JNICALL bridge_FatalError(JNIEnv *env, const char *msg)
{
    (void)env;
    fprintf(stderr, "[jni] FATAL: %s\n", msg ? msg : "(null)");
    fflush(stderr);
    abort();
}

static jboolean JNICALL bridge_ExceptionCheck(JNIEnv *env)
{
    struct jni_thread_env *t = thread_env_from_env(env);
#ifdef LUNARIA_UNITY_PLAYER_PROBE
    static _Thread_local unsigned last_observed_pending;
    static atomic_uint pending_check_count;
    if (t && t->pending &&
        atomic_fetch_add_explicit(&pending_check_count, 1, memory_order_relaxed) < 4096) {
        int saved_errno = errno;
        FILE *log = fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt", "a");
        if (log) {
            fprintf(log, "JNI_PENDING_CHECK thread=%llu caller=%p pending=%u\n",
                    (unsigned long long)(uintptr_t)pthread_self(),
                    __builtin_return_address(0), (unsigned)t->pending);
            fclose(log);
        }
        errno = saved_errno;
    }
    if(t&&t->pending!=last_observed_pending){
        last_observed_pending=t->pending;
        probe_jni_lookup("pending_transition","JNI","ExceptionCheck","observed",t->pending==0,t->pending);
    }
#endif
    return (t && t->pending) ? JNI_TRUE : JNI_FALSE;
}

static jint JNICALL bridge_PushLocalFrame(JNIEnv *env, jint capacity)
{
    struct jni_thread_env *t = NULL;
    bool owned = false;
    if (!enter_slot(env, &t, &owned)) return JNI_ERR;
    jint result = JNI_ERR;
    if (capacity < 0) {
        bridge_set_pending_new(t->bridge, "java/lang/IllegalArgumentException",
                               "negative local frame capacity");
        goto done;
    }
    if (t->frame_depth >= (int)JNI_MAX_THREAD_FRAMES) {
        bridge_set_pending_new(t->bridge, "java/lang/OutOfMemoryError",
                               "local frame stack exhausted");
        goto done;
    }
    uint32_t frame = next_frame_id(t->bridge);
    if (frame == 0) {
        bridge_set_pending_new(t->bridge, "java/lang/OutOfMemoryError",
                               "local frame id exhausted");
        goto done;
    }
    t->frames[t->frame_depth++] = frame;
    result = JNI_OK;
done:
    leave_slot(t, owned);
    return result;
}

static jobject JNICALL bridge_PopLocalFrame(JNIEnv *env, jobject result)
{
    struct jni_thread_env *t = NULL;
    bool owned = false;
    if (!enter_slot(env, &t, &owned)) return NULL;
    jobject promoted = jni_token_null();
    if (t->frame_depth <= 1) {
        bridge_set_pending_new(t->bridge, "java/lang/IllegalStateException",
                               "PopLocalFrame without matching PushLocalFrame");
        goto done;
    }
    uint32_t current = t->frames[t->frame_depth - 1];
    uint32_t parent = t->frames[t->frame_depth - 2];
    if (!jni_token_is_null(result)) {
        promoted = jni_handle_promote_to_frame(&t->bridge->handles, result,
                                               parent);
        if (jni_token_is_null(promoted)&&!jni_gc_cleared_weak(t->bridge,result)) {
            bridge_set_pending_new(t->bridge, "java/lang/IllegalArgumentException",
                                   "PopLocalFrame result is invalid");
        }
    }
    jni_handle_pop_frame_slots(&t->bridge->handles, current);
    t->frames[t->frame_depth - 1] = 0;
    --t->frame_depth;
done:
    leave_slot(t, owned);
    return promoted;
}

static jobject JNICALL bridge_NewGlobalRef(JNIEnv *env, jobject lobj)
{
    struct jni_thread_env *t = NULL;
    bool owned = false;
    if (!enter_slot(env, &t, &owned)) return NULL;
    jobject result = NULL;
    if (!jni_token_is_null(lobj)) {
        uint32_t ref = 0;
        void *cls = NULL;
        jni_ref_kind kind = JNI_REF_NONE;
        if (!jni_handle_resolve_any(&t->bridge->handles, lobj, &ref, &cls,
                                    &kind)) {
            bridge_set_pending_new(t->bridge, "java/lang/IllegalArgumentException",
                                   "NewGlobalRef invalid reference");
        } else {
            result = jni_handle_new_global_ref(&t->bridge->handles, lobj);
            if (jni_token_is_null(result)&&!jni_gc_cleared_weak(t->bridge,lobj))
                bridge_set_pending_new(t->bridge, "java/lang/OutOfMemoryError",
                                       "global reference table exhausted");
        }
    }
    leave_slot(t, owned);
    return result;
}

static void JNICALL bridge_DeleteGlobalRef(JNIEnv *env, jobject gref)
{
    struct jni_thread_env *t = NULL;
    bool owned = false;
    if (!enter_slot(env, &t, &owned)) return;
    if (!jni_token_is_null(gref))
        (void)jni_handle_delete_global(&t->bridge->handles, gref);
    leave_slot(t, owned);
}

static void JNICALL bridge_DeleteLocalRef(JNIEnv *env, jobject obj)
{
    struct jni_thread_env *t = NULL;
    bool owned = false;
    if (!enter_slot(env, &t, &owned)) return;
    if (!jni_token_is_null(obj))
        (void)jni_handle_delete_local_any(&t->bridge->handles, obj);
    leave_slot(t, owned);
}

static jboolean JNICALL bridge_IsSameObject(JNIEnv *env, jobject obj1,
                                            jobject obj2)
{
    struct jni_thread_env *t = NULL;
    bool owned = false;
    if (!enter_slot(env, &t, &owned)) return JNI_FALSE;
    /* A Class returned by guest code is a DVM class-object reference, while
     * FindClass/GetObjectClass use a direct class token. They denote the same
     * Java object when their VM class identities agree. */
    struct dvm_class *class1=NULL,*class2=NULL;
    bool is_class1=resolve_class_token(t->bridge,(jclass)obj1,&class1);
    bool is_class2=resolve_class_token(t->bridge,(jclass)obj2,&class2);
    jboolean result=(is_class1||is_class2)
        ? (is_class1&&is_class2&&class1==class2?JNI_TRUE:JNI_FALSE)
        : jni_handle_is_same(&t->bridge->handles, obj1, obj2);
    leave_slot(t, owned);
    return result;
}

static jobject JNICALL bridge_NewLocalRef(JNIEnv *env, jobject ref)
{
    struct jni_thread_env *t = NULL;
    bool owned = false;
    if (!enter_slot(env, &t, &owned)) return NULL;
    jobject result = thread_new_local_from_token(t, ref);
    if (!jni_token_is_null(ref) && jni_token_is_null(result)&&!jni_gc_cleared_weak(t->bridge,ref))
        bridge_set_pending_new(t->bridge, "java/lang/IllegalArgumentException",
                               "NewLocalRef invalid reference");
    leave_slot(t, owned);
    return result;
}

static jint JNICALL bridge_EnsureLocalCapacity(JNIEnv *env, jint capacity)
{
    struct jni_thread_env *t = NULL;
    bool owned = false;
    if (!enter_slot(env, &t, &owned)) return JNI_ERR;
    jint result = JNI_ERR;
    if (capacity < 0) {
        bridge_set_pending_new(t->bridge, "java/lang/IllegalArgumentException",
                               "negative local capacity");
        goto done;
    }
    uint32_t free_slots = t->bridge->handles.capacity -
                          t->bridge->handles.live_count;
    if ((uint32_t)capacity > free_slots) {
        bridge_set_pending_new(t->bridge, "java/lang/OutOfMemoryError",
                               "local reference capacity exhausted");
        goto done;
    }
    result = JNI_OK;
done:
    leave_slot(t, owned);
    return result;
}

static jobject JNICALL bridge_AllocObject(JNIEnv *env, jclass clazz)
{
    struct jni_thread_env *t = NULL;
    bool owned = false;
    if (!enter_slot(env, &t, &owned)) return NULL;
    jobject result = NULL;
    struct dvm_class *c = NULL;
    if (!resolve_class_token(t->bridge, clazz, &c) || !c) {
        bridge_set_pending_new(t->bridge, "java/lang/NullPointerException",
                               "AllocObject null class");
        goto done;
    }
    if (c->is_array || c->is_primitive || c->is_interface ||
        (c->access & DEX_ACC_ABSTRACT)) {
        bridge_set_pending_new(t->bridge, "java/lang/InstantiationException",
                               c->name ? c->name : "abstract class");
        goto done;
    }
    dvm_ref ref = dvm_new_object(t->bridge->vm, c);
    if (!ref) {
        if (!move_dvm_exception_to_pending(t))
            bridge_set_pending_new(t->bridge, "java/lang/OutOfMemoryError",
                                   "AllocObject allocation failed");
        goto done;
    }
    result = new_local_obj(t, ref);
done:
    leave_slot(t, owned);
    return result;
}

static jclass JNICALL bridge_GetObjectClass(JNIEnv *env, jobject obj)
{
    struct jni_thread_env *t = NULL;
    bool owned = false;
    if (!enter_slot(env, &t, &owned)) return NULL;
    jclass result = NULL;
    uint32_t ref = 0;
    void *cls = NULL;
    jni_ref_kind kind = JNI_REF_NONE;
    if (jni_token_is_null(obj) ||
        !jni_handle_resolve_any(&t->bridge->handles, obj, &ref, &cls, &kind)) {
        bridge_set_pending_new(t->bridge, "java/lang/NullPointerException",
                               "GetObjectClass null object");
        goto done;
    }
    struct dvm_class *c = NULL;
    if (cls) {
        c = dvm_find_class_or_external(t->bridge->vm, "java/lang/Class");
    } else {
        c = dvm_object_class(t->bridge->vm, ref);
        if (!c) {
            struct dvm_object *o = dvm__obj(t->bridge->vm, ref);
            if (o) c = o->cls;
        }
    }
    if (!c) {
        bridge_set_pending_new(t->bridge, "java/lang/NoClassDefFoundError",
                               "GetObjectClass cannot resolve class");
        goto done;
    }
    result = new_local_class(t, c);
done:
    leave_slot(t, owned);
    return result;
}

static jboolean JNICALL bridge_IsInstanceOf(JNIEnv *env, jobject obj,
                                            jclass clazz)
{
    struct jni_thread_env *t = NULL;
    bool owned = false;
    if (!enter_slot(env, &t, &owned)) return JNI_FALSE;
    jboolean result = JNI_FALSE;
    struct dvm_class *target = NULL;
    struct dvm_class *from = NULL;
    if (!resolve_class_token(t->bridge, clazz, &target) || !target) {
        bridge_set_pending_new(t->bridge, "java/lang/NullPointerException",
                               "IsInstanceOf null class");
        goto done;
    }
    if (jni_token_is_null(obj)) goto done;
    uint32_t ref = 0;
    void *cls = NULL;
    jni_ref_kind kind = JNI_REF_NONE;
    if (!jni_handle_resolve_any(&t->bridge->handles, obj, &ref, &cls, &kind)) {
        bridge_set_pending_new(t->bridge, "java/lang/IllegalArgumentException",
                               "IsInstanceOf invalid object");
        goto done;
    }
    if (cls) {
        from = dvm_find_class_or_external(t->bridge->vm, "java/lang/Class");
    } else {
        from = dvm_object_class(t->bridge->vm, ref);
    }
    if (from && dvm__class_assignable(t->bridge->vm, from, target))
        result = JNI_TRUE;
done:
    if (target && target->desc && strstr(target->desc, "MotionEvent")) {
        static int logged;
        if (logged < 32) {
            logged++;
            FILE *f = fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt", "a");
            if (f) {
                fprintf(f, "ISINSTANCEOF target=%s result=%d from=%s from_desc=%s\n",
                        target->desc, result == JNI_TRUE ? 1 : 0,
                        from && from->name ? from->name : "?",
                        from && from->desc ? from->desc : "?");
                fclose(f);
            }
        }
    }
    leave_slot(t, owned);
    return result;
}

/* ------------------------------------------------------------------ */
/* 方法/字段 ID                                                        */
/* ------------------------------------------------------------------ */

static jmethodID method_lookup_common(JNIEnv *env, jclass clazz,
                                      const char *name, const char *sig,
                                      bool want_static)
{
    struct jni_thread_env *t = NULL;
    bool owned = false;
    if (!enter_slot(env, &t, &owned)) return NULL;
    jmethodID result = NULL;
    struct dvm_class *c = NULL;
    if (!name || !sig) {
        bridge_set_pending_new(t->bridge, "java/lang/NullPointerException",
                               "method name/signature is null");
        goto done;
    }
    if (!resolve_class_token(t->bridge, clazz, &c) || !c) {
        bridge_set_pending_new(t->bridge, "java/lang/NullPointerException",
                               "method lookup null class");
        goto done;
    }
    struct dvm_method *m = dvm_find_method(t->bridge->vm, c, name, sig);
    if (!m) {
        char msg[384];
        snprintf(msg, sizeof(msg), "%s.%s%s", c->name ? c->name : "?",
                 name, sig);
        bridge_set_pending_new(t->bridge, "java/lang/NoSuchMethodError", msg);
#ifdef LUNARIA_UNITY_PLAYER_PROBE
        if(!strcmp(name,"getDeclaringClass")||!strcmp(name,"getStackTraceString")){
            static unsigned nf;
            if(nf<8){nf++;FILE*f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt","a");
             if(f){fprintf(f,"METHOD_NOT_FOUND cls=%s name=%s sig=%s\n",c->name?c->name:"?",name,sig);fclose(f);}}
        }
#endif
        goto done;
    }
    if (dvm_method_is_static(m) != want_static) {
        char msg[384];
        snprintf(msg, sizeof(msg), "%s.%s%s static mismatch",
                 c->name ? c->name : "?", name, sig);
        bridge_set_pending_new(t->bridge, "java/lang/NoSuchMethodError", msg);
#ifdef LUNARIA_UNITY_PLAYER_PROBE
        if(!strcmp(name,"getDeclaringClass")||!strcmp(name,"getStackTraceString")){
            static unsigned sm;
            if(sm<8){sm++;FILE*f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt","a");
             if(f){fprintf(f,"METHOD_STATIC_MISMATCH cls=%s name=%s mstatic=%d want=%d\n",c->name?c->name:"?",name,dvm_method_is_static(m)?1:0,want_static?1:0);fclose(f);}}
        }
#endif
        goto done;
    }
    result = method_id_alloc(t->bridge, m, want_static);
    if (!result)
        bridge_set_pending_new(t->bridge, "java/lang/OutOfMemoryError",
                               "method ID table exhausted");
done:
#ifdef LUNARIA_UNITY_PLAYER_PROBE
    probe_jni_lookup(want_static?"static_method":"method",c?c->name:NULL,
                     name,sig,result!=NULL,t->pending);
#endif
    leave_slot(t, owned);
    return result;
}

static jmethodID JNICALL bridge_GetMethodID(JNIEnv *env, jclass clazz,
                                            const char *name, const char *sig)
{
    return method_lookup_common(env, clazz, name, sig, false);
}

static jmethodID JNICALL bridge_GetStaticMethodID(JNIEnv *env, jclass clazz,
                                                  const char *name,
                                                  const char *sig)
{
    return method_lookup_common(env, clazz, name, sig, true);
}

static jfieldID field_lookup_common(JNIEnv *env, jclass clazz,
                                    const char *name, const char *sig,
                                    bool want_static)
{
    struct jni_thread_env *t = NULL;
    bool owned = false;
    if (!enter_slot(env, &t, &owned)) return NULL;
    jfieldID result = NULL;
    struct dvm_class *c = NULL;
    struct dvm_class *decl = NULL;
    struct dvm_field *f = NULL;
    if (!name || !sig) {
        bridge_set_pending_new(t->bridge, "java/lang/NullPointerException",
                               "field name/signature is null");
        goto done;
    }
    if (!resolve_class_token(t->bridge, clazz, &c) || !c) {
        bridge_set_pending_new(t->bridge, "java/lang/NullPointerException",
                               "field lookup null class");
        goto done;
    }
    if (want_static)
        f = find_static_field_rec(c, name, sig, &decl, 0);
    else
        f = find_instance_field_rec(c, name, sig, &decl);
    if (want_static && !f && external_static_constant(c, name, sig, &f))
        decl = c;
    if (!f) {
        char msg[384];
        snprintf(msg, sizeof(msg), "%s.%s:%s", c->name ? c->name : "?",
                 name, sig);
        bridge_set_pending_new(t->bridge, "java/lang/NoSuchFieldError", msg);
        goto done;
    }
    result = field_id_alloc(t->bridge, f, decl && decl ? decl : c, want_static);
    if (!result)
        bridge_set_pending_new(t->bridge, "java/lang/OutOfMemoryError",
                               "field ID table exhausted");
done:
#ifdef LUNARIA_UNITY_PLAYER_PROBE
    probe_jni_lookup(want_static?"static_field":"field",c?c->name:NULL,
                     name,sig,result!=NULL,t->pending);
#endif
    leave_slot(t, owned);
    return result;
}

static jfieldID JNICALL bridge_GetFieldID(JNIEnv *env, jclass clazz,
                                          const char *name, const char *sig)
{
    return field_lookup_common(env, clazz, name, sig, false);
}

static jfieldID JNICALL bridge_GetStaticFieldID(JNIEnv *env, jclass clazz,
                                                const char *name,
                                                const char *sig)
{
    return field_lookup_common(env, clazz, name, sig, true);
}

/* ------------------------------------------------------------------ */
/* 实例字段访问                                                        */
/* ------------------------------------------------------------------ */

static jint JNICALL bridge_GetIntField(JNIEnv *env, jobject obj,
                                       jfieldID fieldID)
{
    struct jni_thread_env *t = NULL;
    bool owned = false;
    if (!enter_slot(env, &t, &owned)) return 0;
    jint result = 0;
    jni_field_id_slot *fs = field_id_slot(t->bridge, fieldID);
    dvm_ref ref = 0;
    union dvm_value value;
    if (!fs || fs->is_static) {
        bridge_set_pending_new(t->bridge, "java/lang/NoSuchFieldError",
                               "invalid instance int field ID");
        goto done;
    }
    if (fs->field->kind != 'I') {
        bridge_set_pending_new(t->bridge, "java/lang/IllegalArgumentException",
                               "field type is not int");
        goto done;
    }
    if (!object_arg(t, obj, &ref, "field access on null object")) goto done;
    if (!field_read_value(t, fs, ref, &value)) goto done;
    result = value.i;
done:
    leave_slot(t, owned);
    return result;
}

static jboolean JNICALL bridge_GetBooleanField(JNIEnv *env, jobject obj,
                                               jfieldID fieldID)
{
    struct jni_thread_env *t = NULL;
    bool owned = false;
    if (!enter_slot(env, &t, &owned)) return JNI_FALSE;
    jboolean result = JNI_FALSE;
    jni_field_id_slot *fs = field_id_slot(t->bridge, fieldID);
    dvm_ref ref = 0;
    union dvm_value value;
    if (!fs || fs->is_static || fs->field->kind != 'Z') {
        bridge_set_pending_new(t->bridge, "java/lang/NoSuchFieldError",
                               "invalid instance boolean field ID");
        goto done;
    }
    if (!object_arg(t, obj, &ref, "field access on null object")) goto done;
    if (!field_read_value(t, fs, ref, &value)) goto done;
    result = value.i ? JNI_TRUE : JNI_FALSE;
done:
    leave_slot(t, owned);
    return result;
}

static jbyte JNICALL bridge_GetByteField(JNIEnv *env, jobject obj,
                                         jfieldID fieldID)
{
    struct jni_thread_env *t = NULL;
    bool owned = false;
    if (!enter_slot(env, &t, &owned)) return 0;
    jbyte result = 0;
    jni_field_id_slot *fs = field_id_slot(t->bridge, fieldID);
    dvm_ref ref = 0;
    union dvm_value value;
    if (!fs || fs->is_static || fs->field->kind != 'B') {
        bridge_set_pending_new(t->bridge, "java/lang/NoSuchFieldError",
                               "invalid instance byte field ID");
        goto done;
    }
    if (!object_arg(t, obj, &ref, "field access on null object")) goto done;
    if (!field_read_value(t, fs, ref, &value)) goto done;
    result = (jbyte)value.i;
done:
    leave_slot(t, owned);
    return result;
}

static jchar JNICALL bridge_GetCharField(JNIEnv *env, jobject obj,
                                         jfieldID fieldID)
{
    struct jni_thread_env *t = NULL;
    bool owned = false;
    if (!enter_slot(env, &t, &owned)) return 0;
    jchar result = 0;
    jni_field_id_slot *fs = field_id_slot(t->bridge, fieldID);
    dvm_ref ref = 0;
    union dvm_value value;
    if (!fs || fs->is_static || fs->field->kind != 'C') {
        bridge_set_pending_new(t->bridge, "java/lang/NoSuchFieldError",
                               "invalid instance char field ID");
        goto done;
    }
    if (!object_arg(t, obj, &ref, "field access on null object")) goto done;
    if (!field_read_value(t, fs, ref, &value)) goto done;
    result = (jchar)value.i;
done:
    leave_slot(t, owned);
    return result;
}

static jshort JNICALL bridge_GetShortField(JNIEnv *env, jobject obj,
                                           jfieldID fieldID)
{
    struct jni_thread_env *t = NULL;
    bool owned = false;
    if (!enter_slot(env, &t, &owned)) return 0;
    jshort result = 0;
    jni_field_id_slot *fs = field_id_slot(t->bridge, fieldID);
    dvm_ref ref = 0;
    union dvm_value value;
    if (!fs || fs->is_static || fs->field->kind != 'S') {
        bridge_set_pending_new(t->bridge, "java/lang/NoSuchFieldError",
                               "invalid instance short field ID");
        goto done;
    }
    if (!object_arg(t, obj, &ref, "field access on null object")) goto done;
    if (!field_read_value(t, fs, ref, &value)) goto done;
    result = (jshort)value.i;
done:
    leave_slot(t, owned);
    return result;
}

static jlong JNICALL bridge_GetLongField(JNIEnv *env, jobject obj,
                                         jfieldID fieldID)
{
    struct jni_thread_env *t = NULL;
    bool owned = false;
    if (!enter_slot(env, &t, &owned)) return 0;
    jlong result = 0;
    jni_field_id_slot *fs = field_id_slot(t->bridge, fieldID);
    dvm_ref ref = 0;
    union dvm_value value;
    if (!fs || fs->is_static || fs->field->kind != 'J') {
        bridge_set_pending_new(t->bridge, "java/lang/NoSuchFieldError",
                               "invalid instance long field ID");
        goto done;
    }
    if (!object_arg(t, obj, &ref, "field access on null object")) goto done;
    if (!field_read_value(t, fs, ref, &value)) goto done;
    result = value.j;
done:
    leave_slot(t, owned);
    return result;
}

static jfloat JNICALL bridge_GetFloatField(JNIEnv *env, jobject obj,
                                           jfieldID fieldID)
{
    struct jni_thread_env *t = NULL;
    bool owned = false;
    if (!enter_slot(env, &t, &owned)) return 0.0f;
    jfloat result = 0.0f;
    jni_field_id_slot *fs = field_id_slot(t->bridge, fieldID);
    dvm_ref ref = 0;
    union dvm_value value;
    if (!fs || fs->is_static || fs->field->kind != 'F') {
        bridge_set_pending_new(t->bridge, "java/lang/NoSuchFieldError",
                               "invalid instance float field ID");
        goto done;
    }
    if (!object_arg(t, obj, &ref, "field access on null object")) goto done;
    if (!field_read_value(t, fs, ref, &value)) goto done;
    result = value.f;
done:
    leave_slot(t, owned);
    return result;
}

static jdouble JNICALL bridge_GetDoubleField(JNIEnv *env, jobject obj,
                                             jfieldID fieldID)
{
    struct jni_thread_env *t = NULL;
    bool owned = false;
    if (!enter_slot(env, &t, &owned)) return 0.0;
    jdouble result = 0.0;
    jni_field_id_slot *fs = field_id_slot(t->bridge, fieldID);
    dvm_ref ref = 0;
    union dvm_value value;
    if (!fs || fs->is_static || fs->field->kind != 'D') {
        bridge_set_pending_new(t->bridge, "java/lang/NoSuchFieldError",
                               "invalid instance double field ID");
        goto done;
    }
    if (!object_arg(t, obj, &ref, "field access on null object")) goto done;
    if (!field_read_value(t, fs, ref, &value)) goto done;
    result = value.d;
done:
    leave_slot(t, owned);
    return result;
}

static jobject JNICALL bridge_GetObjectField(JNIEnv *env, jobject obj,
                                             jfieldID fieldID)
{
    struct jni_thread_env *t = NULL;
    bool owned = false;
    if (!enter_slot(env, &t, &owned)) return NULL;
    jobject result = NULL;
    jni_field_id_slot *fs = field_id_slot(t->bridge, fieldID);
    dvm_ref ref = 0;
    union dvm_value value;
    if (!fs || fs->is_static || !field_is_ref(fs->field->kind)) {
        bridge_set_pending_new(t->bridge, "java/lang/NoSuchFieldError",
                               "invalid instance object field ID");
        goto done;
    }
    if (!object_arg(t, obj, &ref, "field access on null object")) goto done;
    if (!field_read_value(t, fs, ref, &value)) goto done;
    result = new_local_obj(t, value.l);
done:
    leave_slot(t, owned);
    return result;
}

static void JNICALL bridge_SetIntField(JNIEnv *env, jobject obj,
                                       jfieldID fieldID, jint val)
{
    struct jni_thread_env *t = NULL;
    bool owned = false;
    if (!enter_slot(env, &t, &owned)) return;
    jni_field_id_slot *fs = field_id_slot(t->bridge, fieldID);
    dvm_ref ref = 0;
    union dvm_value value;
    memset(&value, 0, sizeof(value));
    if (!fs || fs->is_static || fs->field->kind != 'I') {
        bridge_set_pending_new(t->bridge, "java/lang/NoSuchFieldError",
                               "invalid instance int field ID");
        goto done;
    }
    if (!object_arg(t, obj, &ref, "field access on null object")) goto done;
    value.i = val;
    (void)field_write_value(t, fs, ref, value);
done:
    leave_slot(t, owned);
}

static void JNICALL bridge_SetBooleanField(JNIEnv *env, jobject obj,
                                           jfieldID fieldID, jboolean val)
{
    struct jni_thread_env *t = NULL;
    bool owned = false;
    if (!enter_slot(env, &t, &owned)) return;
    jni_field_id_slot *fs = field_id_slot(t->bridge, fieldID);
    dvm_ref ref = 0;
    union dvm_value value;
    memset(&value, 0, sizeof(value));
    if (!fs || fs->is_static || fs->field->kind != 'Z') {
        bridge_set_pending_new(t->bridge, "java/lang/NoSuchFieldError",
                               "invalid instance boolean field ID");
        goto done;
    }
    if (!object_arg(t, obj, &ref, "field access on null object")) goto done;
    value.i = val ? 1 : 0;
    (void)field_write_value(t, fs, ref, value);
done:
    leave_slot(t, owned);
}

static void JNICALL bridge_SetByteField(JNIEnv *env, jobject obj,
                                        jfieldID fieldID, jbyte val)
{
    struct jni_thread_env *t = NULL;
    bool owned = false;
    if (!enter_slot(env, &t, &owned)) return;
    jni_field_id_slot *fs = field_id_slot(t->bridge, fieldID);
    dvm_ref ref = 0;
    union dvm_value value;
    memset(&value, 0, sizeof(value));
    if (!fs || fs->is_static || fs->field->kind != 'B') {
        bridge_set_pending_new(t->bridge, "java/lang/NoSuchFieldError",
                               "invalid instance byte field ID");
        goto done;
    }
    if (!object_arg(t, obj, &ref, "field access on null object")) goto done;
    value.i = (int32_t)val;
    (void)field_write_value(t, fs, ref, value);
done:
    leave_slot(t, owned);
}

static void JNICALL bridge_SetCharField(JNIEnv *env, jobject obj,
                                        jfieldID fieldID, jchar val)
{
    struct jni_thread_env *t = NULL;
    bool owned = false;
    if (!enter_slot(env, &t, &owned)) return;
    jni_field_id_slot *fs = field_id_slot(t->bridge, fieldID);
    dvm_ref ref = 0;
    union dvm_value value;
    memset(&value, 0, sizeof(value));
    if (!fs || fs->is_static || fs->field->kind != 'C') {
        bridge_set_pending_new(t->bridge, "java/lang/NoSuchFieldError",
                               "invalid instance char field ID");
        goto done;
    }
    if (!object_arg(t, obj, &ref, "field access on null object")) goto done;
    value.i = (int32_t)val;
    (void)field_write_value(t, fs, ref, value);
done:
    leave_slot(t, owned);
}

static void JNICALL bridge_SetShortField(JNIEnv *env, jobject obj,
                                         jfieldID fieldID, jshort val)
{
    struct jni_thread_env *t = NULL;
    bool owned = false;
    if (!enter_slot(env, &t, &owned)) return;
    jni_field_id_slot *fs = field_id_slot(t->bridge, fieldID);
    dvm_ref ref = 0;
    union dvm_value value;
    memset(&value, 0, sizeof(value));
    if (!fs || fs->is_static || fs->field->kind != 'S') {
        bridge_set_pending_new(t->bridge, "java/lang/NoSuchFieldError",
                               "invalid instance short field ID");
        goto done;
    }
    if (!object_arg(t, obj, &ref, "field access on null object")) goto done;
    value.i = (int32_t)val;
    (void)field_write_value(t, fs, ref, value);
done:
    leave_slot(t, owned);
}

static void JNICALL bridge_SetLongField(JNIEnv *env, jobject obj,
                                        jfieldID fieldID, jlong val)
{
    struct jni_thread_env *t = NULL;
    bool owned = false;
    if (!enter_slot(env, &t, &owned)) return;
    jni_field_id_slot *fs = field_id_slot(t->bridge, fieldID);
    dvm_ref ref = 0;
    union dvm_value value;
    memset(&value, 0, sizeof(value));
    if (!fs || fs->is_static || fs->field->kind != 'J') {
        bridge_set_pending_new(t->bridge, "java/lang/NoSuchFieldError",
                               "invalid instance long field ID");
        goto done;
    }
    if (!object_arg(t, obj, &ref, "field access on null object")) goto done;
    value.j = val;
    (void)field_write_value(t, fs, ref, value);
done:
    leave_slot(t, owned);
}

static void JNICALL bridge_SetFloatField(JNIEnv *env, jobject obj,
                                         jfieldID fieldID, jfloat val)
{
    struct jni_thread_env *t = NULL;
    bool owned = false;
    if (!enter_slot(env, &t, &owned)) return;
    jni_field_id_slot *fs = field_id_slot(t->bridge, fieldID);
    dvm_ref ref = 0;
    union dvm_value value;
    memset(&value, 0, sizeof(value));
    if (!fs || fs->is_static || fs->field->kind != 'F') {
        bridge_set_pending_new(t->bridge, "java/lang/NoSuchFieldError",
                               "invalid instance float field ID");
        goto done;
    }
    if (!object_arg(t, obj, &ref, "field access on null object")) goto done;
    value.f = val;
    (void)field_write_value(t, fs, ref, value);
done:
    leave_slot(t, owned);
}

static void JNICALL bridge_SetDoubleField(JNIEnv *env, jobject obj,
                                          jfieldID fieldID, jdouble val)
{
    struct jni_thread_env *t = NULL;
    bool owned = false;
    if (!enter_slot(env, &t, &owned)) return;
    jni_field_id_slot *fs = field_id_slot(t->bridge, fieldID);
    dvm_ref ref = 0;
    union dvm_value value;
    memset(&value, 0, sizeof(value));
    if (!fs || fs->is_static || fs->field->kind != 'D') {
        bridge_set_pending_new(t->bridge, "java/lang/NoSuchFieldError",
                               "invalid instance double field ID");
        goto done;
    }
    if (!object_arg(t, obj, &ref, "field access on null object")) goto done;
    value.d = val;
    (void)field_write_value(t, fs, ref, value);
done:
    leave_slot(t, owned);
}

static void JNICALL bridge_SetObjectField(JNIEnv *env, jobject obj,
                                          jfieldID fieldID, jobject val)
{
    struct jni_thread_env *t = NULL;
    bool owned = false;
    if (!enter_slot(env, &t, &owned)) return;
    jni_field_id_slot *fs = field_id_slot(t->bridge, fieldID);
    dvm_ref ref = 0;
    dvm_ref value_ref = 0;
    union dvm_value value;
    memset(&value, 0, sizeof(value));
    if (!fs || fs->is_static || !field_is_ref(fs->field->kind)) {
        bridge_set_pending_new(t->bridge, "java/lang/NoSuchFieldError",
                               "invalid instance object field ID");
        goto done;
    }
    if (!object_arg(t, obj, &ref, "field access on null object")) goto done;
    if (!token_to_dvm_ref(t, val, &value_ref)) {
        bridge_set_pending_new(t->bridge, "java/lang/IllegalArgumentException",
                               "SetObjectField invalid value");
        goto done;
    }
    value.l = value_ref;
    (void)field_write_value(t, fs, ref, value);
done:
    leave_slot(t, owned);
}

/* ------------------------------------------------------------------ */
/* 静态字段访问                                                        */
/* ------------------------------------------------------------------ */

static bool static_class_arg(struct jni_thread_env *t, jclass clazz,
                             struct dvm_class **out)
{
    if (!resolve_class_token(t->bridge, clazz, out) || !*out) {
        bridge_set_pending_new(t->bridge, "java/lang/NullPointerException",
                               "static field null class");
        return false;
    }
    return true;
}

static jint JNICALL bridge_GetStaticIntField(JNIEnv *env, jclass clazz,
                                             jfieldID fieldID)
{
    struct jni_thread_env *t = NULL;
    bool owned = false;
    if (!enter_slot(env, &t, &owned)) return 0;
    jint result = 0;
    struct dvm_class *c = NULL;
    jni_field_id_slot *fs = field_id_slot(t->bridge, fieldID);
    union dvm_value value;
    if (!static_class_arg(t, clazz, &c)) goto done;
    if (!fs || !fs->is_static || fs->field->kind != 'I') {
        bridge_set_pending_new(t->bridge, "java/lang/NoSuchFieldError",
                               "invalid static int field ID");
        goto done;
    }
    if (!field_read_value(t, fs, 0, &value)) goto done;
    result = value.i;
done:
    leave_slot(t, owned);
    return result;
}

static jboolean JNICALL bridge_GetStaticBooleanField(JNIEnv *env, jclass clazz,
                                                     jfieldID fieldID)
{
    struct jni_thread_env *t = NULL;
    bool owned = false;
    if (!enter_slot(env, &t, &owned)) return JNI_FALSE;
    jboolean result = JNI_FALSE;
    struct dvm_class *c = NULL;
    jni_field_id_slot *fs = field_id_slot(t->bridge, fieldID);
    union dvm_value value;
    if (!static_class_arg(t, clazz, &c)) goto done;
    if (!fs || !fs->is_static || fs->field->kind != 'Z') {
        bridge_set_pending_new(t->bridge, "java/lang/NoSuchFieldError",
                               "invalid static boolean field ID");
        goto done;
    }
    if (!field_read_value(t, fs, 0, &value)) goto done;
    result = value.i ? JNI_TRUE : JNI_FALSE;
done:
    leave_slot(t, owned);
    return result;
}

static jbyte JNICALL bridge_GetStaticByteField(JNIEnv *env, jclass clazz,
                                               jfieldID fieldID)
{
    struct jni_thread_env *t = NULL;
    bool owned = false;
    if (!enter_slot(env, &t, &owned)) return 0;
    jbyte result = 0;
    struct dvm_class *c = NULL;
    jni_field_id_slot *fs = field_id_slot(t->bridge, fieldID);
    union dvm_value value;
    if (!static_class_arg(t, clazz, &c)) goto done;
    if (!fs || !fs->is_static || fs->field->kind != 'B') {
        bridge_set_pending_new(t->bridge, "java/lang/NoSuchFieldError",
                               "invalid static byte field ID");
        goto done;
    }
    if (!field_read_value(t, fs, 0, &value)) goto done;
    result = (jbyte)value.i;
done:
    leave_slot(t, owned);
    return result;
}

static jchar JNICALL bridge_GetStaticCharField(JNIEnv *env, jclass clazz,
                                               jfieldID fieldID)
{
    struct jni_thread_env *t = NULL;
    bool owned = false;
    if (!enter_slot(env, &t, &owned)) return 0;
    jchar result = 0;
    struct dvm_class *c = NULL;
    jni_field_id_slot *fs = field_id_slot(t->bridge, fieldID);
    union dvm_value value;
    if (!static_class_arg(t, clazz, &c)) goto done;
    if (!fs || !fs->is_static || fs->field->kind != 'C') {
        bridge_set_pending_new(t->bridge, "java/lang/NoSuchFieldError",
                               "invalid static char field ID");
        goto done;
    }
    if (!field_read_value(t, fs, 0, &value)) goto done;
    result = (jchar)value.i;
done:
    leave_slot(t, owned);
    return result;
}

static jshort JNICALL bridge_GetStaticShortField(JNIEnv *env, jclass clazz,
                                                 jfieldID fieldID)
{
    struct jni_thread_env *t = NULL;
    bool owned = false;
    if (!enter_slot(env, &t, &owned)) return 0;
    jshort result = 0;
    struct dvm_class *c = NULL;
    jni_field_id_slot *fs = field_id_slot(t->bridge, fieldID);
    union dvm_value value;
    if (!static_class_arg(t, clazz, &c)) goto done;
    if (!fs || !fs->is_static || fs->field->kind != 'S') {
        bridge_set_pending_new(t->bridge, "java/lang/NoSuchFieldError",
                               "invalid static short field ID");
        goto done;
    }
    if (!field_read_value(t, fs, 0, &value)) goto done;
    result = (jshort)value.i;
done:
    leave_slot(t, owned);
    return result;
}

static jlong JNICALL bridge_GetStaticLongField(JNIEnv *env, jclass clazz,
                                               jfieldID fieldID)
{
    struct jni_thread_env *t = NULL;
    bool owned = false;
    if (!enter_slot(env, &t, &owned)) return 0;
    jlong result = 0;
    struct dvm_class *c = NULL;
    jni_field_id_slot *fs = field_id_slot(t->bridge, fieldID);
    union dvm_value value;
    if (!static_class_arg(t, clazz, &c)) goto done;
    if (!fs || !fs->is_static || fs->field->kind != 'J') {
        bridge_set_pending_new(t->bridge, "java/lang/NoSuchFieldError",
                               "invalid static long field ID");
        goto done;
    }
    if (!field_read_value(t, fs, 0, &value)) goto done;
    result = value.j;
done:
    leave_slot(t, owned);
    return result;
}

static jfloat JNICALL bridge_GetStaticFloatField(JNIEnv *env, jclass clazz,
                                                 jfieldID fieldID)
{
    struct jni_thread_env *t = NULL;
    bool owned = false;
    if (!enter_slot(env, &t, &owned)) return 0.0f;
    jfloat result = 0.0f;
    struct dvm_class *c = NULL;
    jni_field_id_slot *fs = field_id_slot(t->bridge, fieldID);
    union dvm_value value;
    if (!static_class_arg(t, clazz, &c)) goto done;
    if (!fs || !fs->is_static || fs->field->kind != 'F') {
        bridge_set_pending_new(t->bridge, "java/lang/NoSuchFieldError",
                               "invalid static float field ID");
        goto done;
    }
    if (!field_read_value(t, fs, 0, &value)) goto done;
    result = value.f;
done:
    leave_slot(t, owned);
    return result;
}

static jdouble JNICALL bridge_GetStaticDoubleField(JNIEnv *env, jclass clazz,
                                                   jfieldID fieldID)
{
    struct jni_thread_env *t = NULL;
    bool owned = false;
    if (!enter_slot(env, &t, &owned)) return 0.0;
    jdouble result = 0.0;
    struct dvm_class *c = NULL;
    jni_field_id_slot *fs = field_id_slot(t->bridge, fieldID);
    union dvm_value value;
    if (!static_class_arg(t, clazz, &c)) goto done;
    if (!fs || !fs->is_static || fs->field->kind != 'D') {
        bridge_set_pending_new(t->bridge, "java/lang/NoSuchFieldError",
                               "invalid static double field ID");
        goto done;
    }
    if (!field_read_value(t, fs, 0, &value)) goto done;
    result = value.d;
done:
    leave_slot(t, owned);
    return result;
}

static jobject JNICALL bridge_GetStaticObjectField(JNIEnv *env, jclass clazz,
                                                   jfieldID fieldID)
{
    struct jni_thread_env *t = NULL;
    bool owned = false;
    if (!enter_slot(env, &t, &owned)) return NULL;
    jobject result = NULL;
    struct dvm_class *c = NULL;
    jni_field_id_slot *fs = field_id_slot(t->bridge, fieldID);
    union dvm_value value;
    if (!static_class_arg(t, clazz, &c)) goto done;
    if (!fs || !fs->is_static || !field_is_ref(fs->field->kind)) {
        bridge_set_pending_new(t->bridge, "java/lang/NoSuchFieldError",
                               "invalid static object field ID");
        goto done;
    }
    if (!field_read_value(t, fs, 0, &value)) goto done;
    result = new_local_obj(t, value.l);
done:
    leave_slot(t, owned);
    return result;
}

#define DEFINE_STATIC_FIELD_SETTER(NAME, CTYPE, KINDCHAR, FIELDF)             \
static void JNICALL bridge_SetStatic##NAME##Field(JNIEnv *env, jclass clazz,  \
                                                  jfieldID fieldID,           \
                                                  CTYPE val)                  \
{                                                                            \
    struct jni_thread_env *t = NULL;                                         \
    bool owned = false;                                                      \
    if (!enter_slot(env, &t, &owned)) return;                                \
    struct dvm_class *c = NULL;                                              \
    jni_field_id_slot *fs = field_id_slot(t->bridge, fieldID);               \
    union dvm_value value;                                                   \
    memset(&value, 0, sizeof(value));                                        \
    if (!static_class_arg(t, clazz, &c)) goto done;                          \
    if (!fs || !fs->is_static || fs->field->kind != KINDCHAR) {              \
        bridge_set_pending_new(t->bridge, "java/lang/NoSuchFieldError",      \
                               "invalid static field ID");                   \
        goto done;                                                           \
    }                                                                        \
    value.FIELDF = val;                                                      \
    (void)field_write_value(t, fs, 0, value);                                \
done:                                                                        \
    leave_slot(t, owned);                                                    \
}

DEFINE_STATIC_FIELD_SETTER(Boolean, jboolean, 'Z', i)
DEFINE_STATIC_FIELD_SETTER(Byte, jbyte, 'B', i)
DEFINE_STATIC_FIELD_SETTER(Char, jchar, 'C', i)
DEFINE_STATIC_FIELD_SETTER(Short, jshort, 'S', i)
DEFINE_STATIC_FIELD_SETTER(Int, jint, 'I', i)
DEFINE_STATIC_FIELD_SETTER(Long, jlong, 'J', j)
DEFINE_STATIC_FIELD_SETTER(Float, jfloat, 'F', f)
DEFINE_STATIC_FIELD_SETTER(Double, jdouble, 'D', d)
#undef DEFINE_STATIC_FIELD_SETTER

static void JNICALL bridge_SetStaticObjectField(JNIEnv *env, jclass clazz,
                                                jfieldID fieldID, jobject val)
{
    struct jni_thread_env *t = NULL;
    bool owned = false;
    if (!enter_slot(env, &t, &owned)) return;
    struct dvm_class *c = NULL;
    jni_field_id_slot *fs = field_id_slot(t->bridge, fieldID);
    union dvm_value value;
    memset(&value, 0, sizeof(value));
    if (!static_class_arg(t, clazz, &c)) goto done;
    if (!fs || !fs->is_static || !field_is_ref(fs->field->kind)) {
        bridge_set_pending_new(t->bridge, "java/lang/NoSuchFieldError",
                               "invalid static object field ID");
        goto done;
    }
    if (!token_to_dvm_ref(t, val, &value.l)) {
        bridge_set_pending_new(t->bridge, "java/lang/IllegalArgumentException",
                               "SetStaticObjectField invalid value");
        goto done;
    }
    (void)field_write_value(t, fs, 0, value);
done:
    leave_slot(t, owned);
}

/* ------------------------------------------------------------------ */
/* 方法调用参数收集与公共路径                                          */
/* ------------------------------------------------------------------ */

struct jni_call_source {
    const jvalue *values; /* A 变体 */
    va_list *list;        /* 普通/V 变体 */
};

static bool collect_call_args(struct jni_thread_env *t,
                              const jni_sig_info *info,
                              const struct jni_call_source *src,
                              union dvm_value *values)
{
    if (!t || !info || !src || !values) return false;
    if (info->nargs > 0 && !src->values && !src->list) {
        bridge_set_pending_new(t->bridge, "java/lang/IllegalArgumentException",
                               "method arguments are missing");
        return false;
    }
    for (int i = 0; i < info->nargs; ++i) {
        jni_kind kind = info->args[i].kind;
        jvalue jv;
        memset(&jv, 0, sizeof(jv));
        memset(&values[i], 0, sizeof(values[i]));
        if (kind == JNI_KIND_VOID || kind == JNI_KIND_INVALID) {
            bridge_set_pending_new(t->bridge, "java/lang/IllegalArgumentException",
                                   "void method argument");
            return false;
        }
        if (src->list) {
            va_list *ap = src->list;
            switch (kind) {
            case JNI_KIND_BOOLEAN:
                jv.z = (jboolean)(va_arg(*ap, int) ? JNI_TRUE : JNI_FALSE);
                break;
            case JNI_KIND_BYTE:
                jv.b = (jbyte)va_arg(*ap, int);
                break;
            case JNI_KIND_CHAR:
                jv.c = (jchar)va_arg(*ap, int);
                break;
            case JNI_KIND_SHORT:
                jv.s = (jshort)va_arg(*ap, int);
                break;
            case JNI_KIND_INT:
                jv.i = va_arg(*ap, jint);
                break;
            case JNI_KIND_LONG:
                jv.j = va_arg(*ap, jlong);
                break;
            case JNI_KIND_FLOAT:
                jv.f = (jfloat)va_arg(*ap, double);
                break;
            case JNI_KIND_DOUBLE:
                jv.d = va_arg(*ap, double);
                break;
            case JNI_KIND_OBJECT:
            case JNI_KIND_ARRAY:
                jv.l = va_arg(*ap, jobject);
                break;
            default:
                break;
            }
        } else {
            jv = src->values[i];
        }
        switch (kind) {
        case JNI_KIND_BOOLEAN:
            values[i].i = jv.z ? 1 : 0;
            break;
        case JNI_KIND_BYTE:
            values[i].i = (int32_t)jv.b;
            break;
        case JNI_KIND_CHAR:
            values[i].i = (int32_t)jv.c;
            break;
        case JNI_KIND_SHORT:
            values[i].i = (int32_t)jv.s;
            break;
        case JNI_KIND_INT:
            values[i].i = jv.i;
            break;
        case JNI_KIND_LONG:
            values[i].j = jv.j;
            break;
        case JNI_KIND_FLOAT:
            values[i].f = jv.f;
            break;
        case JNI_KIND_DOUBLE:
            values[i].d = jv.d;
            break;
        case JNI_KIND_OBJECT:
        case JNI_KIND_ARRAY: {
            dvm_ref ref = 0;
            if (!token_to_dvm_ref(t, jv.l, &ref)) {
                bridge_set_pending_new(t->bridge,
                                       "java/lang/IllegalArgumentException",
                                       "invalid object argument");
                return false;
            }
            values[i].l = ref;
            break;
        }
        default:
            break;
        }
    }
    return true;
}

static bool return_kind_matches(jni_kind want, jni_kind got)
{
    if (want == JNI_KIND_OBJECT)
        return got == JNI_KIND_OBJECT || got == JNI_KIND_ARRAY;
    return want == got;
}

static bool convert_return_value(struct jni_thread_env *t, jni_kind want,
                                 const jni_sig_info *info,
                                 const union dvm_value *ret, jvalue *out)
{
    if (!t || !info || !ret || !out) return false;
    if (!return_kind_matches(want, info->ret)) {
        char msg[192];
        snprintf(msg, sizeof(msg), "method returns %s but caller expects %s",
                 jni_kind_name(info->ret), jni_kind_name(want));
        bridge_set_pending_new(t->bridge, "java/lang/IllegalArgumentException",
                               msg);
        return false;
    }
    switch (want) {
    case JNI_KIND_VOID:
        break;
    case JNI_KIND_BOOLEAN:
        out->z = ret->i ? JNI_TRUE : JNI_FALSE;
        break;
    case JNI_KIND_BYTE:
        out->b = (jbyte)ret->i;
        break;
    case JNI_KIND_CHAR:
        out->c = (jchar)ret->i;
        break;
    case JNI_KIND_SHORT:
        out->s = (jshort)ret->i;
        break;
    case JNI_KIND_INT:
        out->i = ret->i;
        break;
    case JNI_KIND_LONG:
        out->j = ret->j;
        break;
    case JNI_KIND_FLOAT:
        out->f = ret->f;
        break;
    case JNI_KIND_DOUBLE:
        out->d = ret->d;
        break;
    case JNI_KIND_OBJECT:
    case JNI_KIND_ARRAY:
        out->l = new_local_obj(t, ret->l);
        break;
    default:
        break;
    }
    return true;
}

static void touch_jni_trace(struct jni_thread_env *t,jmethodID mid,const char *stage,int result){
#ifdef LUNARIA_UNITY_PLAYER_PROBE
    if(!t)return;
    jni_method_id_slot *slot=method_id_slot(t->bridge,mid);
    struct dvm_method *m=slot?slot->method:NULL;
    if(!m||!m->name||!m->cls||
       (strcmp(m->cls->name,"android/view/MotionEvent")&&strcmp(m->cls->name,"android/view/InputEvent")))return;
    static atomic_uint count;
    if(atomic_fetch_add(&count,1)>=256)return;
    int saved=errno;FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt","a");
    if(f){fprintf(f,"TOUCH_JNI stage=%s class=%s name=%s sig=%s pending=%u vm_exception=%u result=%d pthread=%llu\n",stage,m->cls?m->cls->name:"?",m->name,m->sig?m->sig:"?",(unsigned)t->pending,dvm_exception(t->bridge->vm),result,(unsigned long long)(uintptr_t)pthread_self());fclose(f);}errno=saved;
#else
    (void)t;(void)mid;(void)stage;(void)result;
#endif
}
static jvalue invoke_common(struct jni_thread_env *t, jobject obj,
                            jclass clazz, jmethodID mid,
                            const struct jni_call_source *src,
                            bool is_static, bool nonvirtual,
                            jni_kind want)
{
    jvalue result = zero_value();
    touch_jni_trace(t,mid,"entry",0);
    if (!t || t->pending) return result;
    jni_method_id_slot *slot = method_id_slot(t->bridge, mid);
    if (!slot) {
        bridge_set_pending_new(t->bridge, "java/lang/NoSuchMethodError",
                               "invalid jmethodID");
        return result;
    }
    if (slot->is_static != is_static) {
        bridge_set_pending_new(t->bridge, "java/lang/NoSuchMethodError",
                               "jmethodID static/instance mismatch");
        return result;
    }
    struct dvm_method *m = slot->method;
    if (!m || !m->sig) {
        bridge_set_pending_new(t->bridge, "java/lang/NoSuchMethodError",
                               "jmethodID has no method");
        return result;
    }
    jni_sig_info info;
    memset(&info, 0, sizeof(info));
    if (!jni_sig_parse(m->sig, &info) ||
        info.nargs > JNI_MAX_SIG_ARGS) {
        bridge_set_pending_new(t->bridge, "java/lang/NoSuchMethodError",
                               "invalid method signature");
        return result;
    }
    union dvm_value values[JNI_MAX_SIG_ARGS];
    memset(values, 0, sizeof(values));
    if (!collect_call_args(t, &info, src, values)) return result;

    struct dvm_method *target = m;
    dvm_ref self = 0;
    if (is_static) {
        struct dvm_class *c = NULL;
        if (!clazz || !resolve_class_token(t->bridge, clazz, &c) || !c) {
            bridge_set_pending_new(t->bridge, "java/lang/NullPointerException",
                                   "static call with null class");
            return result;
        }
        if (!dvm__class_assignable(t->bridge->vm, c, m->cls)) {
            bridge_set_pending_new(t->bridge, "java/lang/NoSuchMethodError",
                                   "static method class mismatch");
            return result;
        }
        if (!dvm_init_class(t->bridge->vm, m->cls)) {
            if (!move_dvm_exception_to_pending(t))
                bridge_set_pending_new(t->bridge,
                                       "java/lang/ExceptionInInitializerError",
                                       "class initialization failed");
            return result;
        }
    } else {
        if (!object_arg(t, obj, &self, "method call on null object"))
            return result;
        if (nonvirtual) {
            struct dvm_class *c = NULL;
            if (!clazz || !resolve_class_token(t->bridge, clazz, &c) || !c) {
                bridge_set_pending_new(t->bridge,
                                       "java/lang/NullPointerException",
                                       "nonvirtual call with null class");
                return result;
            }
            if (!dvm__class_assignable(t->bridge->vm, c, m->cls)) {
                bridge_set_pending_new(t->bridge,
                                       "java/lang/NoSuchMethodError",
                                       "nonvirtual class mismatch");
                return result;
            }
        } else {
            struct dvm_class *actual = dvm_object_class(t->bridge->vm, self);
            if (!actual) {
                bridge_set_pending_new(t->bridge,
                                       "java/lang/NullPointerException",
                                       "cannot resolve receiver class");
                return result;
            }
            target = dvm_find_method(t->bridge->vm, actual,
                                     dvm_method_name(m), dvm_method_sig(m));
            if (!target || dvm_method_is_static(target)) {
                bridge_set_pending_new(t->bridge,
                                       "java/lang/NoSuchMethodError",
                                       dvm_method_name(m));
                return result;
            }
        }
    }

    /* 异常构造使用保留内存；其真实 toString 和 JNI 编码也必须使用同一
     * 有界保留区，否则普通 arena 满后会静默返回 NULL 给引擎异常报告器。
     * 不替换内容，不放宽总内存上限，不对普通 String 启用保留区。 */
    bool throwable_text = false;
    if (!is_static && want == JNI_KIND_OBJECT &&
        !strcmp(m->name, "toString") && !strcmp(m->sig, "()Ljava/lang/String;")) {
        for (struct dvm_class *c = dvm_object_class(t->bridge->vm, self); c; c = c->super)
            if (!strcmp(c->name, "java/lang/Throwable")) { throwable_text = true; break; }
    }
    extern void vm_arena_emergency_enter(void), vm_arena_emergency_leave(void);
    extern size_t vm_arena_used(void), vm_arena_limit(void), vm_arena_denied(void);
    if (throwable_text) vm_arena_emergency_enter();
    union dvm_value ret;
    memset(&ret, 0, sizeof(ret));
    bool network_query = network_runtime_query_begin(t->bridge->vm,
        target->cls ? target->cls->name : "", target->name, target->sig);
    if (!dvm_call(t->bridge->vm, target, self, values, info.nargs, &ret)) {
        if (network_query) network_runtime_query_failure(t->bridge->vm,"getter_dvm_call_failed");
        if (network_query) network_runtime_query_end(0,false);
        if (!move_dvm_exception_to_pending(t))
            bridge_set_pending_new(t->bridge, "java/lang/InternalError",
                                   dvm_method_name(target));
        if (throwable_text) vm_arena_emergency_leave();
        return result;
    }
    if (network_query) network_runtime_query_end(ret.i,true);
    if (target->cls && !strcmp(target->cls->name,"com/unity/androidnotifications/UnityNotificationManager") &&
        !strcmp(target->name,"getNotificationManagerImpl")) {
        FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/jni-sweep-notifications.txt","a");
        if(f){fprintf(f,"pid=%d method=%s%s original_DEX_return=%u nonnull=%d exception=%u synthetic_manager=0\n",getpid(),target->name,target->sig,ret.l,ret.l!=0,dvm_exception(t->bridge->vm));fclose(f);}
    }
    if (!convert_return_value(t, want, &info, &ret, &result))
        memset(&result, 0, sizeof(result));
    if (throwable_text) {
        struct jni_utf_cache *prior = result.l ? utf_cache_find(t, (jstring)result.l) : NULL;
        bool unborrowed = !prior || prior->prepared_only;
        struct jni_utf_cache *cache = result.l ? string_modified_cache(t, (jstring)result.l) : NULL;
        if (cache && unborrowed) cache->prepared_only = true;
        FILE *f = fopen("/data/storage/el2/base/haps/entry/files/sdkdex/strncpy-trace.txt", "a");
        if (f) {
            fprintf(f, "JNI_THROWABLE_TEXT pid=%d tid=%ld receiver=%u result=%u utf=%p pending=%u arena_used=%zu arena_limit=%zu denied=%zu\n",
                    getpid(), (long)syscall(SYS_gettid), self, ret.l,
                    cache ? (void *)cache->modified : NULL, t->pending,
                    vm_arena_used(), vm_arena_limit(), vm_arena_denied());
            extern void dvm_lifetime_signal_dump(int);
            dvm_lifetime_signal_dump(fileno(f));
            fclose(f);
        }
        vm_arena_emergency_leave();
    }
    touch_jni_trace(t,mid,"returned",result.i);
#ifdef LUNARIA_UNITY_PLAYER_PROBE
    /* 游戏 Java 桥（com.bilibili.star.bili.UnityAgent）的方法返回值诊断：
     * 托管层在其后抛 NullReferenceException，需要区分“方法返回 null”与
     * “调用本身失败”。只记录该类的调用，不影响其它方法。 */
    if (slot->decl && slot->decl->name &&
        !strcmp(slot->decl->name, "com/bilibili/star/bili/UnityAgent")) {
        static unsigned agent_probe;
        if (agent_probe < 24) {
            agent_probe++;
            bool is_null = want == JNI_KIND_OBJECT && jni_token_is_null(result.l);
            FILE *f = fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt", "a");
            if (f) {
                fprintf(f, "UNITY_AGENT_CALL method=%s static=%d want=%d null=%d pending=%u\n",
                        m->name, is_static ? 1 : 0, (int)want, is_null ? 1 : 0,
                        (unsigned)t->pending);
                if(!is_static&&!strcmp(m->name,"initGameSDK")){
                    struct dvm *vm=t->bridge->vm;union dvm_value activity={0};int found=0;
                    for(struct dvm_class *c=dvm_object_class(vm,self);c&&!found;c=c->super){
                        for(int j=0;j<c->nifields;j++){
                            struct dvm_field *field=&c->ifields[j];
                            if(field->name&&field->type&&!strcmp(field->name,"m_activity")){
                                found=dvm__get_field_from(vm,self,c,field->name,field->type,&activity);break;
                            }
                        }
                    }
                    struct dvm_class *ac=activity.l?dvm_object_class(vm,activity.l):NULL;
                    fprintf(f,"UNITY_AGENT_STATE method=initGameSDK receiver=%u activity_field_found=%d activity=%u activity_class=%s pending=%u readonly=1\n",self,found,activity.l,ac&&ac->name?ac->name:"-",(unsigned)t->pending);
                    unsigned logged=0;
                    for(struct dvm_class *c=ac;c&&logged<8;c=c->super){
                        for(int j=0;j<c->nifields&&logged<8;j++){
                            struct dvm_field *field=&c->ifields[j];
                            if(!field->name||!field->type||(!strstr(field->name,"sdk")&&!strstr(field->name,"Sdk")&&!strstr(field->name,"SDK")))continue;
                            if(field->type[0]!='L'&&field->type[0]!='[')continue;
                            union dvm_value sdk={0};int ok=dvm__get_field_from(vm,activity.l,c,field->name,field->type,&sdk);
                            struct dvm_class *sc=sdk.l?dvm_object_class(vm,sdk.l):NULL;
                            fprintf(f,"UNITY_AGENT_SDK_FIELD name=%s read=%d ref=%u class=%s readonly=1\n",field->name,ok,sdk.l,sc&&sc->name?sc->name:"-");logged++;
                        }
                    }
                }
                fclose(f);
            }
        }
    }
#endif
    return result;
}

static jobject construct_common(struct jni_thread_env *t, jclass clazz,
                                jmethodID mid,
                                const struct jni_call_source *src)
{
    if (!t || t->pending) return NULL;
    jni_method_id_slot *slot = method_id_slot(t->bridge, mid);
    if (!slot || slot->is_static) {
        bridge_set_pending_new(t->bridge, "java/lang/NoSuchMethodError",
                               "invalid constructor ID");
        return NULL;
    }
    struct dvm_method *m = slot->method;
    if (!m || !m->name || strcmp(m->name, "<init>") != 0) {
        bridge_set_pending_new(t->bridge, "java/lang/NoSuchMethodError",
                               "NewObject requires a constructor ID");
        return NULL;
    }
    struct dvm_class *c = NULL;
    if (!clazz || !resolve_class_token(t->bridge, clazz, &c) || !c) {
        bridge_set_pending_new(t->bridge, "java/lang/NullPointerException",
                               "NewObject null class");
        return NULL;
    }
    if (m->cls != c) {
        bridge_set_pending_new(t->bridge, "java/lang/NoSuchMethodError",
                               "constructor does not belong to class");
        return NULL;
    }
    jni_sig_info info;
    memset(&info, 0, sizeof(info));
    if (!jni_sig_parse(m->sig, &info) || info.ret != JNI_KIND_VOID ||
        info.nargs > JNI_MAX_SIG_ARGS) {
        bridge_set_pending_new(t->bridge, "java/lang/NoSuchMethodError",
                               "invalid constructor signature");
        return NULL;
    }
    union dvm_value values[JNI_MAX_SIG_ARGS];
    memset(values, 0, sizeof(values));
    if (!collect_call_args(t, &info, src, values)) return NULL;
    if (!dvm_init_class(t->bridge->vm, c)) {
        if (!move_dvm_exception_to_pending(t))
            bridge_set_pending_new(t->bridge,
                                   "java/lang/ExceptionInInitializerError",
                                   "constructor class initialization failed");
        return NULL;
    }
    dvm_ref self = dvm_new_object(t->bridge->vm, c);
    if (!self) {
        if (!move_dvm_exception_to_pending(t))
            bridge_set_pending_new(t->bridge, "java/lang/OutOfMemoryError",
                                   "NewObject allocation failed");
        return NULL;
    }
    union dvm_value ret;
    memset(&ret, 0, sizeof(ret));
    if (!dvm_call(t->bridge->vm, m, self, values, info.nargs, &ret)) {
        if (!move_dvm_exception_to_pending(t))
            bridge_set_pending_new(t->bridge, "java/lang/InternalError",
                                   "constructor invocation failed");
        return NULL;
    }
    jobject result = new_local_obj(t, self);
    if (jni_token_is_null(result))
        bridge_set_pending_new(t->bridge, "java/lang/OutOfMemoryError",
                               "constructor result reference failed");
#ifdef LUNARIA_UNITY_PLAYER_PROBE
    if (m->cls && m->cls->name &&
        !strcmp(m->cls->name, "com/bilibili/star/bili/UnityAgent")) {
        static unsigned agent_new_probe;
        if (agent_new_probe < 8) {
            agent_new_probe++;
            FILE *f = fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt", "a");
            if (f) {
                fprintf(f, "UNITY_AGENT_NEW ok=%d pending=%u\n",
                        jni_token_is_null(result) ? 0 : 1, (unsigned)t->pending);
                fclose(f);
            }
        }
    }
#endif
    return result;
}

static jobject JNICALL bridge_NewObject(JNIEnv *env, jclass clazz,
                                        jmethodID mid, ...)
{
    struct jni_thread_env *t = NULL;
    bool owned = false;
    if (!enter_slot(env, &t, &owned)) return NULL;
    struct jni_call_source src;
    src.values = NULL;
    va_list ap;
    va_start(ap, mid);
    src.list = &ap;
    jobject result = construct_common(t, clazz, mid, &src);
    va_end(ap);
    leave_slot(t, owned);
    return result;
}

static jobject JNICALL bridge_NewObjectV(JNIEnv *env, jclass clazz,
                                         jmethodID mid, va_list args)
{
    struct jni_thread_env *t = NULL;
    bool owned = false;
    if (!enter_slot(env, &t, &owned)) return NULL;
    struct jni_call_source src;
    src.values = NULL;
    va_list copy;
    va_copy(copy, args);
    src.list = &copy;
    jobject result = construct_common(t, clazz, mid, &src);
    va_end(copy);
    leave_slot(t, owned);
    return result;
}

static jobject JNICALL bridge_NewObjectA(JNIEnv *env, jclass clazz,
                                         jmethodID mid, const jvalue *args)
{
    struct jni_thread_env *t = NULL;
    bool owned = false;
    if (!enter_slot(env, &t, &owned)) return NULL;
    struct jni_call_source src;
    src.values = args;
    src.list = NULL;
    jobject result = construct_common(t, clazz, mid, &src);
    leave_slot(t, owned);
    return result;
}

/* ------------------------------------------------------------------ */
/* Call<Type>Method 三族                                                */
/* ------------------------------------------------------------------ */

#define DEFINE_INSTANCE_CALLS(NAME, CTYPE, FIELD, KIND)                      \
static CTYPE JNICALL bridge_Call##NAME##Method(JNIEnv *env, jobject obj,     \
                                               jmethodID mid, ...)           \
{                                                                            \
    struct jni_thread_env *t = NULL;                                         \
    bool owned = false;                                                      \
    CTYPE result = (CTYPE)0;                                                 \
    if (!enter_slot(env, &t, &owned)) return result;                         \
    struct jni_call_source src;                                              \
    src.values = NULL;                                                       \
    va_list ap;                                                              \
    va_start(ap, mid);                                                       \
    src.list = &ap;                                                          \
    jvalue v = invoke_common(t, obj, NULL, mid, &src, false, false, KIND);   \
    va_end(ap);                                                              \
    result = (CTYPE)v.FIELD;                                                 \
    leave_slot(t, owned);                                                    \
    return result;                                                           \
}                                                                            \
static CTYPE JNICALL bridge_Call##NAME##MethodV(JNIEnv *env, jobject obj,    \
                                                jmethodID mid, va_list args) \
{                                                                            \
    struct jni_thread_env *t = NULL;                                         \
    bool owned = false;                                                      \
    CTYPE result = (CTYPE)0;                                                 \
    if (!enter_slot(env, &t, &owned)) return result;                         \
    struct jni_call_source src;                                              \
    src.values = NULL;                                                       \
    va_list copy;                                                            \
    va_copy(copy, args);                                                     \
    src.list = &copy;                                                        \
    jvalue v = invoke_common(t, obj, NULL, mid, &src, false, false, KIND);   \
    va_end(copy);                                                            \
    result = (CTYPE)v.FIELD;                                                 \
    leave_slot(t, owned);                                                    \
    return result;                                                           \
}                                                                            \
static CTYPE JNICALL bridge_Call##NAME##MethodA(JNIEnv *env, jobject obj,    \
                                                jmethodID mid,               \
                                                const jvalue *args)          \
{                                                                            \
    struct jni_thread_env *t = NULL;                                         \
    bool owned = false;                                                      \
    CTYPE result = (CTYPE)0;                                                 \
    if (!enter_slot(env, &t, &owned)) return result;                         \
    struct jni_call_source src;                                              \
    src.values = args;                                                       \
    src.list = NULL;                                                         \
    jvalue v = invoke_common(t, obj, NULL, mid, &src, false, false, KIND);   \
    result = (CTYPE)v.FIELD;                                                 \
    leave_slot(t, owned);                                                    \
    return result;                                                           \
}

#define DEFINE_NONVIRTUAL_CALLS(NAME, CTYPE, FIELD, KIND)                    \
static CTYPE JNICALL bridge_CallNonvirtual##NAME##Method(                    \
    JNIEnv *env, jobject obj, jclass clazz, jmethodID mid, ...)              \
{                                                                            \
    struct jni_thread_env *t = NULL;                                         \
    bool owned = false;                                                      \
    CTYPE result = (CTYPE)0;                                                 \
    if (!enter_slot(env, &t, &owned)) return result;                         \
    struct jni_call_source src;                                              \
    src.values = NULL;                                                       \
    va_list ap;                                                              \
    va_start(ap, mid);                                                       \
    src.list = &ap;                                                          \
    jvalue v = invoke_common(t, obj, clazz, mid, &src, false, true, KIND);   \
    va_end(ap);                                                              \
    result = (CTYPE)v.FIELD;                                                 \
    leave_slot(t, owned);                                                    \
    return result;                                                           \
}                                                                            \
static CTYPE JNICALL bridge_CallNonvirtual##NAME##MethodV(                   \
    JNIEnv *env, jobject obj, jclass clazz, jmethodID mid, va_list args)     \
{                                                                            \
    struct jni_thread_env *t = NULL;                                         \
    bool owned = false;                                                      \
    CTYPE result = (CTYPE)0;                                                 \
    if (!enter_slot(env, &t, &owned)) return result;                         \
    struct jni_call_source src;                                              \
    src.values = NULL;                                                       \
    va_list copy;                                                            \
    va_copy(copy, args);                                                     \
    src.list = &copy;                                                        \
    jvalue v = invoke_common(t, obj, clazz, mid, &src, false, true, KIND);   \
    va_end(copy);                                                            \
    result = (CTYPE)v.FIELD;                                                 \
    leave_slot(t, owned);                                                    \
    return result;                                                           \
}                                                                            \
static CTYPE JNICALL bridge_CallNonvirtual##NAME##MethodA(                   \
    JNIEnv *env, jobject obj, jclass clazz, jmethodID mid,                   \
    const jvalue *args)                                                      \
{                                                                            \
    struct jni_thread_env *t = NULL;                                         \
    bool owned = false;                                                      \
    CTYPE result = (CTYPE)0;                                                 \
    if (!enter_slot(env, &t, &owned)) return result;                         \
    struct jni_call_source src;                                              \
    src.values = args;                                                       \
    src.list = NULL;                                                         \
    jvalue v = invoke_common(t, obj, clazz, mid, &src, false, true, KIND);   \
    result = (CTYPE)v.FIELD;                                                 \
    leave_slot(t, owned);                                                    \
    return result;                                                           \
}

#define DEFINE_STATIC_CALLS(NAME, CTYPE, FIELD, KIND)                        \
static CTYPE JNICALL bridge_CallStatic##NAME##Method(                        \
    JNIEnv *env, jclass clazz, jmethodID mid, ...)                           \
{                                                                            \
    struct jni_thread_env *t = NULL;                                         \
    bool owned = false;                                                      \
    CTYPE result = (CTYPE)0;                                                 \
    if (!enter_slot(env, &t, &owned)) return result;                         \
    struct jni_call_source src;                                              \
    src.values = NULL;                                                       \
    va_list ap;                                                              \
    va_start(ap, mid);                                                       \
    src.list = &ap;                                                          \
    jvalue v = invoke_common(t, NULL, clazz, mid, &src, true, false, KIND);  \
    va_end(ap);                                                              \
    result = (CTYPE)v.FIELD;                                                 \
    leave_slot(t, owned);                                                    \
    return result;                                                           \
}                                                                            \
static CTYPE JNICALL bridge_CallStatic##NAME##MethodV(                       \
    JNIEnv *env, jclass clazz, jmethodID mid, va_list args)                  \
{                                                                            \
    struct jni_thread_env *t = NULL;                                         \
    bool owned = false;                                                      \
    CTYPE result = (CTYPE)0;                                                 \
    if (!enter_slot(env, &t, &owned)) return result;                         \
    struct jni_call_source src;                                              \
    src.values = NULL;                                                       \
    va_list copy;                                                            \
    va_copy(copy, args);                                                     \
    src.list = &copy;                                                        \
    jvalue v = invoke_common(t, NULL, clazz, mid, &src, true, false, KIND);  \
    va_end(copy);                                                            \
    result = (CTYPE)v.FIELD;                                                 \
    leave_slot(t, owned);                                                    \
    return result;                                                           \
}                                                                            \
static CTYPE JNICALL bridge_CallStatic##NAME##MethodA(                       \
    JNIEnv *env, jclass clazz, jmethodID mid, const jvalue *args)            \
{                                                                            \
    struct jni_thread_env *t = NULL;                                         \
    bool owned = false;                                                      \
    CTYPE result = (CTYPE)0;                                                 \
    if (!enter_slot(env, &t, &owned)) return result;                         \
    struct jni_call_source src;                                              \
    src.values = args;                                                       \
    src.list = NULL;                                                         \
    jvalue v = invoke_common(t, NULL, clazz, mid, &src, true, false, KIND);  \
    result = (CTYPE)v.FIELD;                                                 \
    leave_slot(t, owned);                                                    \
    return result;                                                           \
}

DEFINE_INSTANCE_CALLS(Object, jobject, l, JNI_KIND_OBJECT)
DEFINE_INSTANCE_CALLS(Boolean, jboolean, z, JNI_KIND_BOOLEAN)
DEFINE_INSTANCE_CALLS(Byte, jbyte, b, JNI_KIND_BYTE)
DEFINE_INSTANCE_CALLS(Char, jchar, c, JNI_KIND_CHAR)
DEFINE_INSTANCE_CALLS(Short, jshort, s, JNI_KIND_SHORT)
DEFINE_INSTANCE_CALLS(Int, jint, i, JNI_KIND_INT)
DEFINE_INSTANCE_CALLS(Long, jlong, j, JNI_KIND_LONG)
DEFINE_INSTANCE_CALLS(Float, jfloat, f, JNI_KIND_FLOAT)
DEFINE_INSTANCE_CALLS(Double, jdouble, d, JNI_KIND_DOUBLE)

DEFINE_NONVIRTUAL_CALLS(Object, jobject, l, JNI_KIND_OBJECT)
DEFINE_NONVIRTUAL_CALLS(Boolean, jboolean, z, JNI_KIND_BOOLEAN)
DEFINE_NONVIRTUAL_CALLS(Byte, jbyte, b, JNI_KIND_BYTE)
DEFINE_NONVIRTUAL_CALLS(Char, jchar, c, JNI_KIND_CHAR)
DEFINE_NONVIRTUAL_CALLS(Short, jshort, s, JNI_KIND_SHORT)
DEFINE_NONVIRTUAL_CALLS(Int, jint, i, JNI_KIND_INT)
DEFINE_NONVIRTUAL_CALLS(Long, jlong, j, JNI_KIND_LONG)
DEFINE_NONVIRTUAL_CALLS(Float, jfloat, f, JNI_KIND_FLOAT)
DEFINE_NONVIRTUAL_CALLS(Double, jdouble, d, JNI_KIND_DOUBLE)

DEFINE_STATIC_CALLS(Object, jobject, l, JNI_KIND_OBJECT)
DEFINE_STATIC_CALLS(Boolean, jboolean, z, JNI_KIND_BOOLEAN)
DEFINE_STATIC_CALLS(Byte, jbyte, b, JNI_KIND_BYTE)
DEFINE_STATIC_CALLS(Char, jchar, c, JNI_KIND_CHAR)
DEFINE_STATIC_CALLS(Short, jshort, s, JNI_KIND_SHORT)
DEFINE_STATIC_CALLS(Int, jint, i, JNI_KIND_INT)
DEFINE_STATIC_CALLS(Long, jlong, j, JNI_KIND_LONG)
DEFINE_STATIC_CALLS(Float, jfloat, f, JNI_KIND_FLOAT)
DEFINE_STATIC_CALLS(Double, jdouble, d, JNI_KIND_DOUBLE)

#undef DEFINE_INSTANCE_CALLS
#undef DEFINE_NONVIRTUAL_CALLS
#undef DEFINE_STATIC_CALLS

/* void 返回的调用单独写，避免把 void 塞进泛型宏。 */
#define DEFINE_INSTANCE_VOID_CALLS()                                         \
static void JNICALL bridge_CallVoidMethod(JNIEnv *env, jobject obj,          \
                                          jmethodID mid, ...)                \
{                                                                            \
    struct jni_thread_env *t = NULL;                                         \
    bool owned = false;                                                      \
    if (!enter_slot(env, &t, &owned)) return;                                \
    struct jni_call_source src;                                              \
    src.values = NULL;                                                       \
    va_list ap;                                                              \
    va_start(ap, mid);                                                       \
    src.list = &ap;                                                          \
    (void)invoke_common(t, obj, NULL, mid, &src, false, false,               \
                        JNI_KIND_VOID);                                      \
    va_end(ap);                                                              \
    leave_slot(t, owned);                                                    \
}                                                                            \
static void JNICALL bridge_CallVoidMethodV(JNIEnv *env, jobject obj,         \
                                           jmethodID mid, va_list args)      \
{                                                                            \
    struct jni_thread_env *t = NULL;                                         \
    bool owned = false;                                                      \
    if (!enter_slot(env, &t, &owned)) return;                                \
    struct jni_call_source src;                                              \
    src.values = NULL;                                                       \
    va_list copy;                                                            \
    va_copy(copy, args);                                                     \
    src.list = &copy;                                                        \
    (void)invoke_common(t, obj, NULL, mid, &src, false, false,               \
                        JNI_KIND_VOID);                                      \
    va_end(copy);                                                            \
    leave_slot(t, owned);                                                    \
}                                                                            \
static void JNICALL bridge_CallVoidMethodA(JNIEnv *env, jobject obj,         \
                                           jmethodID mid,                    \
                                           const jvalue *args)               \
{                                                                            \
    struct jni_thread_env *t = NULL;                                         \
    bool owned = false;                                                      \
    if (!enter_slot(env, &t, &owned)) return;                                \
    struct jni_call_source src;                                              \
    src.values = args;                                                       \
    src.list = NULL;                                                         \
    (void)invoke_common(t, obj, NULL, mid, &src, false, false,               \
                        JNI_KIND_VOID);                                      \
    leave_slot(t, owned);                                                    \
}

#define DEFINE_NONVIRTUAL_VOID_CALLS()                                       \
static void JNICALL bridge_CallNonvirtualVoidMethod(                         \
    JNIEnv *env, jobject obj, jclass clazz, jmethodID mid, ...)              \
{                                                                            \
    struct jni_thread_env *t = NULL;                                         \
    bool owned = false;                                                      \
    if (!enter_slot(env, &t, &owned)) return;                                \
    struct jni_call_source src;                                              \
    src.values = NULL;                                                       \
    va_list ap;                                                              \
    va_start(ap, mid);                                                       \
    src.list = &ap;                                                          \
    (void)invoke_common(t, obj, clazz, mid, &src, false, true,               \
                        JNI_KIND_VOID);                                      \
    va_end(ap);                                                              \
    leave_slot(t, owned);                                                    \
}                                                                            \
static void JNICALL bridge_CallNonvirtualVoidMethodV(                        \
    JNIEnv *env, jobject obj, jclass clazz, jmethodID mid, va_list args)     \
{                                                                            \
    struct jni_thread_env *t = NULL;                                         \
    bool owned = false;                                                      \
    if (!enter_slot(env, &t, &owned)) return;                                \
    struct jni_call_source src;                                              \
    src.values = NULL;                                                       \
    va_list copy;                                                            \
    va_copy(copy, args);                                                     \
    src.list = &copy;                                                        \
    (void)invoke_common(t, obj, clazz, mid, &src, false, true,               \
                        JNI_KIND_VOID);                                      \
    va_end(copy);                                                            \
    leave_slot(t, owned);                                                    \
}                                                                            \
static void JNICALL bridge_CallNonvirtualVoidMethodA(                        \
    JNIEnv *env, jobject obj, jclass clazz, jmethodID mid,                   \
    const jvalue *args)                                                      \
{                                                                            \
    struct jni_thread_env *t = NULL;                                         \
    bool owned = false;                                                      \
    if (!enter_slot(env, &t, &owned)) return;                                \
    struct jni_call_source src;                                              \
    src.values = args;                                                       \
    src.list = NULL;                                                         \
    (void)invoke_common(t, obj, clazz, mid, &src, false, true,               \
                        JNI_KIND_VOID);                                      \
    leave_slot(t, owned);                                                    \
}

#define DEFINE_STATIC_VOID_CALLS()                                           \
static void JNICALL bridge_CallStaticVoidMethod(                             \
    JNIEnv *env, jclass clazz, jmethodID mid, ...)                           \
{                                                                            \
    struct jni_thread_env *t = NULL;                                         \
    bool owned = false;                                                      \
    if (!enter_slot(env, &t, &owned)) return;                                \
    struct jni_call_source src;                                              \
    src.values = NULL;                                                       \
    va_list ap;                                                              \
    va_start(ap, mid);                                                       \
    src.list = &ap;                                                          \
    (void)invoke_common(t, NULL, clazz, mid, &src, true, false,              \
                        JNI_KIND_VOID);                                      \
    va_end(ap);                                                              \
    leave_slot(t, owned);                                                    \
}                                                                            \
static void JNICALL bridge_CallStaticVoidMethodV(                            \
    JNIEnv *env, jclass clazz, jmethodID mid, va_list args)                  \
{                                                                            \
    struct jni_thread_env *t = NULL;                                         \
    bool owned = false;                                                      \
    if (!enter_slot(env, &t, &owned)) return;                                \
    struct jni_call_source src;                                              \
    src.values = NULL;                                                       \
    va_list copy;                                                            \
    va_copy(copy, args);                                                     \
    src.list = &copy;                                                        \
    (void)invoke_common(t, NULL, clazz, mid, &src, true, false,              \
                        JNI_KIND_VOID);                                      \
    va_end(copy);                                                            \
    leave_slot(t, owned);                                                    \
}                                                                            \
static void JNICALL bridge_CallStaticVoidMethodA(                            \
    JNIEnv *env, jclass clazz, jmethodID mid, const jvalue *args)            \
{                                                                            \
    struct jni_thread_env *t = NULL;                                         \
    bool owned = false;                                                      \
    if (!enter_slot(env, &t, &owned)) return;                                \
    struct jni_call_source src;                                              \
    src.values = args;                                                       \
    src.list = NULL;                                                         \
    (void)invoke_common(t, NULL, clazz, mid, &src, true, false,              \
                        JNI_KIND_VOID);                                      \
    leave_slot(t, owned);                                                    \
}

DEFINE_INSTANCE_VOID_CALLS()
DEFINE_NONVIRTUAL_VOID_CALLS()
DEFINE_STATIC_VOID_CALLS()

#undef DEFINE_INSTANCE_VOID_CALLS
#undef DEFINE_NONVIRTUAL_VOID_CALLS
#undef DEFINE_STATIC_VOID_CALLS

/* ------------------------------------------------------------------ */
/* 字符串槽                                                            */
/* ------------------------------------------------------------------ */

static jstring JNICALL bridge_NewString(JNIEnv *env, const jchar *unicode,
                                        jsize len)
{
    struct jni_thread_env *t = NULL;
    bool owned = false;
    if (!enter_slot(env, &t, &owned)) return NULL;
    jstring result = NULL;
    if (len < 0 || (len > 0 && !unicode)) {
        bridge_set_pending_new(t->bridge, "java/lang/IllegalArgumentException",
                               "NewString invalid buffer or length");
        goto done;
    }
    size_t utf8_len = 0;
    char *utf8 = utf16_to_utf8_alloc((const uint16_t *)unicode,
                                     (size_t)len, &utf8_len, NULL);
    if (!utf8) {
        bridge_set_pending_new(t->bridge, "java/lang/OutOfMemoryError",
                               "NewString conversion failed");
        goto done;
    }
    dvm_ref ref = dvm_new_string_n(t->bridge->vm, utf8, utf8_len);
    native_scratch_release(utf8);
    if (ref == 0) {
        if (!move_dvm_exception_to_pending(t))
            bridge_set_pending_new(t->bridge, "java/lang/OutOfMemoryError",
                                   "NewString allocation failed");
        goto done;
    }
    result = new_local_obj(t, ref);
done:
    leave_slot(t, owned);
    return result;
}

static jsize JNICALL bridge_GetStringLength(JNIEnv *env, jstring str)
{
    struct jni_thread_env *t = NULL;
    bool owned = false;
    if (!enter_slot(env, &t, &owned)) return 0;
    jsize result = 0;
    const char *bytes = NULL;
    size_t len = 0;
    if (!string_bytes(t->bridge, str, &bytes, &len)) {
        bridge_set_pending_new(t->bridge, "java/lang/NullPointerException",
                               "GetStringLength invalid jstring");
        goto done;
    }
    result = (jsize)utf16_units_from_utf8(bytes, len);
done:
    leave_slot(t, owned);
    return result;
}

/* 释放字符串缓存中由 GetStringChars/GetStringCritical 建立的那一份 UTF-16
 * 副本；ReleaseStringChars 之后原生代码不得再使用该指针。 */
/* Each acquisition remembers its issuing token. Release accepts that token
 * even after DeleteLocalRef, or a currently valid alias of the same identity. */
static bool jni_string_loan_add(struct jni_thread_env *t,struct jni_utf_cache *c,jstring str,bool modified)
{
    unsigned *count=modified?&c->modified_borrows:&c->utf16_borrows;
    if(*count==UINT_MAX){bridge_set_pending_new(t->bridge,"java/lang/OutOfMemoryError","JNI string loan count overflow");return false;}
    struct jni_string_loan *loan=malloc(sizeof *loan);
    if(!loan){bridge_set_pending_new(t->bridge,"java/lang/OutOfMemoryError","JNI string loan metadata");return false;}
    *loan=(struct jni_string_loan){.next=c->loans,.token=str,.modified=modified};
    c->loans=loan;++*count;c->prepared_only=false;return true;
}

static bool jni_string_loan_drop(struct jni_thread_env *t,struct jni_utf_cache *c,jstring str,bool modified)
{
    if(!dvm_gc_handle_ref(t->bridge->vm,c->retention))return false;
    dvm_ref alias=0;bool valid_alias=resolve_object_any(t->bridge,str,&alias)&&alias==c->ref;
    for(unsigned pass=0;pass<2;pass++)for(struct jni_string_loan **at=&c->loans;*at;at=&(*at)->next){
        struct jni_string_loan *loan=*at;
        if(loan->modified!=modified)continue;
        if(pass==0?loan->token!=str:!valid_alias)continue;
        *at=loan->next;free(loan);
        if(modified)--c->modified_borrows;else --c->utf16_borrows;
        return true;
    }
    return false;
}

static void release_utf16_cache(struct jni_thread_env *t, jstring str,
                                const jchar *chars)
{
    if (!t) return;
    for (struct jni_utf_cache *c = t->strings; c; c = c->next) {
        /* Local/global aliases denote the same Java String but have different
         * JNI tokens. The issued native pointer identifies its loan. */
        if (!c->used || c->utf16 != (const uint16_t *)chars)
            continue;
        if(!jni_string_loan_drop(t,c,str,false)){
            bridge_set_pending_new(t->bridge,"java/lang/IllegalArgumentException","ReleaseStringChars wrong string/loan");return;}
        if(c->utf16_borrows)return;
        free(c->utf16);
        c->utf16 = NULL;
        c->utf16_len = 0;
        if (!c->modified) clear_utf_cache(c);
        return;
    }
}

static void release_modified_cache(struct jni_thread_env *t, jstring str,
                                   const char *chars)
{
    if (!t) return;
    for (struct jni_utf_cache *c = t->strings; c; c = c->next) {
        if (!c->used || c->modified != chars) continue;
        if(!jni_string_loan_drop(t,c,str,true)){
            bridge_set_pending_new(t->bridge,"java/lang/IllegalArgumentException","ReleaseStringUTFChars wrong string/loan");return;}
        if(c->modified_borrows)return;
        free(c->modified);
        c->modified = NULL;
        c->modified_len = 0;
        if (!c->utf16) clear_utf_cache(c);
        return;
    }
}

static const jchar *JNICALL bridge_GetStringChars(JNIEnv *env, jstring str,
                                                  jboolean *isCopy)
{
    struct jni_thread_env *t = NULL;
    bool owned = false;
    if (!enter_slot(env, &t, &owned)) return NULL;
    const jchar *result = NULL;
    struct jni_utf_cache *c = string_utf16_cache(t, str);
    if (c && c->utf16 && jni_string_loan_add(t,c,str,false)) {
        result = (const jchar *)c->utf16;
        if (isCopy) *isCopy = JNI_TRUE;
    }
    leave_slot(t, owned);
    return result;
}

static void JNICALL bridge_ReleaseStringChars(JNIEnv *env, jstring str,
                                              const jchar *chars)
{
    struct jni_thread_env *t=NULL;bool owned=false;
    if(!enter_slot(env,&t,&owned))return;
    if(chars)release_utf16_cache(t,str,chars);
    leave_slot(t,owned);
}

static jstring JNICALL bridge_NewStringUTF(JNIEnv *env, const char *utf)
{
    struct jni_thread_env *t = NULL;
    bool owned = false;
    if (!enter_slot(env, &t, &owned)) return NULL;
    jstring result = NULL;
    if (!utf) {
        bridge_set_pending_new(t->bridge, "java/lang/NullPointerException",
                               "NewStringUTF received NULL");
        goto done;
    }
    size_t utf8_len = 0;
    char *utf8 = modified_to_utf8_alloc(utf, strlen(utf), &utf8_len);
    if (!utf8) {
        bridge_set_pending_new(t->bridge, "java/lang/OutOfMemoryError",
                               "NewStringUTF conversion failed");
        goto done;
    }
    dvm_ref ref = dvm_new_string_n(t->bridge->vm, utf8, utf8_len);
    native_scratch_release(utf8);
    if (ref == 0) {
        if (!move_dvm_exception_to_pending(t))
            bridge_set_pending_new(t->bridge, "java/lang/OutOfMemoryError",
                                   "NewStringUTF allocation failed");
        goto done;
    }
    result = new_local_obj(t, ref);
done:
    leave_slot(t, owned);
    return result;
}

static jsize JNICALL bridge_GetStringUTFLength(JNIEnv *env, jstring str)
{
    struct jni_thread_env *t = NULL;
    bool owned = false;
    if (!enter_slot(env, &t, &owned)) return 0;
    jsize result = 0;
    struct jni_utf_cache *c = string_modified_cache(t, str);
    if (c) result = (jsize)c->modified_len;
    leave_slot(t, owned);
    return result;
}

static const char *JNICALL bridge_GetStringUTFChars(JNIEnv *env, jstring str,
                                                    jboolean *isCopy)
{
    struct jni_thread_env *t = NULL;
    bool owned = false;
    if (!enter_slot(env, &t, &owned)) return NULL;
    /* JNI string access must not replace the pending exception that explains
     * why the native caller has no usable string. Preserve it verbatim. */
    if (t->pending) {
#ifdef LUNARIA_UNITY_PLAYER_PROBE
        diagnostic_primary_exception(t,"GetStringUTFChars_pending_preserved");
#endif
        leave_slot(t,owned);
        return NULL;
    }
    const char *result = NULL;
    struct jni_utf_cache *c = string_modified_cache(t, str);
    if (c && c->modified && jni_string_loan_add(t,c,str,true)) {
        result = c->modified;
        if (isCopy) *isCopy = JNI_TRUE;
    }
#ifdef LUNARIA_UNITY_PLAYER_PROBE
    if(result){
        if(!strncmp(result,"org.json.JSONObject@",sizeof("org.json.JSONObject@")-1)){
            int saved=errno;
            FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt","a");
            if(f){fprintf(f,"JNI_JSON_IDENTITY_STRING caller=%p length=%zu text=%.64s\n",__builtin_return_address(0),strlen(result),result);fclose(f);}
            errno=saved;
        }
        static atomic_uint paths;
        if(atomic_fetch_add(&paths,1)<256){
            int saved=errno;
            FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt","a");
            if(f){fprintf(f,"JNI_UTF_PATH len=%zu caller=%p path=%s\n",strlen(result),
                          __builtin_return_address(0),result[0]=='/'?result:(result[0]?"<nonpath>":"<empty>"));fclose(f);}
            errno=saved;
        }
    }
    if (!result) {
        uint32_t ref=0;void *cls=NULL;jni_ref_kind kind=JNI_REF_NONE;
        bool resolved=jni_handle_resolve_any(&t->bridge->handles,str,&ref,&cls,&kind);
        struct dvm_object *object=ref?dvm__obj(t->bridge->vm,ref):NULL;
        const char *type=object&&object->cls&&object->cls->name?object->cls->name:"<none>";
        sdk_trace_fprintf(stderr,"JNI_STRING_UTF_NULL token=0x%llx resolved=%d ref=%u kind=%d type=%s class_token=%d\n",
                         (unsigned long long)jni_token_raw(str),resolved,ref,(int)kind,type,cls!=NULL);
        diagnostic_primary_exception(t,"GetStringUTFChars_null");
    }
#endif
    leave_slot(t, owned);
    return result;
}

static void JNICALL bridge_ReleaseStringUTFChars(JNIEnv *env, jstring str,
                                                 const char *chars)
{
    struct jni_thread_env *t=NULL;bool owned=false;
    if(!enter_slot(env,&t,&owned))return;
    if(chars)release_modified_cache(t,str,chars);
    leave_slot(t,owned);
}

static void JNICALL bridge_GetStringRegion(JNIEnv *env, jstring str,
                                           jsize start, jsize len, jchar *buf)
{
    struct jni_thread_env *t = NULL;
    bool owned = false;
    if (!enter_slot(env, &t, &owned)) return;
    struct jni_utf_cache *c = string_utf16_cache(t, str);
    if (!c || !c->utf16) goto done;
    if (start < 0 || len < 0 || !buf ||
        (uint64_t)start + (uint64_t)len > (uint64_t)c->utf16_len) {
        bridge_set_pending_new(t->bridge,
                               "java/lang/StringIndexOutOfBoundsException",
                               "GetStringRegion range out of bounds");
        goto done;
    }
    if (len > 0) memcpy(buf, c->utf16 + start, (size_t)len * sizeof(jchar));
done:
    leave_slot(t, owned);
}

static void JNICALL bridge_GetStringUTFRegion(JNIEnv *env, jstring str,
                                              jsize start, jsize len,
                                              char *buf)
{
    struct jni_thread_env *t = NULL;
    bool owned = false;
    if (!enter_slot(env, &t, &owned)) return;
    struct jni_utf_cache *c = string_utf16_cache(t, str);
    if (!c || !c->utf16) goto done;
    if (start < 0 || len < 0 || !buf ||
        (uint64_t)start + (uint64_t)len > (uint64_t)c->utf16_len) {
        bridge_set_pending_new(t->bridge,
                               "java/lang/StringIndexOutOfBoundsException",
                               "GetStringUTFRegion range out of bounds");
        goto done;
    }
    size_t out_len = 0;
    char *tmp = utf16_to_modified_alloc(c->utf16 + start, (size_t)len,
                                        &out_len);
    if (!tmp) {
        bridge_set_pending_new(t->bridge, "java/lang/OutOfMemoryError",
                               "GetStringUTFRegion conversion failed");
        goto done;
    }
    if (out_len > 0) memcpy(buf, tmp, out_len);
    native_scratch_release(tmp);
done:
    leave_slot(t, owned);
}

static const jchar *JNICALL bridge_GetStringCritical(JNIEnv *env, jstring str,
                                                     jboolean *isCopy)
{
    /* DVM 字符串本体是 UTF-8，无法零拷贝暴露 UTF-16；这里返回线程内副本。 */
    return bridge_GetStringChars(env, str, isCopy);
}

static void JNICALL bridge_ReleaseStringCritical(JNIEnv *env, jstring str,
                                                 const jchar *cstring)
{
    struct jni_thread_env *t=NULL;bool owned=false;
    if(!enter_slot(env,&t,&owned))return;
    if(cstring)release_utf16_cache(t,str,cstring);
    leave_slot(t,owned);
}

/* ------------------------------------------------------------------ */
/* 数组槽                                                              */
/* ------------------------------------------------------------------ */

static jsize JNICALL bridge_GetArrayLength(JNIEnv *env, jarray array)
{
    struct jni_thread_env *t = NULL;
    bool owned = false;
    if (!enter_slot(env, &t, &owned)) return 0;
    jsize result = 0;
    uint32_t len = 0;
    if (!array_info(t->bridge, array, 0, NULL, &len)) {
        bridge_set_pending_new(t->bridge, "java/lang/IllegalArgumentException",
                               "GetArrayLength invalid jarray");
        goto done;
    }
    result = (jsize)len;
done:
    leave_slot(t, owned);
    return result;
}

static jobjectArray JNICALL bridge_NewObjectArray(JNIEnv *env, jsize len,
                                                  jclass clazz, jobject init)
{
    struct jni_thread_env *t = NULL;
    bool owned = false;
    if (!enter_slot(env, &t, &owned)) return NULL;
    jobjectArray result = new_object_array(t, len, clazz, init);
    leave_slot(t, owned);
    return result;
}

static jobject JNICALL bridge_GetObjectArrayElement(JNIEnv *env,
                                                    jobjectArray array,
                                                    jsize index)
{
    struct jni_thread_env *t = NULL;
    bool owned = false;
    if (!enter_slot(env, &t, &owned)) return NULL;
    jobject result = NULL;
    void *data = NULL;
    uint32_t len = 0;
    if (!array_info(t->bridge, array, 'L', &data, &len)) {
        bridge_set_pending_new(t->bridge, "java/lang/IllegalArgumentException",
                               "GetObjectArrayElement invalid array");
        goto done;
    }
    if (index < 0 || (uint32_t)index >= len) {
        bridge_set_pending_new(t->bridge,
                               "java/lang/ArrayIndexOutOfBoundsException",
                               "GetObjectArrayElement index out of bounds");
        goto done;
    }
    if (data) result = new_local_obj(t, ((dvm_ref *)data)[index]);
done:
    leave_slot(t, owned);
    return result;
}

static void JNICALL bridge_SetObjectArrayElement(JNIEnv *env,
                                                 jobjectArray array,
                                                 jsize index, jobject val)
{
    struct jni_thread_env *t = NULL;
    bool owned = false;
    if (!enter_slot(env, &t, &owned)) return;
    struct gc_operation gc6_store;
    if(!gc_operation_begin(t->bridge->vm,&gc6_store)){gc_operation_end(&gc6_store);leave_slot(t,owned);return;}
    uint32_t ref = 0;
    void *cls = NULL;
    jni_ref_kind kind = JNI_REF_NONE;
    struct dvm_object *ao = NULL;
    void *data = NULL;
    uint32_t len = 0;
    dvm_ref vref = 0;
    if (!array_info(t->bridge, array, 'L', &data, &len)) {
        bridge_set_pending_new(t->bridge, "java/lang/IllegalArgumentException",
                               "SetObjectArrayElement invalid array");
        goto done;
    }
    if (index < 0 || (uint32_t)index >= len) {
        bridge_set_pending_new(t->bridge,
                               "java/lang/ArrayIndexOutOfBoundsException",
                               "SetObjectArrayElement index out of bounds");
        goto done;
    }
    if (!jni_handle_resolve_any(&t->bridge->handles, array, &ref, &cls, &kind)) {
        bridge_set_pending_new(t->bridge, "java/lang/IllegalArgumentException",
                               "SetObjectArrayElement invalid array reference");
        goto done;
    }
    ao = dvm__obj(t->bridge->vm, ref);
    if (!ao || !ao->cls || !ao->cls->elem) {
        bridge_set_pending_new(t->bridge, "java/lang/IllegalArgumentException",
                               "SetObjectArrayElement missing element class");
        goto done;
    }
    if (!token_to_dvm_ref(t, val, &vref)) {
        bridge_set_pending_new(t->bridge, "java/lang/IllegalArgumentException",
                               "SetObjectArrayElement invalid value");
        goto done;
    }
    if (vref != 0) {
        struct dvm_class *vc = dvm_object_class(t->bridge->vm, vref);
        if (!vc || !dvm__class_assignable(t->bridge->vm, vc, ao->cls->elem)) {
            bridge_set_pending_new(t->bridge, "java/lang/ArrayStoreException",
                                   "SetObjectArrayElement type mismatch");
            goto done;
        }
    }
    if (data) GC_STORE_REF(t->bridge->vm,&((dvm_ref *)data)[index],vref,"JNI_SetObjectArrayElement");
done:
    gc_operation_end(&gc6_store);
    leave_slot(t, owned);
}

#define DEFINE_PRIM_ARRAY_ACCESS(NAME, CTYPE, KINDCHAR, JKIND, DESC)          \
static jarray JNICALL bridge_New##NAME##Array(JNIEnv *env, jsize len)         \
{                                                                             \
    struct jni_thread_env *t = NULL;                                          \
    bool owned = false;                                                       \
    if (!enter_slot(env, &t, &owned)) return NULL;                            \
    jarray result = new_prim_array(t, len, KINDCHAR, DESC);                   \
    probe_array_event("new", KINDCHAR, result, NULL, (size_t)len,              \
                      t->pending, t->frame_depth);                            \
    leave_slot(t, owned);                                                     \
    return result;                                                            \
}                                                                             \
static CTYPE * JNICALL bridge_Get##NAME##ArrayElements(                       \
    JNIEnv *env, jarray array, jboolean *isCopy)                              \
{                                                                             \
    struct jni_thread_env *t = NULL;                                          \
    bool owned = false;                                                       \
    if (!enter_slot(env, &t, &owned)) return NULL;                            \
    CTYPE *result = NULL;                                                     \
    void *data = NULL;                                                        \
    uint32_t len = 0;                                                         \
    if (!array_info(t->bridge, array, KINDCHAR, &data, &len)) {               \
        bridge_set_pending_new(t->bridge, "java/lang/IllegalArgumentException",\
                               "array elements: wrong array type");           \
        goto done;                                                            \
    }                                                                         \
    size_t bytes = (size_t)len * sizeof(CTYPE);                               \
    CTYPE *copy = malloc(bytes ? bytes : 1u);                                 \
    if (!copy) {                                                              \
        bridge_set_pending_new(t->bridge, "java/lang/OutOfMemoryError",       \
                               "array elements allocation failed");           \
        goto done;                                                            \
    }                                                                         \
    if (data && bytes) memcpy(copy, data, bytes);                             \
    if (!array_cache_store(t, array, JKIND, copy, bytes)) {                   \
        free(copy);                                                           \
        goto done;                                                            \
    }                                                                         \
    result = copy;                                                            \
    if (isCopy) *isCopy = JNI_TRUE;                                           \
done:                                                                         \
    probe_array_event("get_elements", KINDCHAR, array, result, len,            \
                      t->pending, t->frame_depth);                            \
    leave_slot(t, owned);                                                     \
    return result;                                                            \
}                                                                             \
static void JNICALL bridge_Release##NAME##ArrayElements(                      \
    JNIEnv *env, jarray array, CTYPE *elems, jint mode)                       \
{                                                                             \
    struct jni_thread_env *t = NULL;                                          \
    bool owned = false;                                                       \
    if (!enter_slot(env, &t, &owned)) return;                                 \
    struct jni_array_cache *c = array_cache_find(t, array, elems);            \
    if (!c) {                                                                 \
        bridge_set_pending_new(t->bridge, "java/lang/IllegalArgumentException",\
                               "ReleaseArrayElements: unknown buffer");      \
        goto done;                                                            \
    }                                                                         \
    if(mode!=0&&mode!=JNI_COMMIT&&mode!=JNI_ABORT){                           \
        bridge_set_pending_new(t->bridge,"java/lang/IllegalArgumentException",\
                               "ReleaseArrayElements invalid mode");          \
        goto done;                                                            \
    }                                                                         \
    dvm_ref retained=dvm_gc_handle_ref(t->bridge->vm,c->retention);           \
    struct dvm_object *object=retained?dvm__obj(t->bridge->vm,retained):NULL; \
    if(!object||object->identity_generation!=c->identity||                    \
       object->elem_kind!=KINDCHAR||c->kind!=JKIND||                          \
       (size_t)object->length*sizeof(CTYPE)!=c->bytes){                       \
        bridge_set_pending_new(t->bridge,"java/lang/IllegalArgumentException",\
                               "ReleaseArrayElements stale/wrong array");     \
        goto done;                                                            \
    }                                                                         \
    if(mode!=JNI_ABORT&&c->bytes)memcpy(object->data,c->elems,c->bytes);      \
    if(mode==JNI_COMMIT)goto done;                                            \
    clear_array_cache(c);                                                     \
done:                                                                         \
    leave_slot(t, owned);                                                     \
}                                                                             \
static void JNICALL bridge_Get##NAME##ArrayRegion(                            \
    JNIEnv *env, jarray array, jsize start, jsize len, CTYPE *buf)            \
{                                                                             \
    struct jni_thread_env *t = NULL;                                          \
    bool owned = false;                                                       \
    if (!enter_slot(env, &t, &owned)) return;                                 \
    void *data = NULL;                                                        \
    uint32_t alen = 0;                                                        \
    if (!array_info(t->bridge, array, KINDCHAR, &data, &alen)) {              \
        bridge_set_pending_new(t->bridge, "java/lang/IllegalArgumentException",\
                               "array region: wrong array type");             \
        goto done;                                                            \
    }                                                                         \
    if (start < 0 || len < 0 || !buf ||                                      \
        (uint64_t)start + (uint64_t)len > (uint64_t)alen) {                   \
        bridge_set_pending_new(t->bridge,                                     \
                               "java/lang/ArrayIndexOutOfBoundsException",    \
                               "GetArrayRegion range out of bounds");         \
        goto done;                                                            \
    }                                                                         \
    if (data && len > 0)                                                      \
        memcpy(buf, (CTYPE *)data + start, (size_t)len * sizeof(CTYPE));      \
done:                                                                         \
    leave_slot(t, owned);                                                     \
}                                                                             \
static void JNICALL bridge_Set##NAME##ArrayRegion(                            \
    JNIEnv *env, jarray array, jsize start, jsize len, const CTYPE *buf)      \
{                                                                             \
    struct jni_thread_env *t = NULL;                                          \
    bool owned = false;                                                       \
    if (!enter_slot(env, &t, &owned)) return;                                 \
    void *data = NULL;                                                        \
    uint32_t alen = 0;                                                        \
    if (!array_info(t->bridge, array, KINDCHAR, &data, &alen)) {              \
        bridge_set_pending_new(t->bridge, "java/lang/IllegalArgumentException",\
                               "array region: wrong array type");             \
        goto done;                                                            \
    }                                                                         \
    if (start < 0 || len < 0 || !buf ||                                      \
        (uint64_t)start + (uint64_t)len > (uint64_t)alen) {                   \
        bridge_set_pending_new(t->bridge,                                     \
                               "java/lang/ArrayIndexOutOfBoundsException",    \
                               "SetArrayRegion range out of bounds");         \
        goto done;                                                            \
    }                                                                         \
    if (data && len > 0)                                                      \
        memcpy((CTYPE *)data + start, buf, (size_t)len * sizeof(CTYPE));      \
done:                                                                         \
    leave_slot(t, owned);                                                     \
}

DEFINE_PRIM_ARRAY_ACCESS(Boolean, jboolean, 'Z', JNI_KIND_BOOLEAN, "[Z")
DEFINE_PRIM_ARRAY_ACCESS(Byte, jbyte, 'B', JNI_KIND_BYTE, "[B")
DEFINE_PRIM_ARRAY_ACCESS(Char, jchar, 'C', JNI_KIND_CHAR, "[C")
DEFINE_PRIM_ARRAY_ACCESS(Short, jshort, 'S', JNI_KIND_SHORT, "[S")
DEFINE_PRIM_ARRAY_ACCESS(Int, jint, 'I', JNI_KIND_INT, "[I")
DEFINE_PRIM_ARRAY_ACCESS(Long, jlong, 'J', JNI_KIND_LONG, "[J")
DEFINE_PRIM_ARRAY_ACCESS(Float, jfloat, 'F', JNI_KIND_FLOAT, "[F")
DEFINE_PRIM_ARRAY_ACCESS(Double, jdouble, 'D', JNI_KIND_DOUBLE, "[D")

#undef DEFINE_PRIM_ARRAY_ACCESS

static bool jni_borrow_token_matches(struct jni_thread_env *t,jobject issued,jobject provided,dvm_ref ref)
{
    if(issued==provided)return true;
    dvm_ref other=0;return resolve_object_any(t->bridge,provided,&other)&&other==ref;
}

static void *JNICALL bridge_GetPrimitiveArrayCritical(JNIEnv *env,jarray array,jboolean *isCopy)
{
    struct jni_thread_env *t=NULL;bool owned=false;
    if(!enter_slot(env,&t,&owned))return NULL;
    void *data=NULL;dvm_ref ref=0;
    if(!resolve_object_any(t->bridge,array,&ref)||!ref)goto invalid;
    struct dvm_object *o=dvm__obj(t->bridge->vm,ref);
    if(!o||o->kind!=DVM_OBJ_ARRAY||!strchr("ZBCSIJFD",o->elem_kind)||!o->elem_kind||!o->data)goto invalid;
    struct jni_critical_borrow *loan=calloc(1,sizeof *loan);
    if(!loan){bridge_set_pending_new(t->bridge,"java/lang/OutOfMemoryError","critical borrow allocation");goto done;}
    loan->retention=jni_gc_retain(t,ref,DVM_GC_HANDLE_STRONG|DVM_GC_HANDLE_BORROW_PAYLOAD);
    if(!loan->retention){free(loan);goto done;}
    if(!dvm_gc_pin_checked(t->bridge->vm,ref)){
        dvm_gc_handle_drop(t->bridge->vm,loan->retention);free(loan);goto done;
    }
    /* Retention/pin helpers may enter the VM; acquire the pinned body anew. */
    o=dvm__obj(t->bridge->vm,ref);
    if(!o){(void)dvm_gc_unpin_checked(t->bridge->vm,ref);(void)dvm_gc_handle_drop(t->bridge->vm,loan->retention);free(loan);goto invalid;}
    loan->array=array;loan->ref=ref;loan->identity=o->identity_generation;loan->pointer=o->data;
    loan->next=t->critical;t->critical=loan;data=loan->pointer;
    if(isCopy)*isCopy=JNI_FALSE;
    goto done;
invalid:
    bridge_set_pending_new(t->bridge,"java/lang/IllegalArgumentException","GetPrimitiveArrayCritical invalid primitive array");
done:
    leave_slot(t,owned);return data;
}

static void JNICALL bridge_ReleasePrimitiveArrayCritical(JNIEnv *env,jarray array,void *carray,jint mode)
{
    struct jni_thread_env *t=NULL;bool owned=false;
    if(!enter_slot(env,&t,&owned))return;
    if(mode!=0&&mode!=JNI_COMMIT&&mode!=JNI_ABORT)goto invalid;
    for(unsigned pass=0;pass<2;pass++)for(struct jni_critical_borrow **slot=&t->critical;*slot;slot=&(*slot)->next){
        struct jni_critical_borrow *loan=*slot;
        if(pass==0&&loan->array!=array)continue;
        if(loan->pointer!=carray||!jni_borrow_token_matches(t,loan->array,array,loan->ref))continue;
        dvm_ref ref=dvm_gc_handle_ref(t->bridge->vm,loan->retention);
        struct dvm_object *o=ref?dvm__obj(t->bridge->vm,ref):NULL;
        if(!o||o->identity_generation!=loan->identity||o->data!=carray)goto invalid;
        /* Direct payload: COMMIT keeps this exact acquisition alive. ABORT
         * cannot undo writes to direct storage, but releases one acquisition. */
        if(mode!=JNI_COMMIT){
            if(!dvm_gc_unpin_checked(t->bridge->vm,ref))goto invalid;
            if(!dvm_gc_handle_drop(t->bridge->vm,loan->retention))goto invalid;
            *slot=loan->next;free(loan);
        }
        leave_slot(t,owned);return;
    }
invalid:
    bridge_set_pending_new(t->bridge,"java/lang/IllegalArgumentException","ReleasePrimitiveArrayCritical invalid mode/pointer/array");
    /* JNI already has the original IllegalArgumentException in t->pending.
     * Do not create a second VM exception that leave_slot could substitute. */
    dvm_gc_contract_note(GC_CONTRACT_LOAN);
    leave_slot(t,owned);
}

/* ------------------------------------------------------------------ */
/* RegisterNatives / monitor / 弱引用 / JavaVM                         */
/* ------------------------------------------------------------------ */

static jint JNICALL bridge_RegisterNatives(JNIEnv *env, jclass clazz,
                                           const JNINativeMethod *methods,
                                           jint nMethods)
{
    struct jni_thread_env *t = NULL;
    bool owned = false;
    if (!enter_slot(env, &t, &owned)) return JNI_ERR;
    jint result = JNI_ERR;
    if (!t->bridge->natives) {
        bridge_set_pending_new(t->bridge, "java/lang/UnsatisfiedLinkError",
                               "native loader is unavailable");
        goto done;
    }
    if (nMethods < 0 || (nMethods > 0 && !methods)) {
        bridge_set_pending_new(t->bridge, "java/lang/IllegalArgumentException",
                               "RegisterNatives invalid method array");
        goto done;
    }
    if (!jni_native_register(t->bridge->natives, clazz, methods, nMethods)) {
        char msg[JNI_ERROR_MAX];
        snprintf(msg, sizeof(msg), "RegisterNatives failed: %s",
                 jni_native_last_error(t->bridge->natives));
        bridge_set_pending_new(t->bridge, "java/lang/UnsatisfiedLinkError", msg);
#ifdef LUNARIA_UNITY_PLAYER_PROBE
        {
            static unsigned rf_probe;
            if (rf_probe < 8) {
                struct dvm_class *rc = NULL;
                resolve_class_token(t->bridge, clazz, &rc);
                rf_probe++;
                FILE *f = fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt", "a");
                if (f) {
                    fprintf(f, "REGISTER_NATIVES_FAILED class=%s count=%d err=%s\n",
                            rc && rc->name ? rc->name : "?", (int)nMethods,
                            jni_native_last_error(t->bridge->natives));
                    for (jint i = 0; i < nMethods && i < 8; ++i)
                        fprintf(f, "  REJECT name=%s sig=%s\n",
                                methods[i].name ? methods[i].name : "?",
                                methods[i].signature ? methods[i].signature : "?");
                    fclose(f);
                }
            }
        }
#endif
        goto done;
    }
#ifdef LUNARIA_UNITY_PLAYER_PROBE
    /* 绑定表诊断：Unity 通过 RegisterNatives 注册 native 方法（触摸入口
     * nativeInjectEvent 等都在此表）。记录类名与每个方法名/签名，用于定位
     * 输入注入的目标，而不是猜测混淆后的 Java 方法名。 */
    {
        static unsigned reg_probe;
        if (reg_probe < 8) {
            struct dvm_class *rc = NULL;
            resolve_class_token(t->bridge, clazz, &rc);
            reg_probe++;
            FILE *f = fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt", "a");
            if (f) {
                fprintf(f, "REGISTER_NATIVES class=%s count=%d\n",
                        rc && rc->name ? rc->name : "?", (int)nMethods);
                for (jint i = 0; i < nMethods && i < 40; ++i)
                    fprintf(f, "  NATIVE name=%s sig=%s fn=%p\n",
                            methods[i].name ? methods[i].name : "?",
                            methods[i].signature ? methods[i].signature : "?",
                            (void *)methods[i].fnPtr);
                fclose(f);
            }
        }
    }
#endif
    result = JNI_OK;
done:
    leave_slot(t, owned);
    return result;
}

static jint JNICALL bridge_UnregisterNatives(JNIEnv *env, jclass clazz)
{
    struct jni_thread_env *t = NULL;
    bool owned = false;
    if (!enter_slot(env, &t, &owned)) return JNI_ERR;
    jint result = JNI_ERR;
    if (!t->bridge->natives ||
        !jni_native_unregister(t->bridge->natives, clazz)) {
        bridge_set_pending_new(t->bridge, "java/lang/IllegalArgumentException",
                               "UnregisterNatives invalid class");
        goto done;
    }
    result = JNI_OK;
done:
    leave_slot(t, owned);
    return result;
}

static jint JNICALL bridge_MonitorEnter(JNIEnv *env, jobject obj)
{
    struct jni_thread_env *t = NULL;
    bool owned = false;
    if (!enter_slot(env, &t, &owned)) return JNI_ERR;
    jint result = JNI_ERR;
    dvm_ref ref = 0;
    if (!object_arg(t, obj, &ref, "MonitorEnter object")) goto done;
    if (!dvm__monitor_enter(t->bridge->vm, ref)) {
        bridge_set_pending_new(t->bridge, "java/lang/IllegalMonitorStateException",
                               "MonitorEnter failed");
        goto done;
    }
    result = JNI_OK;
done:
    leave_slot(t, owned);
    return result;
}

static jint JNICALL bridge_MonitorExit(JNIEnv *env, jobject obj)
{
    struct jni_thread_env *t = NULL;
    bool owned = false;
    if (!enter_slot(env, &t, &owned)) return JNI_ERR;
    jint result = JNI_ERR;
    dvm_ref ref = 0;
    if (!object_arg(t, obj, &ref, "MonitorExit object")) goto done;
    if (!dvm__monitor_exit(t->bridge->vm, ref)) {
        bridge_set_pending_new(t->bridge, "java/lang/IllegalMonitorStateException",
                               "MonitorExit failed");
        goto done;
    }
    result = JNI_OK;
done:
    leave_slot(t, owned);
    return result;
}

static jint JNICALL bridge_GetJavaVM(JNIEnv *env, JavaVM **vm)
{
    struct jni_thread_env *t = NULL;
    bool owned = false;
    if (!enter_slot(env, &t, &owned)) return JNI_ERR;
    jint result = JNI_ERR;
    if (!vm) {
        bridge_set_pending_new(t->bridge, "java/lang/NullPointerException",
                               "GetJavaVM output pointer is null");
        goto done;
    }
    /* 调用方变量（JavaVM *）应指向一个 JavaVM 值；这里给出 vm_value 的地址。 */
    *vm = &t->bridge->vm_value;
    result = JNI_OK;
done:
    leave_slot(t, owned);
    return result;
}

static jweak JNICALL bridge_NewWeakGlobalRef(JNIEnv *env, jobject obj)
{
    struct jni_thread_env *t = NULL;
    bool owned = false;
    if (!enter_slot(env, &t, &owned)) return NULL;
    jweak result = NULL;
    dvm_ref ref = 0;
    if (token_to_dvm_ref(t, obj, &ref) && ref != 0) {
        result = jni_handle_new_weak(&t->bridge->handles, ref);
    } else if (!jni_token_is_null(obj)&&!jni_gc_cleared_weak(t->bridge,obj)) {
        bridge_set_pending_new(t->bridge, "java/lang/IllegalArgumentException",
                               "NewWeakGlobalRef invalid reference");
    }
    leave_slot(t, owned);
    return result;
}

static void JNICALL bridge_DeleteWeakGlobalRef(JNIEnv *env, jweak obj)
{
    struct jni_thread_env *t = NULL;
    bool owned = false;
    if (!enter_slot(env, &t, &owned)) return;
    if (!jni_token_is_null(obj) &&
        !jni_handle_delete_weak(&t->bridge->handles, obj)) {
        /* 与 JNI 规范一致：对无效 weak ref 的删除是 no-op，不伪造成功。 */
        jni_bridge_note_limited(t->bridge, 227, "DeleteWeakGlobalRef");
    }
    leave_slot(t, owned);
}

static jobjectRefType JNICALL bridge_GetObjectRefType(JNIEnv *env,
                                                      jobject obj)
{
    struct jni_thread_env *t = NULL;
    bool owned = false;
    if (!enter_slot(env, &t, &owned)) return JNIInvalidRefType;
    jobjectRefType result = jni_handle_ref_type(&t->bridge->handles, obj);
    leave_slot(t, owned);
    return result;
}

/* 未实现槽的统一失败入口：记录受限槽并设置 pending UnsatisfiedLinkError。
 * 返回 0 对返回指针/整数的槽都成立；返回浮点的槽由 stub 自行返回 0.0。 */
int jni_abi_stub_fail(JNIEnv *env, int slot, const char *name)
{
    struct jni_thread_env *t = thread_env_from_env(env);
    if (!t) return 0;
    jni_bridge_note_limited(t->bridge, slot, name);
    char msg[160];
    snprintf(msg, sizeof(msg), "JNI slot %d (%s) 未实现", slot,
             name ? name : "?");
    bridge_set_pending_new(t->bridge, "java/lang/UnsatisfiedLinkError", msg);
    return 0;
}

/* ------------------------------------------------------------------ */
/* native 方法校验与调用                                               */
/* ------------------------------------------------------------------ */

bool jni_bridge_validate_native(struct jni_bridge *b, jclass cls,
                                const char *name, const char *sig)
{
    if (!b || !name || !sig) return false;
    bool owned = false;
    if (!dvm_gil_held(b->vm)) {
        dvm_gil_acquire(b->vm);
        owned = true;
    }
    bool ok = false;
    struct dvm_class *c = NULL;
    struct dvm_method *m = NULL;
    jni_sig_info info;
    if (!resolve_class_token(b, cls, &c) || !c) goto done;
    if (!jni_sig_parse(sig, &info)) goto done;
    m = dvm_find_method(b->vm, c, name, sig);
    if (!m || m->cls != c) goto done;
    if (!m->dex || !(m->access & DEX_ACC_NATIVE)) goto done;
    ok = true;
done:
    if (owned) dvm_gil_release(b->vm);
    return ok;
}

typedef uintptr_t (*jni_native_va_fn)(JNIEnv *, ...);

static jni_native_va_fn native_fn_from_ptr(void *sym)
{
    jni_native_va_fn fn = NULL;
    if (!sym) return NULL;
    if (sizeof(fn) != sizeof(sym)) return NULL;
    memcpy(&fn, &sym, sizeof(fn));
    return fn;
}

/* 参数全部按整数/指针字传递；float/double 已在入口拒绝。 */
#define JNI_NATIVE_CALL_0(fn, env, recv) ((fn)((env), (recv)))
#define JNI_NATIVE_CALL_1(fn, env, recv, w) \
    ((fn)((env), (recv), (w)[0]))
#define JNI_NATIVE_CALL_2(fn, env, recv, w) \
    ((fn)((env), (recv), (w)[0], (w)[1]))
#define JNI_NATIVE_CALL_3(fn, env, recv, w) \
    ((fn)((env), (recv), (w)[0], (w)[1], (w)[2]))
#define JNI_NATIVE_CALL_4(fn, env, recv, w) \
    ((fn)((env), (recv), (w)[0], (w)[1], (w)[2], (w)[3]))
#define JNI_NATIVE_CALL_5(fn, env, recv, w) \
    ((fn)((env), (recv), (w)[0], (w)[1], (w)[2], (w)[3], (w)[4]))
#define JNI_NATIVE_CALL_6(fn, env, recv, w) \
    ((fn)((env), (recv), (w)[0], (w)[1], (w)[2], (w)[3], (w)[4], (w)[5]))
#define JNI_NATIVE_CALL_7(fn, env, recv, w) \
    ((fn)((env), (recv), (w)[0], (w)[1], (w)[2], (w)[3], (w)[4], (w)[5], \
          (w)[6]))
#define JNI_NATIVE_CALL_8(fn, env, recv, w) \
    ((fn)((env), (recv), (w)[0], (w)[1], (w)[2], (w)[3], (w)[4], (w)[5], \
          (w)[6], (w)[7]))
#define JNI_NATIVE_CALL_9(fn, env, recv, w) \
    ((fn)((env), (recv), (w)[0], (w)[1], (w)[2], (w)[3], (w)[4], (w)[5], \
          (w)[6], (w)[7], (w)[8]))
#define JNI_NATIVE_CALL_10(fn, env, recv, w) \
    ((fn)((env), (recv), (w)[0], (w)[1], (w)[2], (w)[3], (w)[4], (w)[5], \
          (w)[6], (w)[7], (w)[8], (w)[9]))
#define JNI_NATIVE_CALL_11(fn, env, recv, w) \
    ((fn)((env), (recv), (w)[0], (w)[1], (w)[2], (w)[3], (w)[4], (w)[5], \
          (w)[6], (w)[7], (w)[8], (w)[9], (w)[10]))
#define JNI_NATIVE_CALL_12(fn, env, recv, w) \
    ((fn)((env), (recv), (w)[0], (w)[1], (w)[2], (w)[3], (w)[4], (w)[5], \
          (w)[6], (w)[7], (w)[8], (w)[9], (w)[10], (w)[11]))
#define JNI_NATIVE_CALL_13(fn, env, recv, w) \
    ((fn)((env), (recv), (w)[0], (w)[1], (w)[2], (w)[3], (w)[4], (w)[5], \
          (w)[6], (w)[7], (w)[8], (w)[9], (w)[10], (w)[11], (w)[12]))
#define JNI_NATIVE_CALL_14(fn, env, recv, w) \
    ((fn)((env), (recv), (w)[0], (w)[1], (w)[2], (w)[3], (w)[4], (w)[5], \
          (w)[6], (w)[7], (w)[8], (w)[9], (w)[10], (w)[11], (w)[12], (w)[13]))
#define JNI_NATIVE_CALL_15(fn, env, recv, w) \
    ((fn)((env), (recv), (w)[0], (w)[1], (w)[2], (w)[3], (w)[4], (w)[5], \
          (w)[6], (w)[7], (w)[8], (w)[9], (w)[10], (w)[11], (w)[12], (w)[13], \
          (w)[14]))
#define JNI_NATIVE_CALL_16(fn, env, recv, w) \
    ((fn)((env), (recv), (w)[0], (w)[1], (w)[2], (w)[3], (w)[4], (w)[5], \
          (w)[6], (w)[7], (w)[8], (w)[9], (w)[10], (w)[11], (w)[12], (w)[13], \
          (w)[14], (w)[15]))

static uintptr_t native_call_words(jni_native_va_fn fn, JNIEnv *env,
                                   uintptr_t receiver, const uintptr_t *words,
                                   int nargs)
{
    switch (nargs) {
    case 0:  return JNI_NATIVE_CALL_0(fn, env, receiver);
    case 1:  return JNI_NATIVE_CALL_1(fn, env, receiver, words);
    case 2:  return JNI_NATIVE_CALL_2(fn, env, receiver, words);
    case 3:  return JNI_NATIVE_CALL_3(fn, env, receiver, words);
    case 4:  return JNI_NATIVE_CALL_4(fn, env, receiver, words);
    case 5:  return JNI_NATIVE_CALL_5(fn, env, receiver, words);
    case 6:  return JNI_NATIVE_CALL_6(fn, env, receiver, words);
    case 7:  return JNI_NATIVE_CALL_7(fn, env, receiver, words);
    case 8:  return JNI_NATIVE_CALL_8(fn, env, receiver, words);
    case 9:  return JNI_NATIVE_CALL_9(fn, env, receiver, words);
    case 10: return JNI_NATIVE_CALL_10(fn, env, receiver, words);
    case 11: return JNI_NATIVE_CALL_11(fn, env, receiver, words);
    case 12: return JNI_NATIVE_CALL_12(fn, env, receiver, words);
    case 13: return JNI_NATIVE_CALL_13(fn, env, receiver, words);
    case 14: return JNI_NATIVE_CALL_14(fn, env, receiver, words);
    case 15: return JNI_NATIVE_CALL_15(fn, env, receiver, words);
    case 16: return JNI_NATIVE_CALL_16(fn, env, receiver, words);
    default: return 0;
    }
}

static bool native_kind_is_float(jni_kind kind)
{
    return kind == JNI_KIND_FLOAT || kind == JNI_KIND_DOUBLE;
}

static void thread_clear_pending(struct jni_bridge *b)
{
    struct jni_thread_env *t = g_thread_env;
    if (!t || t->bridge != b) return;
    if (t->pending && t->pending_pinned) dvm_unpin(b->vm, t->pending);
    t->pending = 0;
    t->pending_pinned = false;
}

static _Thread_local jni_native_observer g_native_observer;
static _Thread_local void *g_native_observer_data;
int jni_bridge_with_native_observer(jni_native_observer observer,void *userdata,
 jni_native_observed_body body,void *bodydata)
{
 if(!body)return -1;
 jni_native_observer old=g_native_observer;void *old_data=g_native_observer_data;
 g_native_observer=observer;g_native_observer_data=userdata;
 int result=body(bodydata);
 g_native_observer=old;g_native_observer_data=old_data;
 return result;
}

#include "gil_long_selftest.inc"

bool jni_bridge_call_native(void *user, struct dvm *vm,
                            const char *class_name, const char *method,
                            const char *sig, bool is_static, dvm_ref self,
                            const union dvm_value *args, int nargs,
                            union dvm_value *out)
{
    (void)user;
    if (out) memset(out, 0, sizeof(*out));
    struct jni_bridge *b = bridge_from_any((void *)jni_bridge_for_vm(vm));
    if (!b || !method || !sig || !class_name) return false;

    bool gil_owned = false;
    if (!dvm_gil_held(vm)) {
        dvm_gil_acquire(vm);
        gil_owned = true;
    }

    struct jni_thread_env *t = thread_ensure(b);
    if (!t) {
        if (gil_owned) dvm_gil_release(vm);
        return false;
    }
    /* 通道已能接管：后续所有失败都通过 pending 异常交回 DVM。 */
    bool handled = true;
    uint32_t frame = 0;
    jni_sig_info info;
    struct dvm_class *decl = NULL;
    void *sym = NULL;
    jni_native_va_fn fn = NULL;
    uintptr_t words[JNI_NATIVE_MAX_ARGS];
    uintptr_t receiver = 0;

    if (t->pending) {
        /* 已有未处理异常时不再执行 native，交给 take_pending_exception。 */
        {char why[320]="-";dvm_describe_exception(vm,t->pending,why,sizeof why);
         FILE *df=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt","a");
         if(df){fprintf(df,"JNI_NATIVE_SKIPPED_PENDING class=%s method=%s sig=%s pending=%u why=%s\n",class_name,method,sig,(unsigned)t->pending,why);fclose(df);}}
        goto done;
    }
    if (nargs > JNI_NATIVE_MAX_ARGS) {
        bridge_set_pending_new(b, "java/lang/UnsatisfiedLinkError",
                               "native 方法参数超过 16 个，桥不支持");
        goto done;
    }
    if (!jni_sig_parse(sig, &info) || info.nargs != nargs) {
        bridge_set_pending_new(b, "java/lang/UnsatisfiedLinkError",
                               "native 方法签名与参数数量不一致");
        goto done;
    }
    if (native_kind_is_float(info.ret)) {
        bridge_set_pending_new(b, "java/lang/UnsatisfiedLinkError",
                               "native 返回 float/double 暂不支持");
        goto done;
    }
    for (int i = 0; i < info.nargs; ++i) {
        if (native_kind_is_float(info.args[i].kind)) {
            bridge_set_pending_new(b, "java/lang/UnsatisfiedLinkError",
                                   "native 参数含 float/double，桥暂不支持");
            goto done;
        }
    }
    decl = dvm_find_class_or_external(vm, class_name);
    if (!decl) {
        bridge_set_pending_new(b, "java/lang/NoClassDefFoundError",
                               class_name);
        goto done;
    }
    struct dvm_method *m = dvm_find_method(vm, decl, method, sig);
    if (!m || m->cls != decl) {
        bridge_set_pending_new(b, "java/lang/UnsatisfiedLinkError",
                               "native 方法声明未找到");
        goto done;
    }
    if (((m->access & DEX_ACC_STATIC) != 0) != is_static) {
        bridge_set_pending_new(b, "java/lang/UnsatisfiedLinkError",
                               "native 方法 static 属性不一致");
        goto done;
    }
    sym = jni_native_find(b->natives, class_name, method, sig);
    if (!sym) {
        char msg[JNI_ERROR_MAX];
        snprintf(msg, sizeof(msg), "找不到 native 符号：%s.%s%s", class_name,
                 method, sig);
        bridge_set_pending_new(b, "java/lang/UnsatisfiedLinkError", msg);
        goto done;
    }
    fn = native_fn_from_ptr(sym);
    if (!fn) {
        bridge_set_pending_new(b, "java/lang/UnsatisfiedLinkError",
                               "native 函数指针尺寸不兼容");
        goto done;
    }

    if (t->frame_depth >= (int)JNI_MAX_THREAD_FRAMES) {
        bridge_set_pending_new(b, "java/lang/StackOverflowError",
                               "JNI local frame 深度超过 64");
        goto done;
    }
    frame = next_frame_id(b);
    if (frame == 0) {
        bridge_set_pending_new(b, "java/lang/OutOfMemoryError",
                               "JNI frame id 分配失败");
        goto done;
    }
    t->frames[t->frame_depth++] = frame;

    if (is_static) {
        jobject cls_token = new_local_class(t, decl);
        if (jni_token_is_null(cls_token)) goto done;
        receiver = (uintptr_t)jni_token_raw(cls_token);
    } else {
        if (self == 0) {
            bridge_set_pending_new(b, "java/lang/NullPointerException",
                                   "实例 native 方法 receiver 为 null");
            goto done;
        }
        jobject self_token = new_local_obj(t, self);
        if (jni_token_is_null(self_token)) goto done;
        receiver = (uintptr_t)jni_token_raw(self_token);
    }
    for (int i = 0; i < info.nargs; ++i) {
        jni_kind k = info.args[i].kind;
        if (k == JNI_KIND_OBJECT || k == JNI_KIND_ARRAY) {
            dvm_ref ref = args ? args[i].u : 0;
            if (ref == 0) {
                words[i] = 0;
            } else {
                jobject token = new_local_obj(t, ref);
                if (jni_token_is_null(token)) goto done;
                words[i] = (uintptr_t)jni_token_raw(token);
            }
        } else if (k == JNI_KIND_LONG) {
            words[i] = (uintptr_t)(args ? args[i].ju : 0);
        } else {
            words[i] = (uintptr_t)(uint32_t)(args ? args[i].u : 0);
        }
    }

    bool trace_native = !strcmp(class_name,"com/unity3d/player/UnityPlayer") || strstr(class_name,"NativeLoader")!=NULL;
#ifdef LUNARIA_UNITY_PLAYER_PROBE
    if(!strcmp(class_name,"com/unity3d/player/UnityPlayer")&&!strcmp(method,"nativeUnitySendMessage")&&args&&nargs==3){
        struct dvm *vm=b->vm;
        struct gc_operation gc6_message __attribute__((cleanup(gc_operation_end)));
        if(!gc_operation_begin(vm,&gc6_message))goto done;
        const char *object=dvm_string_utf8(vm,args[0].l),*callback=dvm_string_utf8(vm,args[1].l);
        struct dvm_object *payload=dvm__obj(vm,args[2].l);uint32_t bytes=0;const unsigned char *data=NULL;
        if(payload&&payload->kind==DVM_OBJ_ARRAY&&payload->elem_kind=='B'){bytes=dvm_array_length(vm,args[2].l);data=dvm_array_data(vm,args[2].l);}
        const char expected[]="{\"sendType\":\"Sdk_Inited\",\"sendCode\":0,\"sendData\":\"\"}";
        int expected_init=data&&bytes==sizeof expected-1&&!memcmp(data,expected,sizeof expected-1);
        FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt","a");
        if(f){fprintf(f,"SDK_MESSAGE object=%s callback=%s bytes=%u expected_sdk_init=%d head=%02x%02x%02x%02x java_thread=%u\n",object?object:"-",callback?callback:"-",bytes,expected_init,data&&bytes>0?data[0]:0,data&&bytes>1?data[1]:0,data&&bytes>2?data[2]:0,data&&bytes>3?data[3]:0,vm->cur_thread);fclose(f);}
    }
#endif
    if(trace_native&&perf_trace_level()>=2){FILE*df=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt","a");if(df){fprintf(df,"NATIVE_CALL class=%s method=%s sym=%p entering=1\n",class_name,method,sym);fclose(df);}}
#ifdef LUNARIA_UNITY_PLAYER_PROBE
    { extern void unity_player_probe_on_native_render(void);
      if(trace_native && method){
        int hit=!strcmp(method,"nativeRender");
        FILE*hf=perf_trace_level()>=2?fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt","a"):NULL;
        if(hf){fprintf(hf,"NATIVE_RENDER_HOOK method=%s hit=%d\n",method,hit);fclose(hf);}
        if(hit) unity_player_probe_on_native_render();
      } }
#endif
#ifdef LUNARIA_UNITY_PLAYER_PROBE
    bool launch_init=!strcmp(class_name,"com/unity3d/player/UnityPlayer")&&!strcmp(method,"initJni")&&!strcmp(sig,"(Landroid/content/Context;)V");
    extern void unity_player_probe_init_jni(int,dvm_ref,dvm_ref);
    if(launch_init)unity_player_probe_init_jni(1,self,args&&nargs?args[0].l:0);
#endif
    if(g_native_observer&&leak_scope_current)leak_scope_cancel(vm,LEAK_OBSERVER);
    if(g_native_observer && g_native_observer(class_name,method,sig,self,args,nargs,1,(uintptr_t)sym,g_native_observer_data)) abort();
    bool perf_render=!strcmp(class_name,"com/unity3d/player/UnityPlayer")&&!strcmp(method,"nativeRender");
    if(perf_render){extern void dvm_gil_render_thread(void);dvm_gil_render_thread();extern void sdk_input_frame_touch(struct dvm *) __attribute__((weak));if(sdk_input_frame_touch)sdk_input_frame_touch(b->vm);}
    struct perf_external perf_external=perf_external_begin();
    if(perf_render)perf_frame_begin(); /* under GIL: cannot race a collector entry */
    unsigned depth = jni_bridge_gil_drop(b);
    if(perf_render)perf_result_event(3,receiver,0);
#ifdef LUNARIA_TASK8A
    extern void unity_player_probe_native_phase(const char *,const char *,int);
    unity_player_probe_native_phase(class_name,method,1);
#endif
    uintptr_t raw = native_call_words(fn, (JNIEnv *)(void *)t, receiver,
                                      words, info.nargs);
    if(perf_render){perf_result_event(4,receiver,raw);perf_frame_end();}
    perf_external_end(&perf_external);
#ifdef LUNARIA_TASK8A
    unity_player_probe_native_phase(class_name,method,2);
#endif
    jni_bridge_gil_restore(b, depth);
    if(!strcmp(class_name,"com/unity3d/player/UnityPlayer")&&!strcmp(method,"nativeInjectEvent")&&nargs>0&&args){extern void sdk_input_native_touch_done(struct dvm *,dvm_ref,int) __attribute__((weak));if(sdk_input_native_touch_done)sdk_input_native_touch_done(b->vm,args[0].l,(int)raw);}
    if(perf_render){extern void dvm_gc_frame_boundary(struct dvm *);dvm_gc_frame_boundary(b->vm);}
#ifdef LUNARIA_TASK8A
    unity_player_probe_native_phase(class_name,method,3);
#endif
#ifdef LUNARIA_UNITY_PLAYER_PROBE
    if(!t->pending&&!strcmp(class_name,"com/unity3d/player/UnityPlayer")&&!strcmp(method,"nativeRender")){
        extern void unity_player_probe_observe_title_state(void);
        extern void dvm_gil_deep_host(struct dvm *,const char *,int);
        dvm_gil_deep_host(b->vm,"unity_player_probe_observe_title_state",1);
#ifdef LUNARIA_TASK6F_PREFLIGHT
        unsigned observation_depth=jni_bridge_gil_drop(b);
#endif
        unity_player_probe_observe_title_state();
#ifdef LUNARIA_TASK6F_PREFLIGHT
        jni_bridge_gil_restore(b,observation_depth);
#endif
        dvm_gil_deep_host(b->vm,"unity_player_probe_observe_title_state",0);
    }
#endif
    if(trace_native&&perf_trace_level()>=2){FILE*df=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt","a");if(df){fprintf(df,"NATIVE_CALL class=%s method=%s entering=0 ret_low=%d\n",class_name,method,(int)(raw&0xff));fclose(df);}}
#ifdef LUNARIA_UNITY_PLAYER_PROBE
    if(launch_init)unity_player_probe_init_jni(0,self,args&&nargs?args[0].l:0);
#endif
    leak_scope_raw_observers(vm,g_native_observer!=NULL);
    if(g_native_observer && g_native_observer(class_name,method,sig,self,args,nargs,0,(uintptr_t)sym,g_native_observer_data)) abort();

    if (t->pending) {
        /* native 内 Throw/ThrowNew 或 JNI 回调失败：返回值按 0 处理。 */
        goto done;
    }
    switch (info.ret) {
    case JNI_KIND_VOID:
        break;
    case JNI_KIND_BOOLEAN:
        out->i = (raw & 0xffu) ? 1 : 0;
        break;
    case JNI_KIND_BYTE:
        out->i = (int8_t)(uint8_t)raw;
        break;
    case JNI_KIND_CHAR:
        out->i = (uint16_t)raw;
        break;
    case JNI_KIND_SHORT:
        out->i = (int16_t)(uint16_t)raw;
        break;
    case JNI_KIND_INT:
        out->i = (int32_t)(uint32_t)raw;
        break;
    case JNI_KIND_LONG:
        out->j = (int64_t)(uint64_t)raw;
        break;
    case JNI_KIND_OBJECT:
    case JNI_KIND_ARRAY: {
        dvm_ref ref = 0;
        if (raw != 0) {
            if (!token_to_dvm_ref(t, (jobject)raw, &ref) || ref == 0) {
                bridge_set_pending_new(b, "java/lang/UnsatisfiedLinkError",
                                       "native 返回了无效的 JNI 引用");
                memset(out, 0, sizeof(*out));
                goto done;
            }
        }
        out->l = ref;
        break;
    }
    default:
        bridge_set_pending_new(b, "java/lang/UnsatisfiedLinkError",
                               "native 返回类型不支持");
        memset(out, 0, sizeof(*out));
        break;
    }
done:
    if (frame != 0) {
        if (t->frame_depth > 1 &&
            t->frames[t->frame_depth - 1] == frame) {
            jni_handle_pop_frame_slots(&b->handles, frame);
            t->frames[t->frame_depth - 1] = 0;
            --t->frame_depth;
        }
    }
    if (t->pending && out) memset(out, 0, sizeof(*out));
    if (gil_owned) dvm_gil_release(vm);
    return handled;
}

dvm_ref jni_bridge_take_pending_exception(void *user, struct dvm *vm)
{
    (void)user;
    struct jni_bridge *b = jni_bridge_for_vm(vm);
    if (!b) return 0;
    struct jni_thread_env *t = g_thread_env;
    if (!t || t->bridge != b) return 0;
    dvm_ref e = t->pending;
    if (!e) return 0;
    t->pending = 0;
    if (t->pending_pinned) {
        dvm_unpin(vm, e);
        t->pending_pinned = false;
    }
    return e;
}

bool jni_bridge_native_registered(struct dvm *vm, const char *class_name,
 const char *method, const char *sig)
{
    struct jni_bridge *b = jni_bridge_for_vm(vm);
    return b && jni_native_is_registered(b->natives, class_name, method, sig);
}

uint32_t jni_bridge_native_registration_count(struct dvm *vm)
{
    struct jni_bridge *b = jni_bridge_for_vm(vm);
    return b ? jni_native_registration_count(b->natives) : 0;
}

bool jni_bridge_load_library(void *user, struct dvm *vm, const char *name)
{
    (void)user;
    struct jni_bridge *b = jni_bridge_for_vm(vm);
    if (!b || !b->natives || !name) return false;
    /* DVM 的 sys_loadLibrary 在 hook 返回 false 时会自己抛
     * UnsatisfiedLinkError；成功路径不会取 pending。因此失败或 OnLoad
     * 留下异常时必须清掉桥内 pending，避免它污染下一次 JNI 调用。 */
    if (!jni_native_load_name(b->natives, name)) {
        jni_bridge_set_last_error(b, "loadLibrary(%s) 失败：%s", name,
                                  jni_native_last_error(b->natives));
        thread_clear_pending(b);
        return false;
    }
    if (jni_bridge_pending(b)) {
        jni_bridge_set_last_error(b, "JNI_OnLoad(%s) 留下了未处理异常", name);
        thread_clear_pending(b);
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* 真实槽安装表                                                        */
/* ------------------------------------------------------------------ */

#define INSTALL_CALLS(NAME, BRIDGE_NAME)                                     \
    do {                                                                     \
        b->env_table.Call##NAME##Method = bridge_Call##NAME##Method;         \
        b->env_table.Call##NAME##MethodV = bridge_Call##NAME##MethodV;       \
        b->env_table.Call##NAME##MethodA = bridge_Call##NAME##MethodA;       \
        b->env_table.CallNonvirtual##NAME##Method =                          \
            bridge_CallNonvirtual##NAME##Method;                             \
        b->env_table.CallNonvirtual##NAME##MethodV =                         \
            bridge_CallNonvirtual##NAME##MethodV;                            \
        b->env_table.CallNonvirtual##NAME##MethodA =                         \
            bridge_CallNonvirtual##NAME##MethodA;                            \
        b->env_table.CallStatic##NAME##Method =                              \
            bridge_CallStatic##NAME##Method;                                 \
        b->env_table.CallStatic##NAME##MethodV =                             \
            bridge_CallStatic##NAME##MethodV;                                \
        b->env_table.CallStatic##NAME##MethodA =                             \
            bridge_CallStatic##NAME##MethodA;                                \
        (void)BRIDGE_NAME;                                                   \
    } while (0)

#define INSTALL_FIELDS(NAME)                                                 \
    do {                                                                     \
        b->env_table.Get##NAME##Field = bridge_Get##NAME##Field;             \
        b->env_table.Set##NAME##Field = bridge_Set##NAME##Field;             \
        b->env_table.GetStatic##NAME##Field = bridge_GetStatic##NAME##Field; \
        b->env_table.SetStatic##NAME##Field = bridge_SetStatic##NAME##Field; \
    } while (0)

#define INSTALL_ARRAYS(NAME)                                                 \
    do {                                                                     \
        b->env_table.New##NAME##Array = bridge_New##NAME##Array;             \
        b->env_table.Get##NAME##ArrayElements =                              \
            bridge_Get##NAME##ArrayElements;                                 \
        b->env_table.Release##NAME##ArrayElements =                          \
            bridge_Release##NAME##ArrayElements;                             \
        b->env_table.Get##NAME##ArrayRegion =                                \
            bridge_Get##NAME##ArrayRegion;                                   \
        b->env_table.Set##NAME##ArrayRegion =                                \
            bridge_Set##NAME##ArrayRegion;                                   \
    } while (0)

static void bridge_install_real_slots(struct jni_bridge *b)
{
    b->env_table.GetVersion = bridge_GetVersion;
    b->env_table.FindClass = bridge_FindClass;
    b->env_table.GetSuperclass = bridge_GetSuperclass;
    b->env_table.IsAssignableFrom = bridge_IsAssignableFrom;
    b->env_table.FromReflectedMethod = bridge_FromReflectedMethod;
    b->env_table.FromReflectedField = bridge_FromReflectedField;
    b->env_table.ToReflectedMethod = bridge_ToReflectedMethod;
    b->env_table.Throw = bridge_Throw;
    b->env_table.ThrowNew = bridge_ThrowNew;
    b->env_table.ExceptionOccurred = bridge_ExceptionOccurred;
    b->env_table.ExceptionDescribe = bridge_ExceptionDescribe;
    b->env_table.ExceptionClear = bridge_ExceptionClear;
    b->env_table.FatalError = bridge_FatalError;
    b->env_table.ExceptionCheck = bridge_ExceptionCheck;
    b->env_table.PushLocalFrame = bridge_PushLocalFrame;
    b->env_table.PopLocalFrame = bridge_PopLocalFrame;
    b->env_table.NewGlobalRef = bridge_NewGlobalRef;
    b->env_table.DeleteGlobalRef = bridge_DeleteGlobalRef;
    b->env_table.DeleteLocalRef = bridge_DeleteLocalRef;
    b->env_table.IsSameObject = bridge_IsSameObject;
    b->env_table.NewLocalRef = bridge_NewLocalRef;
    b->env_table.EnsureLocalCapacity = bridge_EnsureLocalCapacity;
    b->env_table.AllocObject = bridge_AllocObject;
    b->env_table.NewObject = bridge_NewObject;
    b->env_table.NewObjectV = bridge_NewObjectV;
    b->env_table.NewObjectA = bridge_NewObjectA;
    b->env_table.GetObjectClass = bridge_GetObjectClass;
    b->env_table.IsInstanceOf = bridge_IsInstanceOf;
    b->env_table.GetMethodID = bridge_GetMethodID;
    b->env_table.GetFieldID = bridge_GetFieldID;
    b->env_table.GetStaticMethodID = bridge_GetStaticMethodID;
    b->env_table.GetStaticFieldID = bridge_GetStaticFieldID;

    INSTALL_CALLS(Object, bridge_CallObjectMethod);
    INSTALL_CALLS(Boolean, bridge_CallBooleanMethod);
    INSTALL_CALLS(Byte, bridge_CallByteMethod);
    INSTALL_CALLS(Char, bridge_CallCharMethod);
    INSTALL_CALLS(Short, bridge_CallShortMethod);
    INSTALL_CALLS(Int, bridge_CallIntMethod);
    INSTALL_CALLS(Long, bridge_CallLongMethod);
    INSTALL_CALLS(Float, bridge_CallFloatMethod);
    INSTALL_CALLS(Double, bridge_CallDoubleMethod);

    b->env_table.CallVoidMethod = bridge_CallVoidMethod;
    b->env_table.CallVoidMethodV = bridge_CallVoidMethodV;
    b->env_table.CallVoidMethodA = bridge_CallVoidMethodA;
    b->env_table.CallNonvirtualVoidMethod = bridge_CallNonvirtualVoidMethod;
    b->env_table.CallNonvirtualVoidMethodV = bridge_CallNonvirtualVoidMethodV;
    b->env_table.CallNonvirtualVoidMethodA = bridge_CallNonvirtualVoidMethodA;
    b->env_table.CallStaticVoidMethod = bridge_CallStaticVoidMethod;
    b->env_table.CallStaticVoidMethodV = bridge_CallStaticVoidMethodV;
    b->env_table.CallStaticVoidMethodA = bridge_CallStaticVoidMethodA;

    INSTALL_FIELDS(Object);
    INSTALL_FIELDS(Boolean);
    INSTALL_FIELDS(Byte);
    INSTALL_FIELDS(Char);
    INSTALL_FIELDS(Short);
    INSTALL_FIELDS(Int);
    INSTALL_FIELDS(Long);
    INSTALL_FIELDS(Float);
    INSTALL_FIELDS(Double);

    b->env_table.NewString = bridge_NewString;
    b->env_table.GetStringLength = bridge_GetStringLength;
    b->env_table.GetStringChars = bridge_GetStringChars;
    b->env_table.ReleaseStringChars = bridge_ReleaseStringChars;
    b->env_table.NewStringUTF = bridge_NewStringUTF;
    b->env_table.GetStringUTFLength = bridge_GetStringUTFLength;
    b->env_table.GetStringUTFChars = bridge_GetStringUTFChars;
    b->env_table.ReleaseStringUTFChars = bridge_ReleaseStringUTFChars;
    b->env_table.GetStringRegion = bridge_GetStringRegion;
    b->env_table.GetStringUTFRegion = bridge_GetStringUTFRegion;
    b->env_table.GetStringCritical = bridge_GetStringCritical;
    b->env_table.ReleaseStringCritical = bridge_ReleaseStringCritical;

    b->env_table.GetArrayLength = bridge_GetArrayLength;
    b->env_table.NewObjectArray = bridge_NewObjectArray;
    b->env_table.GetObjectArrayElement = bridge_GetObjectArrayElement;
    b->env_table.SetObjectArrayElement = bridge_SetObjectArrayElement;
    INSTALL_ARRAYS(Boolean);
    INSTALL_ARRAYS(Byte);
    INSTALL_ARRAYS(Char);
    INSTALL_ARRAYS(Short);
    INSTALL_ARRAYS(Int);
    INSTALL_ARRAYS(Long);
    INSTALL_ARRAYS(Float);
    INSTALL_ARRAYS(Double);
    b->env_table.GetPrimitiveArrayCritical = bridge_GetPrimitiveArrayCritical;
    b->env_table.ReleasePrimitiveArrayCritical =
        bridge_ReleasePrimitiveArrayCritical;

    b->env_table.RegisterNatives = bridge_RegisterNatives;
    b->env_table.UnregisterNatives = bridge_UnregisterNatives;
    b->env_table.MonitorEnter = bridge_MonitorEnter;
    b->env_table.MonitorExit = bridge_MonitorExit;
    b->env_table.GetJavaVM = bridge_GetJavaVM;
    b->env_table.NewWeakGlobalRef = bridge_NewWeakGlobalRef;
    b->env_table.DeleteWeakGlobalRef = bridge_DeleteWeakGlobalRef;
    b->env_table.GetObjectRefType = bridge_GetObjectRefType;
}

#undef INSTALL_CALLS
#undef INSTALL_FIELDS
#undef INSTALL_ARRAYS

/* ==== 文件末尾：桥主体实现完毕 ==== */
#include "jni_gc_selftest.inc"
