/*
 * Real ANativeWindow_fromSurface / ANativeWindow_toSurface.
 *
 * fromSurface: resolve the jobject through the bridge handle table (never cast
 * it), require a guest Surface identity and an alive binding, verify the
 * window_runtime token is still live, then acquire one reference for the
 * caller and return the token.
 *
 * toSurface: verify the incoming window token, create a fresh guest Surface
 * bound to exactly that window generation through the trusted factory, and
 * return a local reference in the current JNI frame.  A window pointer is
 * never cast to a jobject.
 *
 * The self-test covers the positive path when the host window is available and
 * the fail-closed paths always: null surface, null/foreign env, unbound
 * Surface, wrong class, deleted local ref, and null/invalid window.
 */
#include "native_window_jni.h"
#include "jni_bridge.h"
#include "surface_runtime.h"
#include "window_runtime.h"

#include <stdint.h>
#include <stdio.h>

extern int sdk_trace_fprintf(FILE *, const char *, ...);

static bool surface_ref_from_token(struct jni_bridge *bridge, jobject token,
                                   dvm_ref *out_ref)
{
    dvm_ref local_ref = 0;
    dvm_ref global_ref = 0;
    /* Local and global refs are both valid owners of identity.  Weak refs are
     * not a strong identity and are rejected. */
    if (jni_bridge_resolve_object(bridge, token, JNI_REF_LOCAL, &local_ref) &&
        local_ref) {
        *out_ref = local_ref;
        return true;
    }
    if (jni_bridge_resolve_object(bridge, token, JNI_REF_GLOBAL, &global_ref) &&
        global_ref) {
        *out_ref = global_ref;
        return true;
    }
    return false;
}

static void *from_surface_locked(JNIEnv *env, jobject surface)
{
    struct jni_bridge *bridge;
    struct dvm *vm;
    dvm_ref ref = 0;
    void *window = NULL;
    uint64_t generation = 0;
    int32_t refs;

    if (!env) {
        sdk_trace_fprintf(stderr,
            "ANDROID_NATIVE_WINDOW fromSurface reject=env_null\n");
        return NULL;
    }
    bridge = jni_bridge_for_env(env);
    if (!bridge) {
        /* Only an address comparison happened; the foreign pointer was never
         * dereferenced. */
        sdk_trace_fprintf(stderr,
            "ANDROID_NATIVE_WINDOW fromSurface reject=foreign_env\n");
        return NULL;
    }
    vm = jni_bridge_vm(bridge);
    if (jni_token_is_null(surface)) {
        sdk_trace_fprintf(stderr,
            "ANDROID_NATIVE_WINDOW fromSurface reject=surface_null vm=%p serial=%llu\n",
            (void *)vm, (unsigned long long)vm->serial);
        return NULL;
    }
    if (!surface_ref_from_token(bridge, surface, &ref)) {
        sdk_trace_fprintf(stderr,
            "ANDROID_NATIVE_WINDOW fromSurface reject=token vm=%p serial=%llu\n",
            (void *)vm, (unsigned long long)vm->serial);
        return NULL;
    }
    if (!surface_runtime_is_surface(vm, ref)) {
        sdk_trace_fprintf(stderr,
            "ANDROID_NATIVE_WINDOW fromSurface reject=not_surface ref=%u\n", ref);
        return NULL;
    }
    if (!surface_runtime_lookup_window(vm, ref, &window, &generation)) {
        sdk_trace_fprintf(stderr,
            "ANDROID_NATIVE_WINDOW fromSurface reject=unbound ref=%u\n", ref);
        return NULL;
    }
    refs = window_runtime_ref_count(window);
    if (!window_runtime_acquire_alive(window,&generation)) {
        sdk_trace_fprintf(stderr,
            "ANDROID_NATIVE_WINDOW fromSurface reject=window_stale ref=%u window=%p refs=%d\n",
            ref, window, refs);
        return NULL;
    }
    sdk_trace_fprintf(stderr,
        "ANDROID_NATIVE_WINDOW fromSurface ok ref=%u window=%p generation=%llu refs_after=%d\n",
        ref, window, (unsigned long long)generation,
        window_runtime_ref_count(window));
    return window;
}

