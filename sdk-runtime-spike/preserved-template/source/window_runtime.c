#define _POSIX_C_SOURCE 200809L
#include "window_runtime.h"

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define WINDOW_SLOT_COUNT 64u
#define TOKEN_TAG UINT64_C(0xA7)
#define TOKEN_TAG_SHIFT 56u
#define TOKEN_SLOT_SHIFT 48u
#define TOKEN_SLOT_MASK UINT64_C(0xFF)
#define TOKEN_GENERATION_MASK UINT64_C(0x0000FFFFFFFFFFFF)

typedef enum slot_state {
    SLOT_FREE = 0,
    SLOT_ACQUIRING,
    SLOT_LIVE,
    SLOT_CLOSING,
    SLOT_RELEASING,
    SLOT_RETIRED
} slot_state;

typedef struct window_slot {
    slot_state state;
    uint64_t token_generation;
    uint32_t refs;
    uint32_t active_queries;
    void *native;
    uint64_t host_generation;
} window_slot;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static window_slot g_slots[WINDOW_SLOT_COUNT];
static window_runtime_host g_host;
static bool g_host_installed;

static int normalize_host_error(int rc)
{
    if (rc < 0) return rc;
    return rc == 0 ? 0 : -EIO;
}

static uint64_t make_token(unsigned slot_index, uint64_t generation)
{
    return (TOKEN_TAG << TOKEN_TAG_SHIFT) |
           ((uint64_t)(slot_index + 1u) << TOKEN_SLOT_SHIFT) |
           (generation & TOKEN_GENERATION_MASK);
}

static bool decode_token(void *handle, unsigned *slot_index, uint64_t *generation)
{
#if UINTPTR_MAX < UINT64_MAX
    (void)handle;
    (void)slot_index;
    (void)generation;
    return false;
#else
    uint64_t token = (uint64_t)(uintptr_t)handle;
    uint64_t encoded_slot = (token >> TOKEN_SLOT_SHIFT) & TOKEN_SLOT_MASK;
    if (handle == NULL || (token >> TOKEN_TAG_SHIFT) != TOKEN_TAG ||
        encoded_slot == 0 || encoded_slot > WINDOW_SLOT_COUNT) {
        return false;
    }
    *slot_index = (unsigned)(encoded_slot - 1u);
    *generation = token & TOKEN_GENERATION_MASK;
    return *generation != 0;
#endif
}

/* Caller holds g_lock. This validates the token without dereferencing handle. */
static window_slot *find_live_locked(void *handle, unsigned *index_out)
{
    unsigned index;
    uint64_t generation;
    if (!decode_token(handle, &index, &generation)) return NULL;
    window_slot *slot = &g_slots[index];
    if (slot->state != SLOT_LIVE || slot->token_generation != generation ||
        slot->refs == 0) return NULL;
    if (index_out) *index_out = index;
    return slot;
}

int window_runtime_set_host(const window_runtime_host *host)
{
    if (host && (host->abi_version != WINDOW_RUNTIME_HOST_ABI_VERSION ||
                 host->struct_size < sizeof(window_runtime_host) ||
                 !host->acquire_current || !host->query || !host->release)) {
        return -EINVAL;
    }

    pthread_mutex_lock(&g_lock);
    for (unsigned i = 0; i < WINDOW_SLOT_COUNT; ++i) {
        if (g_slots[i].state != SLOT_FREE && g_slots[i].state != SLOT_RETIRED) {
            pthread_mutex_unlock(&g_lock);
            return -EBUSY;
        }
    }
    if (host) {
        memcpy(&g_host, host, sizeof(g_host));
        g_host_installed = true;
    } else {
        memset(&g_host, 0, sizeof(g_host));
        g_host_installed = false;
    }
    pthread_mutex_unlock(&g_lock);
    return 0;
}

