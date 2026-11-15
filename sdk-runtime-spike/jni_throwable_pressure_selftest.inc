/* TASK7-3c: real JNI exception text under the existing bounded arena pressure.
 * Included by jni_forname_selftest.c; never creates or destroys a VM. */
#include "next_arena.h"
#include "dvm/dvm_internal.h"
#include <stddef.h>
#include <errno.h>
#include <unistd.h>

/* All loans coexist; never accept success from merely enlarging a fixed cap.
 * Alias tokens and mixed UTF encodings exercise pointer ownership separately
 * from JNI handle identity. Uses only synthetic strings, never credentials. */
static bool pressure_many_string_loans(FILE *log, JNIEnv *env)
{
    enum { COUNT = 128 };
    jstring strings[COUNT] = {0};
    const char *utf[COUNT] = {0};
    const jchar *wide[COUNT] = {0};
    const char *again = NULL;
    bool ok = false;
    if ((*env)->PushLocalFrame(env, COUNT + 8) < 0) return false;
    for (unsigned i = 0; i < COUNT; ++i) {
        char text[48]; snprintf(text, sizeof text, "jni-borrow-fixture-%03u", i);
        strings[i] = (*env)->NewStringUTF(env, text);
        if (!strings[i]) goto done;
        utf[i] = (*env)->GetStringUTFChars(env, strings[i], NULL);
        wide[i] = (*env)->GetStringChars(env, strings[i], NULL);
        if (!utf[i] || !wide[i] || (*env)->ExceptionCheck(env)) goto done;
        for (unsigned j = 0; j <= i; ++j) {
            char want[48]; snprintf(want, sizeof want, "jni-borrow-fixture-%03u", j);
            size_t n = strlen(want);
            if (strcmp(utf[j], want)) goto done;
            for (size_t k = 0; k < n; ++k) if (wide[j][k] != (unsigned char)want[k]) goto done;
        }
    }
    again = (*env)->GetStringUTFChars(env, strings[0], NULL);
    if (!again || strcmp(again, utf[0]) || (*env)->ExceptionCheck(env)) goto done;
    (*env)->ReleaseStringUTFChars(env, strings[0], utf[0]); utf[0] = NULL;
    /* Every other buffer remains valid while half of the loans are released. */
    for (int i = COUNT - 1; i >= 0; --i) {
        char want[48]; snprintf(want, sizeof want, "jni-borrow-fixture-%03u", (unsigned)i);
        if ((utf[i] && strcmp(utf[i], want)) || (i == 0 && strcmp(again, want))) goto done;
        jobject alias = (*env)->NewLocalRef(env, strings[i]);
        if (!alias) goto done;
        (*env)->DeleteLocalRef(env, strings[i]); strings[i] = (jstring)alias;
        if (utf[i]) { (*env)->ReleaseStringUTFChars(env, strings[i], utf[i]); utf[i] = NULL; }
        (*env)->ReleaseStringChars(env, strings[i], wide[i]); wide[i] = NULL;
    }
    if (strcmp(again, "jni-borrow-fixture-000")) goto done;
    (*env)->ReleaseStringUTFChars(env, strings[0], again); again = NULL;
    for (unsigned i = 0; i < COUNT * 2; ++i) {
        jstring text = (*env)->NewStringUTF(env, "released-cache-reuse");
        if (!text) goto done;
        const char *bytes = (*env)->GetStringUTFChars(env, text, NULL);
        if (!bytes || strcmp(bytes, "released-cache-reuse")) goto done;
        (*env)->ReleaseStringUTFChars(env, text, bytes);
        (*env)->DeleteLocalRef(env, text);
        if ((*env)->ExceptionCheck(env)) goto done;
    }
    ok = true;
done:
    for (unsigned i = 0; i < COUNT; ++i) {
        if (utf[i]) (*env)->ReleaseStringUTFChars(env, strings[i], utf[i]);
        if (wide[i]) (*env)->ReleaseStringChars(env, strings[i], wide[i]);
    }
    if (again) (*env)->ReleaseStringUTFChars(env, strings[0], again);
    (*env)->PopLocalFrame(env, NULL);
    if (log) dprintf(fileno(log), "CHECK case=simultaneous_string_loans ok=%d distinct=128 utf8=128 utf16=128 all_previous_checked=1 repeated_partial_release=1 alias_release_after_DeleteLocalRef=1 reverse_release=1 reuse=256 actual_JNI=1\n", ok);
    return ok;
}

