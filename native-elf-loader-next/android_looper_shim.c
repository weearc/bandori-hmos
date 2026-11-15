#define _GNU_SOURCE 1
#include "android_looper_shim.h"

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <sys/eventfd.h>
#include <unistd.h>

extern int sdk_trace_fprintf(FILE *, const char *, ...);

#define LOOPER_MAX 32

struct looper_slot {
    bool used;
    bool thread_ref;
    int eventfd;
    unsigned external_refs;
    uintptr_t token;
};

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct looper_slot g_slots[LOOPER_MAX];
static pthread_key_t g_thread_key;
static pthread_once_t g_key_once = PTHREAD_ONCE_INIT;
static int g_key_error;
static uintptr_t g_next_token = 1;
static _Thread_local void *g_thread_looper;

/* Tokens are small opaque integer handles. Never dereference caller input. */
static void *make_token(uintptr_t token)
{
    return (void *)token;
}

static uintptr_t token_value(const void *handle)
{
    return (uintptr_t)handle;
}

static struct looper_slot *find_locked(const void *handle)
{
    uintptr_t token = token_value(handle);
    if (!token) return NULL;
    for (int i = 0; i < LOOPER_MAX; ++i)
        if (g_slots[i].used && g_slots[i].token == token) return &g_slots[i];
    return NULL;
}

static void maybe_free_locked(struct looper_slot *slot)
{
    if (!slot->thread_ref && slot->external_refs == 0) {
        close(slot->eventfd);
        slot->used = false;
        slot->eventfd = -1;
        slot->token = 0;
    }
}

static void thread_destructor(void *value)
{
    struct looper_slot *slot;
    if (!value) return;
    pthread_mutex_lock(&g_lock);
    slot = find_locked(value);
    if (slot) {
        slot->thread_ref = false;
        maybe_free_locked(slot);
    }
    pthread_mutex_unlock(&g_lock);
    g_thread_looper = NULL;
}

static void make_key(void)
{
    g_key_error = pthread_key_create(&g_thread_key, thread_destructor);
}

void *android_looper_for_thread(void)
{
    return g_thread_looper;
}

void *android_looper_prepare(int opts)
{
    struct looper_slot *slot = NULL;
    void *handle;
    (void)opts;
    if (g_thread_looper) return g_thread_looper;
    if (pthread_once(&g_key_once, make_key) != 0 || g_key_error != 0) return NULL;
    pthread_mutex_lock(&g_lock);
    if (g_next_token == UINTPTR_MAX) {
        pthread_mutex_unlock(&g_lock);
        return NULL;
    }
    for (int i = 0; i < LOOPER_MAX; ++i) {
        if (!g_slots[i].used) {
            int fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
            if (fd < 0) break;
            slot = &g_slots[i];
            slot->used = true;
            slot->thread_ref = true;
            slot->external_refs = 0;
            slot->eventfd = fd;
            slot->token = g_next_token++;
            break;
        }
    }
    handle = slot ? make_token(slot->token) : NULL;
    pthread_mutex_unlock(&g_lock);
    if (!slot) {
        sdk_trace_fprintf(stderr, "ANDROID_LOOPER prepare=failed reason=table_full_or_eventfd\n");
        return NULL;
    }
    g_thread_looper = handle;
    if(pthread_setspecific(g_thread_key,handle)!=0) {
        thread_destructor(handle);return NULL;
    }
    sdk_trace_fprintf(stderr, "ANDROID_LOOPER prepare=ok looper=%p real_eventfd=1\n", handle);
    return handle;
}

void android_looper_acquire(void *looper)
{
    struct looper_slot *slot;
    pthread_mutex_lock(&g_lock);
    slot = find_locked(looper);
    if (slot && slot->external_refs < UINT_MAX) ++slot->external_refs;
    pthread_mutex_unlock(&g_lock);
}

void android_looper_release(void *looper)
{
    struct looper_slot *slot;
    pthread_mutex_lock(&g_lock);
    slot = find_locked(looper);
    if (slot && slot->external_refs > 0) {
        --slot->external_refs;
        maybe_free_locked(slot);
    }
    pthread_mutex_unlock(&g_lock);
}

void android_looper_wake(void *looper)
{
    struct looper_slot *slot;
    uint64_t one = 1;
    pthread_mutex_lock(&g_lock);
    slot = find_locked(looper);
    if (slot) {
        ssize_t written;
        do { written=write(slot->eventfd,&one,sizeof one); } while(written<0&&errno==EINTR);
        (void)written; /* EAGAIN means a wake is already pending. */
    }
    pthread_mutex_unlock(&g_lock);
}

static int remaining_ms(const struct timespec *deadline)
{
    struct timespec now;
    int64_t ns;
    clock_gettime(CLOCK_MONOTONIC, &now);
    ns = (int64_t)(deadline->tv_sec - now.tv_sec) * 1000000000LL +
         deadline->tv_nsec - now.tv_nsec;
    if (ns <= 0) return 0;
    ns = (ns + 999999LL) / 1000000LL;
    return ns > INT_MAX ? INT_MAX : (int)ns;
}

int android_looper_poll_once(int timeout_millis, int *out_fd, int *out_events,
                             void **out_data)
{
    struct looper_slot *slot;
    struct pollfd pfd;
    struct timespec deadline;
    int rc, timeout = timeout_millis;
    if (out_fd) *out_fd = -1;
    if (out_events) *out_events = 0;
    if (out_data) *out_data = NULL;
    pthread_mutex_lock(&g_lock);
    slot = find_locked(g_thread_looper);
    if (!slot) {
        pthread_mutex_unlock(&g_lock);
        return ALOOPER_POLL_ERROR;
    }
    pfd.fd = slot->eventfd;
    pthread_mutex_unlock(&g_lock);
    if (timeout_millis >= 0) {
        clock_gettime(CLOCK_MONOTONIC, &deadline);
        deadline.tv_sec += timeout_millis / 1000;
        deadline.tv_nsec += (long)(timeout_millis % 1000) * 1000000L;
        if (deadline.tv_nsec >= 1000000000L) {
            ++deadline.tv_sec;
            deadline.tv_nsec -= 1000000000L;
        }
    }
    pfd.events = POLLIN;
    for (;;) {
        pfd.revents = 0;
        rc = poll(&pfd, 1, timeout);
        if (rc < 0 && errno == EINTR) {
            if (timeout_millis < 0) continue;
            timeout = remaining_ms(&deadline);
            if (timeout == 0) return ALOOPER_POLL_TIMEOUT;
            continue;
        }
        break;
    }
    if (rc < 0) return ALOOPER_POLL_ERROR;
    if (rc == 0) return ALOOPER_POLL_TIMEOUT;
    if (pfd.revents & POLLIN) {
        uint64_t counter;
        ssize_t got = read(pfd.fd, &counter, sizeof counter);
        if (got == (ssize_t)sizeof counter) return ALOOPER_POLL_WAKE;
        if (got < 0 && errno == EAGAIN) return ALOOPER_POLL_TIMEOUT;
        return ALOOPER_POLL_ERROR;
    }
    return ALOOPER_POLL_ERROR;
}

int android_looper_poll_all(int timeout_millis, int *out_fd, int *out_events,
                            void **out_data)
{
    for (;;) {
        int rc = android_looper_poll_once(timeout_millis, out_fd, out_events,
                                          out_data);
        if (rc != ALOOPER_POLL_CALLBACK) return rc;
    }
}
