#ifndef ANDROID_SEMAPHORE_SHIM_H
#define ANDROID_SEMAPHORE_SHIM_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Guest sem_t is an opaque 16-byte identity. A mapped native object lives in
 * the process-local registry. Process-shared semaphores are not supported. */
int android_sem_init(void *guest, int pshared, unsigned value);
int android_sem_destroy(void *guest);
int android_sem_post(void *guest);
int android_sem_wait(void *guest);
int android_sem_trywait(void *guest);
/* timeout points to Android LP64 { int64_t tv_sec, int64_t tv_nsec }. */
int android_sem_timedwait(void *guest, const void *timeout);
int android_sem_getvalue(void *guest, int *value);
#ifdef __cplusplus
}
#endif
#endif