void *window_runtime_acquire_current(void)
{
#if UINTPTR_MAX < UINT64_MAX
    return NULL; /* Tokens encode a 64-bit generation and require 64-bit pointers. */
#else
    window_runtime_host host;
    unsigned index = WINDOW_SLOT_COUNT;
    uint64_t next_generation = 0;

    pthread_mutex_lock(&g_lock);
    if (!g_host_installed) {
        pthread_mutex_unlock(&g_lock);
        return NULL;
    }
    for (unsigned i = 0; i < WINDOW_SLOT_COUNT; ++i) {
        window_slot *slot = &g_slots[i];
        if (slot->state != SLOT_FREE) continue;
        if (slot->token_generation >= TOKEN_GENERATION_MASK) {
            slot->state = SLOT_RETIRED;
            continue;
        }
        index = i;
        next_generation = slot->token_generation + 1u;
        slot->token_generation = next_generation;
        slot->state = SLOT_ACQUIRING;
        break;
    }
    if (index == WINDOW_SLOT_COUNT) {
        pthread_mutex_unlock(&g_lock);
        return NULL;
    }
    host = g_host;
    pthread_mutex_unlock(&g_lock);

    void *native = NULL;
    uint64_t host_generation = 0;
    int rc = normalize_host_error(host.acquire_current(host.user, &native, &host_generation));
    if (rc == 0 && !native) rc = -EINVAL;

    pthread_mutex_lock(&g_lock);
    window_slot *slot = &g_slots[index];
    if (rc == 0) {
        slot->native = native;
        slot->host_generation = host_generation;
        slot->refs = 1;
        slot->active_queries = 0;
        slot->state = SLOT_LIVE;
        uint64_t token = make_token(index, next_generation);
        pthread_mutex_unlock(&g_lock);
        return (void *)(uintptr_t)token;
    }
    slot->native = NULL;
    slot->refs = 0;
    slot->active_queries = 0;
    slot->state = SLOT_FREE;
    pthread_mutex_unlock(&g_lock);
    return NULL;
#endif
}

static int32_t window_operation(void *handle, bool want_width, bool configure,
                                int32_t requested_width, int32_t requested_height,
                                int32_t requested_format)
{
    unsigned index;
    window_runtime_host host;
    void *native;
    uint64_t host_generation;

    pthread_mutex_lock(&g_lock);
    window_slot *slot = find_live_locked(handle, &index);
    if (!slot || !g_host_installed) {
        pthread_mutex_unlock(&g_lock);
        return -EINVAL;
    }
    ++slot->active_queries;
    host = g_host;
    native = slot->native;
    host_generation = slot->host_generation;
    pthread_mutex_unlock(&g_lock);

    int32_t width = 0, height = 0;
    int rc;
    if (configure) {
        rc = host.configure ? normalize_host_error(host.configure(host.user,native,
            host_generation,requested_width,requested_height,requested_format)) : -ENOTSUP;
    } else {
        rc = normalize_host_error(host.query(host.user, native, host_generation,&width,&height));
        if (rc == 0 && (width <= 0 || height <= 0)) rc = -EINVAL;
    }

    void *release_native = NULL;
    window_runtime_host release_host;
    bool do_release = false;
    pthread_mutex_lock(&g_lock);
    slot = &g_slots[index];
    if (slot->token_generation != ((uint64_t)((uintptr_t)handle) & TOKEN_GENERATION_MASK) ||
        slot->active_queries == 0) {
        /* Internal invariant failure: avoid touching an unrelated slot. */
        pthread_mutex_unlock(&g_lock);
        return -EINVAL;
    }
    --slot->active_queries;
    if (slot->state == SLOT_CLOSING && slot->active_queries == 0) {
        release_native = slot->native;
        release_host = g_host;
        slot->state = SLOT_RELEASING;
        do_release = true;
    }
    pthread_mutex_unlock(&g_lock);

    if (do_release) {
        (void)release_host.release(release_host.user, release_native);
        pthread_mutex_lock(&g_lock);
        slot = &g_slots[index];
        slot->native = NULL;
        slot->host_generation = 0;
        slot->refs = 0;
        slot->state = slot->token_generation >= TOKEN_GENERATION_MASK
                          ? SLOT_RETIRED : SLOT_FREE;
        pthread_mutex_unlock(&g_lock);
    }
    if (rc != 0) return rc;
    if (configure) return 0;
    return want_width ? width : height;
}