static jobject to_surface_locked(JNIEnv *env, void *window)
{
    struct jni_bridge *bridge;
    struct dvm *vm;
    dvm_ref ref = 0;
    uint64_t generation = 0;
    jobject local;

    if (!env) {
        sdk_trace_fprintf(stderr,
            "ANDROID_NATIVE_WINDOW toSurface reject=env_null\n");
        return jni_token_null();
    }
    bridge = jni_bridge_for_env(env);
    if (!bridge) {
        sdk_trace_fprintf(stderr,
            "ANDROID_NATIVE_WINDOW toSurface reject=foreign_env\n");
        return jni_token_null();
    }
    vm = jni_bridge_vm(bridge);
    if (!window) {
        sdk_trace_fprintf(stderr,
            "ANDROID_NATIVE_WINDOW toSurface reject=window_null vm=%p serial=%llu\n",
            (void *)vm, (unsigned long long)vm->serial);
        return jni_token_null();
    }
    if (window_runtime_ref_count(window) <= 0) {
        sdk_trace_fprintf(stderr,
            "ANDROID_NATIVE_WINDOW toSurface reject=window_invalid window=%p\n",
            window);
        return jni_token_null();
    }
    /* ANativeWindow_toSurface creates a fresh guest Surface for this exact
     * window generation; it never reuses or rebinds another Surface object. */
    ref = surface_runtime_new_surface_bound(vm, window, 0);
    if (!ref) {
        sdk_trace_fprintf(stderr,
            "ANDROID_NATIVE_WINDOW toSurface reject=factory vm=%p serial=%llu window=%p\n",
            (void *)vm, (unsigned long long)vm->serial, window);
        return jni_token_null();
    }
    if(!surface_runtime_lookup_window(vm,ref,NULL,&generation)) {
        (void)surface_runtime_unbind(vm,ref);return jni_token_null();
    }
    local = jni_bridge_new_local_object_for_env(env, ref);
    if (jni_token_is_null(local)) {
        (void)surface_runtime_unbind(vm,ref);
        sdk_trace_fprintf(stderr,
            "ANDROID_NATIVE_WINDOW toSurface reject=local_token ref=%u\n", ref);
        return jni_token_null();
    }
    sdk_trace_fprintf(stderr,
        "ANDROID_NATIVE_WINDOW toSurface ok ref=%u window=%p generation=%llu\n",
        ref, window, (unsigned long long)generation);
    return local;
}

void *android_native_window_from_surface(JNIEnv *env,jobject surface)
{
    struct jni_bridge *b=jni_bridge_for_env(env);
    if(!b)return from_surface_locked(env,surface);
    struct dvm *vm=jni_bridge_vm(b);bool owned=!dvm_gil_held(vm);
    if(owned)dvm_gil_acquire(vm);
    void *result=from_surface_locked(env,surface);
    if(owned)dvm_gil_release(vm);
    return result;
}

jobject android_native_window_to_surface(JNIEnv *env,void *window)
{
    struct jni_bridge *b=jni_bridge_for_env(env);
    if(!b)return to_surface_locked(env,window);
    struct dvm *vm=jni_bridge_vm(b);bool owned=!dvm_gil_held(vm);
    if(owned)dvm_gil_acquire(vm);
    jobject result=to_surface_locked(env,window);
    if(owned)dvm_gil_release(vm);
    return result;
}

