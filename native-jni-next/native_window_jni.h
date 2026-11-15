#ifndef NATIVE_WINDOW_JNI_H
#define NATIVE_WINDOW_JNI_H

/*
 * libandroid ANativeWindow_fromSurface/toSurface entry points.
 *
 * These are the JNI-facing half of the guest Surface bridge:
 *   - the JNIEnv* must be this thread's attached bridge env (checked by
 *     address against TLS, never dereferenced when foreign),
 *   - the jobject must resolve through the bridge handle table to a dvm_ref,
 *   - the object must carry the surface_runtime Surface payload and have an
 *     alive binding with a still-live window_runtime token.
 * Any failure returns NULL / null token; there is no fallback to "the current
 * window".
 */

#include "jni_abi.h"
#include "dvm/dvm_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

void *android_native_window_from_surface(JNIEnv *env, jobject surface);
jobject android_native_window_to_surface(JNIEnv *env, void *window);

/* Positive + negative coverage, runs inside the VM self-test context. */
bool surface_jni_selftest(struct dvm *vm);

#ifdef __cplusplus
}
#endif

#endif
