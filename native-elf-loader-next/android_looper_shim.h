#ifndef ANDROID_LOOPER_SHIM_H
#define ANDROID_LOOPER_SHIM_H

/*
 * Minimal Android NDK looper shim (android/looper.h).
 *
 * The supported subset has no registered fd/callback sources; pollOnce waits
 * on a real eventfd and returns WAKE when wake() signals it. Negative timeout
 * values wait indefinitely; nonnegative values are measured against a single
 * monotonic deadline across EINTR.
 *
 * Looper storage is a bounded static table with TLS destructor cleanup. Opaque
 * handles are unique integer tokens (never dereferenced), preventing stale
 * handles from matching a later slot occupant. Handles remain valid while a
 * thread ref or acquired external ref exists.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ALOOPER_POLL_WAKE (-1)
#define ALOOPER_POLL_CALLBACK (-2)
#define ALOOPER_POLL_TIMEOUT (-3)
#define ALOOPER_POLL_ERROR (-4)

void *android_looper_for_thread(void);
void *android_looper_prepare(int opts);
void android_looper_acquire(void *looper);
void android_looper_release(void *looper);
void android_looper_wake(void *looper);
int android_looper_poll_once(int timeout_millis, int *out_fd, int *out_events,
                             void **out_data);
int android_looper_poll_all(int timeout_millis, int *out_fd, int *out_events,
                            void **out_data);

#ifdef __cplusplus
}
#endif

#endif