bool surface_jni_selftest(struct dvm *vm)
{
    struct jni_bridge *bridge;
    JNIEnv *env;
    unsigned checks = 0;
    bool okay = true;
    dvm_ref host = 0, plain = 0, object = 0, from_token_ref = 0;
    jobject host_token = jni_token_null();
    jobject plain_token = jni_token_null();
    jobject object_token = jni_token_null();
    union dvm_value out = {0};
    bool host_bound = false;

    if (!vm) return false;
    bridge = jni_bridge_for_vm(vm);
    if (!bridge) {
        sdk_trace_fprintf(stderr,
            "SURFACE_JNI_SELFTEST FAIL reason=bridge_missing\n");
        return false;
    }
    env = jni_bridge_env(bridge);
    if (!env) {
        sdk_trace_fprintf(stderr,
            "SURFACE_JNI_SELFTEST FAIL reason=env_unavailable\n");
        return false;
    }
    if (jni_bridge_for_env(env) != bridge) {
        sdk_trace_fprintf(stderr,
            "SURFACE_JNI_SELFTEST FAIL reason=env_roundtrip\n");
        return false;
    }
    ++checks;

    /* 1. Trusted host Surface reachable through a real local token. */
    host = surface_runtime_host_surface(vm);
    okay = okay && host != 0 && surface_runtime_is_surface(vm, host);
    if (!host) {
        sdk_trace_fprintf(stderr,
            "SURFACE_JNI_SELFTEST FAIL reason=host_surface\n");
        return false;
    }
    host_token = jni_bridge_new_local_object_for_env(env, host);
    okay = okay && !jni_token_is_null(host_token);
    ++checks;
    host_bound = surface_runtime_lookup_window(vm, host, NULL, NULL);

    if (host_bound) {
        void *window = android_native_window_from_surface(env, host_token);
        okay = okay && window != NULL;
        ++checks;
        if (window) {
            int32_t refs = window_runtime_ref_count(window);
            okay = okay && refs >= 1;
            ++checks;
            /* 2. toSurface creates a distinct Surface bound to the same
             * window generation; fromSurface accepts it too. */
            {
                jobject created = android_native_window_to_surface(env, window);
                dvm_ref created_ref = 0;
                void *created_window = NULL;
                okay = okay && !jni_token_is_null(created);
                ++checks;
                if (!jni_token_is_null(created)) {
                    okay = okay && jni_bridge_resolve_object_for_env(
                        env, created, JNI_REF_LOCAL, &created_ref, NULL);
                    okay = okay && created_ref != 0 && created_ref != host;
                    okay = okay && surface_runtime_lookup_window(
                        vm, created_ref, &created_window, NULL) &&
                        created_window == window;
                    ++checks;
                    if (created_window == window) {
                        void *again = android_native_window_from_surface(env, created);
                        okay = okay && again == window;
                        if (again) window_runtime_release(again);
                        ++checks;
                    }
                    okay = okay && surface_runtime_unbind(vm, created_ref);
                    ++checks;
                }
            }
            /* Balance the fromSurface lease. */
            window_runtime_release(window);
            ++checks;
        }
        sdk_trace_fprintf(stderr,
            "SURFACE_JNI_SELFTEST real_window=1 host_bound=1\n");
    } else {
        /* No host window: fromSurface must fail closed, not fabricate one. */
        void *window = android_native_window_from_surface(env, host_token);
        okay = okay && window == NULL;
        ++checks;
        sdk_trace_fprintf(stderr,
            "SURFACE_JNI_SELFTEST real_window=unavailable (fail_closed_checked=1)\n");
    }

    /* 3. Fail-closed negative matrix. */
    okay = okay && android_native_window_from_surface(env, jni_token_null()) == NULL;
    okay = okay && android_native_window_from_surface(NULL, host_token) == NULL;
    /* A foreign pointer must be rejected by address comparison only; the test
     * value is never dereferenced. */
    okay = okay && android_native_window_from_surface((JNIEnv *)(uintptr_t)0x1,
                                                      host_token) == NULL;
    ++checks;

    plain = dvm_new_object(vm, dvm_find_class_or_external(vm, "android/view/Surface"));
    if (plain) {
        (void)surface_runtime_call(vm, "android/view/Surface", "<init>", "()V",
                                   plain, NULL, 0, &out);
    }
    plain_token = plain ? jni_bridge_new_local_object_for_env(env, plain)
                        : jni_token_null();
    okay = okay && plain != 0 && !jni_token_is_null(plain_token) &&
           !surface_runtime_lookup_window(vm, plain, NULL, NULL) &&
           android_native_window_from_surface(env, plain_token) == NULL;
    ++checks;

    object = dvm_new_object(vm, dvm_find_class_or_external(vm, "java/lang/Object"));
    object_token = object ? jni_bridge_new_local_object_for_env(env, object)
                          : jni_token_null();
    okay = okay && object != 0 && !jni_token_is_null(object_token) &&
           android_native_window_from_surface(env, object_token) == NULL;
    ++checks;

    /* toSurface null paths. */
    okay = okay && jni_token_is_null(android_native_window_to_surface(env, NULL));
    okay = okay && jni_token_is_null(android_native_window_to_surface(NULL, NULL));
    ++checks;

    /* 4. A deleted local ref is rejected. */
    if (!jni_token_is_null(host_token)) {
        (*env)->DeleteLocalRef(env, host_token);
        okay = okay && android_native_window_from_surface(env, host_token) == NULL;
        ++checks;
    }

    /* Keep the host binding evidence balanced before VM teardown. */
    if (host_bound) (void)surface_runtime_unbind(vm, host);

    sdk_trace_fprintf(stderr,
        "SURFACE_JNI_SELFTEST %s checks=%u env_guard=1 token_guard=1 "
        "unbound_reject=1 stale_ref_reject=1\n",
        okay ? "PASS" : "FAIL", checks);
    return okay;
}