int32_t window_runtime_get_width(void *handle)
{
    return window_operation(handle, true, false, 0, 0, 0);
}

int32_t window_runtime_get_height(void *handle)
{
    return window_operation(handle, false, false, 0, 0, 0);
}

int32_t window_runtime_set_buffers_geometry(void *handle,int32_t width,int32_t height,int32_t format)
{
    if(width<0||height<0||format<0||((width==0)!=(height==0)))return -EINVAL;
    return window_operation(handle,false,true,width,height,format);
}

void window_runtime_acquire(void *handle)
{
    pthread_mutex_lock(&g_lock);
    window_slot *slot = find_live_locked(handle, NULL);
    if (slot && slot->refs < UINT32_MAX) ++slot->refs;
    pthread_mutex_unlock(&g_lock);
}

bool window_runtime_acquire_alive(void *handle,uint64_t *host_generation)
{
    if(host_generation)*host_generation=0;
    pthread_mutex_lock(&g_lock);
    window_slot *slot=find_live_locked(handle,NULL);
    if(!slot||!g_host_installed||slot->refs==UINT32_MAX) {
        pthread_mutex_unlock(&g_lock);return false;
    }
    ++slot->refs;
    uint64_t generation=slot->host_generation;
    pthread_mutex_unlock(&g_lock);
    /* Our reference keeps the slot and host reference alive during query. */
    if(window_runtime_get_width(handle)<=0) {
        window_runtime_release(handle);return false;
    }
    if(host_generation)*host_generation=generation;
    return true;
}

bool window_runtime_acquire_native_lease(void *token, void **native,
                                         uint64_t *generation)
{
    if (native) *native = NULL;
    if (generation) *generation = 0;
    if (!native || !generation) return false;

    uint64_t checked_generation = 0;
    if (!window_runtime_acquire_alive(token, &checked_generation)) return false;

    pthread_mutex_lock(&g_lock);
    window_slot *slot = find_live_locked(token, NULL);
    if (!slot || slot->host_generation != checked_generation || !slot->native) {
        pthread_mutex_unlock(&g_lock);
        window_runtime_release(token);
        return false;
    }
    *native = slot->native;
    *generation = slot->host_generation;
    pthread_mutex_unlock(&g_lock);
    return true;
}

void window_runtime_release(void *handle)
{
    unsigned index;
    window_runtime_host host;
    void *native = NULL;
    bool do_release = false;

    pthread_mutex_lock(&g_lock);
    window_slot *slot = find_live_locked(handle, &index);
    if (!slot) {
        pthread_mutex_unlock(&g_lock);
        return;
    }
    --slot->refs;
    if (slot->refs == 0) {
        if (slot->active_queries != 0) {
            slot->state = SLOT_CLOSING;
        } else {
            native = slot->native;
            host = g_host;
            slot->state = SLOT_RELEASING;
            do_release = true;
        }
    }
    pthread_mutex_unlock(&g_lock);

    if (!do_release) return;
    (void)host.release(host.user, native);
    pthread_mutex_lock(&g_lock);
    slot = &g_slots[index];
    slot->native = NULL;
    slot->host_generation = 0;
    slot->refs = 0;
    slot->active_queries = 0;
    slot->state = slot->token_generation >= TOKEN_GENERATION_MASK
                      ? SLOT_RETIRED : SLOT_FREE;
    pthread_mutex_unlock(&g_lock);
}

int32_t window_runtime_ref_count(void *handle)
{
    pthread_mutex_lock(&g_lock);
    window_slot *slot = find_live_locked(handle, NULL);
    int32_t refs = slot ? (slot->refs > INT32_MAX ? INT32_MAX : (int32_t)slot->refs) : -EINVAL;
    pthread_mutex_unlock(&g_lock);
    return refs;
}