/* Keep this test's accounting mirror in step with next_arena.c's block header.
 * No allocator internals are modified and the total limit is never raised. */
union pressure_arena_header {
    max_align_t alignment;
    struct {
        size_t charge, requested;
        void *previous, *next;
        bool owned;
    } fields;
};

static void pressure_prepare_trace(FILE *log, JNIEnv *env,
                                   struct jni_bridge *bridge, struct dvm *vm,
                                   const char *where, int present)
{
    if (!log) return;
    jthrowable pending = (*env)->ExceptionOccurred(env);
    dvm_ref ref = 0;
    bool resolved = pending && jni_bridge_resolve_object(bridge, pending, JNI_REF_NONE, &ref);
    struct dvm_class *type = resolved ? dvm_object_class(vm, ref) : NULL;
    struct dvm_class *vm_type = dvm_object_class(vm, dvm_exception(vm));
    dprintf(fileno(log), "PREPARE where=%s present=%d pending_type=%s vm_exception_type=%s\n",
        where, present, type && type->name ? type->name : pending ? "<unresolved>" : "<none>",
        vm_type && vm_type->name ? vm_type->name : "<none>");
    if (pending) (*env)->DeleteLocalRef(env, pending);
}

bool jni_throwable_pressure_selftest(struct dvm *vm)
{
    /* Defense in depth: alternate callers cannot bypass the opt-in gate. */
    if (!vm_pressure_selftest_enabled()) return false;
    const int saved_errno = errno;
    if (!vm || !dvm_gil_held(vm) || dvm_exception(vm)) return false;
    struct jni_bridge *bridge = jni_bridge_for_vm(vm);
    JNIEnv *env = bridge ? jni_bridge_env(bridge) : NULL;
    if (!env || (*env)->ExceptionCheck(env)) return false;
    uint64_t saved_call_steps = vm->call_steps, saved_limit = vm->step_limit;
    bool saved_parked = vm->parked;
    FILE *log = fopen("/data/storage/el2/base/haps/entry/files/sdkdex/jni-throwable-pressure-selftest.txt", "a");
    bool ok = false, frame = false;
    const char *stage = "local_frame";
    void *filler = NULL, *unexpected = NULL;
    jstring borrowed_text = NULL;
    const char *borrowed_utf = NULL;
    const char *borrowed_second = NULL;
    char borrowed_expected[256] = {0};
    size_t limit = vm_arena_limit(), before = vm_arena_used();
    size_t saturated = 0, denied_before = vm_arena_denied(), checks = 0;
    if ((*env)->PushLocalFrame(env, 24) < 0) goto done;
    frame = true;

    stage = "prepare_Object_class";
    jclass object_class = (*env)->FindClass(env, "java/lang/Object");
    pressure_prepare_trace(log, env, bridge, vm, stage, object_class != NULL);
    if (!object_class || (*env)->ExceptionCheck(env)) goto done;
    stage = "prepare_Throwable_class";
    jclass throwable_class = (*env)->FindClass(env, "java/lang/Throwable");
    pressure_prepare_trace(log, env, bridge, vm, stage, throwable_class != NULL);
    if (!throwable_class || (*env)->ExceptionCheck(env)) goto done;
    stage = "prepare_OOM_class";
    jclass oom_class = (*env)->FindClass(env, "java/lang/OutOfMemoryError");
    pressure_prepare_trace(log, env, bridge, vm, stage, oom_class != NULL);
    if (!oom_class || (*env)->ExceptionCheck(env)) goto done;
    stage = "prepare_Object_toString_methodID";
    jmethodID to_string = (*env)->GetMethodID(env, object_class, "toString", "()Ljava/lang/String;");
    pressure_prepare_trace(log, env, bridge, vm, stage, to_string != NULL);
    if (!to_string || (*env)->ExceptionCheck(env)) goto done;
    jobject errors[2] = {NULL, NULL};
    stage = "prepare_Throwable_ThrowNew_no_message";
    jint thrown = (*env)->ThrowNew(env, throwable_class, "");
    pressure_prepare_trace(log, env, bridge, vm, stage, thrown == JNI_OK);
    if (thrown != JNI_OK) goto done;
    errors[0] = (*env)->ExceptionOccurred(env);
    (*env)->ExceptionClear(env);
    if (!errors[0] || dvm_exception(vm) || (*env)->ExceptionCheck(env)) goto done;
    stage = "prepare_OOM_ThrowNew";
    thrown = (*env)->ThrowNew(env, oom_class, "pressure-selftest");
    pressure_prepare_trace(log, env, bridge, vm, stage, thrown == JNI_OK);
    if (thrown != JNI_OK) goto done;
    errors[1] = (*env)->ExceptionOccurred(env);
    (*env)->ExceptionClear(env);
    if (!errors[1] || dvm_exception(vm) || (*env)->ExceptionCheck(env)) goto done;
    ++checks;

    stage = "saturate_normal_arena";
    size_t reserve = limit > 4u * 1024u * 1024u ? 2u * 1024u * 1024u : limit / 8u;
    size_t threshold = limit - reserve, used = vm_arena_used();
    if (!limit || used >= threshold || threshold - used <= sizeof(union pressure_arena_header)) goto done;
    filler = vm_arena_malloc(threshold - used - sizeof(union pressure_arena_header));
    if (!filler || vm_arena_used() != threshold) goto done;
    saturated = vm_arena_used();
    unexpected = vm_arena_malloc(1);
    if (unexpected || vm_arena_denied() <= denied_before) goto done;
    ++checks;

    stage = "real_JNI_toString_and_UTF";
    for (unsigned i = 0; i < 2; ++i) {
        /* Object's jmethodID must virtually select the real Throwable method.
         * No fabricated String or JNI return value enters this assertion. */
        jstring text = (jstring)(*env)->CallObjectMethodA(env, errors[i], to_string, NULL);
        if (!text || (*env)->ExceptionCheck(env) || dvm_exception(vm)) goto done;
        const char *utf = (*env)->GetStringUTFChars(env, text, NULL);
        bool matches = utf && (i == 0 ? strstr(utf, "Throwable") != NULL :
            strstr(utf, "OutOfMemoryError") != NULL && strstr(utf, "pressure-selftest") != NULL);
        if (utf) (*env)->ReleaseStringUTFChars(env, text, utf);
        (*env)->DeleteLocalRef(env, text);
        if (!matches || (*env)->ExceptionCheck(env) || dvm_exception(vm) || vm_arena_used() > limit) goto done;
        if (log) dprintf(fileno(log), "CHECK case=%s actual_JNI=1 nonnull_text=1 virtual_Object_toString=1 normal_arena_saturated=1 used=%zu limit=%zu\n",
            i ? "OutOfMemoryError" : "Throwable_no_message", vm_arena_used(), limit);
        ++checks;
    }

    /* The production cache has 16 entries. Exceed it without consuming any
     * prewarm pointers: stale prepared-only entries must be evictable while
     * the pointer actually borrowed below must remain valid until Release. */
    stage = "borrowed_pointer_prepare";
    borrowed_text = (jstring)(*env)->CallObjectMethodA(env, errors[1], to_string, NULL);
    if (!borrowed_text || (*env)->ExceptionCheck(env) || dvm_exception(vm)) goto done;
    borrowed_utf = (*env)->GetStringUTFChars(env, borrowed_text, NULL);
    if (!borrowed_utf || strlen(borrowed_utf) >= sizeof borrowed_expected ||
        (*env)->ExceptionCheck(env) || dvm_exception(vm)) goto done;
    memcpy(borrowed_expected, borrowed_utf, strlen(borrowed_utf) + 1u);

    stage = "unconsumed_prewarm_cache_churn";
    enum { PRESSURE_PREWARM_CALLS = 40 };
    for (unsigned i = 0; i < PRESSURE_PREWARM_CALLS; ++i) {
        jstring unconsumed = (jstring)(*env)->CallObjectMethodA(env, errors[i % 2], to_string, NULL);
        if (!unconsumed || (*env)->ExceptionCheck(env) || dvm_exception(vm)) goto done;
        (*env)->DeleteLocalRef(env, unconsumed);
    }
    if (log) dprintf(fileno(log), "CHECK case=unconsumed_prewarm_cache_churn actual_JNI=1 calls=%u cache_capacity=16 no_GetUTF_in_loop=1 no_pending=1\n", PRESSURE_PREWARM_CALLS);
    ++checks;

    stage = "post_churn_real_GetUTF";
    jstring final_text = (jstring)(*env)->CallObjectMethodA(env, errors[1], to_string, NULL);
    if (!final_text || (*env)->ExceptionCheck(env) || dvm_exception(vm)) goto done;
    const char *final_utf = (*env)->GetStringUTFChars(env, final_text, NULL);
    bool final_matches = final_utf && strstr(final_utf, "OutOfMemoryError") && strstr(final_utf, "pressure-selftest");
    if (final_utf) (*env)->ReleaseStringUTFChars(env, final_text, final_utf);
    (*env)->DeleteLocalRef(env, final_text);
    if (!final_matches || (*env)->ExceptionCheck(env) || dvm_exception(vm)) goto done;
    if (log) dprintf(fileno(log), "CHECK case=post_churn_real_GetUTF nonnull_text=1 actual_JNI=1 no_pending=1\n");
    ++checks;

    stage = "normal_allocation_restored";
    vm_arena_free(filler); filler = NULL;
    unexpected = vm_arena_malloc(32);
    if (!unexpected || vm_arena_limit() != limit || vm_arena_used() >= saturated ||
        (*env)->ExceptionCheck(env) || dvm_exception(vm)) goto done;
    vm_arena_free(unexpected); unexpected = NULL;
    ++checks;

    /* NewStringUTF is an ordinary allocation, so release only the filler
     * first. The borrowed exception UTF pointer remains live throughout. */
    stage = "ordinary_UTFLength_cache_churn";
    for (unsigned i = 0; i < 40; ++i) {
        jstring ordinary = (*env)->NewStringUTF(env, "cache-length-probe");
        if (!ordinary || (*env)->ExceptionCheck(env) || dvm_exception(vm)) goto done;
        jsize length = (*env)->GetStringUTFLength(env, ordinary);
        (*env)->DeleteLocalRef(env, ordinary);
        if (length != 18 || (*env)->ExceptionCheck(env) || dvm_exception(vm)) goto done;
    }
    if (log) dprintf(fileno(log), "CHECK case=ordinary_UTFLength_cache_churn calls=40 cache_dynamic=1 no_GetChars_in_loop=1 actual_JNI=1\n");
    ++checks;

    stage = "post_UTFLength_churn_Throwable_UTF";
    jstring after_length = (jstring)(*env)->CallObjectMethodA(env, errors[1], to_string, NULL);
    if (!after_length || (*env)->ExceptionCheck(env) || dvm_exception(vm)) goto done;
    const char *after_utf = (*env)->GetStringUTFChars(env, after_length, NULL);
    bool after_matches = after_utf && strstr(after_utf, "OutOfMemoryError") && strstr(after_utf, "pressure-selftest");
    if (after_utf) (*env)->ReleaseStringUTFChars(env, after_length, after_utf);
    (*env)->DeleteLocalRef(env, after_length);
    if (!after_matches || (*env)->ExceptionCheck(env) || dvm_exception(vm)) goto done;
    if (log) dprintf(fileno(log), "CHECK case=post_UTFLength_churn_Throwable_UTF nonnull_text=1 actual_JNI=1\n");
    ++checks;

    stage = "borrowed_pointer_survives_churn";
    if (strcmp(borrowed_utf, borrowed_expected) || (*env)->ExceptionCheck(env) || dvm_exception(vm)) goto done;
    if (log) dprintf(fileno(log), "CHECK case=borrowed_pointer_survives_churn unchanged_until_Release=1 actual_JNI=1\n");
    ++checks;

    stage = "repeated_borrow_partial_release_churn";
    borrowed_second = (*env)->GetStringUTFChars(env, borrowed_text, NULL);
    if (!borrowed_second || strcmp(borrowed_second, borrowed_expected) ||
        (*env)->ExceptionCheck(env) || dvm_exception(vm)) goto done;
    (*env)->ReleaseStringUTFChars(env, borrowed_text, borrowed_utf); borrowed_utf = NULL;
    for (unsigned i = 0; i < 40; ++i) {
        jstring ordinary = (*env)->NewStringUTF(env, "cache-length-probe");
        if (!ordinary || (*env)->ExceptionCheck(env) || dvm_exception(vm)) goto done;
        jsize length = (*env)->GetStringUTFLength(env, ordinary);
        (*env)->DeleteLocalRef(env, ordinary);
        if (length != 18 || (*env)->ExceptionCheck(env) || dvm_exception(vm)) goto done;
    }
    if (strcmp(borrowed_second, borrowed_expected) || (*env)->ExceptionCheck(env) || dvm_exception(vm)) goto done;
    (*env)->ReleaseStringUTFChars(env, borrowed_text, borrowed_second); borrowed_second = NULL;
    (*env)->DeleteLocalRef(env, borrowed_text); borrowed_text = NULL;
    if (log) dprintf(fileno(log), "CHECK case=repeated_borrow_partial_release_churn borrows=2 first_Release_before_churn=1 churn=40 second_pointer_valid_until_final_Release=1 actual_JNI=1\n");
    ++checks;
    stage = "simultaneous_128_UTF8_UTF16_loans";
    if (!pressure_many_string_loans(log, env) || (*env)->ExceptionCheck(env) || dvm_exception(vm)) goto done;
    ++checks;
    ok = true;

done:
    if (borrowed_utf) (*env)->ReleaseStringUTFChars(env, borrowed_text, borrowed_utf);
    if (borrowed_second) (*env)->ReleaseStringUTFChars(env, borrowed_text, borrowed_second);
    if (borrowed_text) (*env)->DeleteLocalRef(env, borrowed_text);
    if (unexpected) vm_arena_free(unexpected);
    if (filler) vm_arena_free(filler);
    if (!ok) pressure_prepare_trace(log, env, bridge, vm, stage, 0);
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    if (dvm_exception(vm)) dvm_clear_exception(vm);
    if (frame) (*env)->PopLocalFrame(env, NULL);
    vm->call_steps = saved_call_steps;
    vm->step_limit = saved_limit;
    vm->parked = saved_parked;
    if (log) {
        dprintf(fileno(log), "RESULT ok=%d checks=%zu stage=%s before=%zu after=%zu saturated=%zu limit=%zu limit_unchanged=%d denied_delta=%zu filler_released=1\n",
            ok, checks, stage, before, vm_arena_used(), saturated, limit,
            vm_arena_limit() == limit, vm_arena_denied() - denied_before);
        fclose(log);
    }
    sdk_trace_fprintf(stderr, "JNI_THROWABLE_PRESSURE_SELFTEST %s checks=%zu stage=%s actual_JNI=1 total_limit_unchanged=%d filler_released=1\n",
        ok ? "PASS" : "FAIL", checks, stage, vm_arena_limit() == limit);
    errno = saved_errno;
    return ok;
}
