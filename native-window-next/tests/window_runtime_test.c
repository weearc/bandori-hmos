#define _POSIX_C_SOURCE 200809L
#include "../window_runtime.h"

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct fixture {
    pthread_mutex_t lock;
    pthread_cond_t changed;
    uint64_t generation;
    int32_t width;
    int32_t height;
    int32_t base_width;
    int32_t base_height;
    int32_t format;
    int32_t base_format;
    int acquire_refs;
    int release_calls;
    int configure_calls;
    int no_current;
    int block_acquire;
    int acquire_entered;
    int block_query;
    int query_entered;
    int block_release;
    int release_entered;
} fixture;

static void wait_if_blocked(fixture *f)
{
    if (f->block_query) {
        f->query_entered = 1;
        pthread_cond_broadcast(&f->changed);
        while (f->block_query) pthread_cond_wait(&f->changed, &f->lock);
    }
}

static int fixture_acquire(void *user, void **native, uint64_t *generation)
{
    fixture *f = user;
    pthread_mutex_lock(&f->lock);
    if (f->block_acquire) {
        f->acquire_entered = 1;
        pthread_cond_broadcast(&f->changed);
        while (f->block_acquire) pthread_cond_wait(&f->changed, &f->lock);
    }
    if (f->no_current) {
        pthread_mutex_unlock(&f->lock);
        return -ENODEV;
    }
    ++f->acquire_refs;
    *native = f;
    *generation = f->generation;
    pthread_mutex_unlock(&f->lock);
    return 0;
}

static int fixture_query(void *user, void *native, uint64_t generation,
                         int32_t *width, int32_t *height)
{
    fixture *f = user;
    pthread_mutex_lock(&f->lock);
    assert(native == f);
    if (generation != f->generation) {
        pthread_mutex_unlock(&f->lock);
        return -ESTALE;
    }
    wait_if_blocked(f);
    *width = f->width;
    *height = f->height;
    pthread_mutex_unlock(&f->lock);
    return 0;
}

static int fixture_configure(void *user, void *native, uint64_t generation,
                             int32_t width, int32_t height, int32_t android_format)
{
    fixture *f = user;
    pthread_mutex_lock(&f->lock);
    assert(native == f);
    if (generation != f->generation) {
        pthread_mutex_unlock(&f->lock);
        return -ESTALE;
    }
    wait_if_blocked(f);
    if (width < 0 || height < 0 || ((width == 0) != (height == 0))) {
        pthread_mutex_unlock(&f->lock);
        return -EINVAL;
    }
    if (android_format != 0 && android_format != 1 && android_format != 2 && android_format != 4) {
        pthread_mutex_unlock(&f->lock);
        return -ENOTSUP;
    }
    if (width == 0 && height == 0) {
        f->width = f->base_width;
        f->height = f->base_height;
    } else {
        f->width = width;
        f->height = height;
    }
    f->format = android_format == 0 ? f->base_format : android_format;
    ++f->configure_calls;
    pthread_mutex_unlock(&f->lock);
    return 0;
}

static int fixture_release(void *user, void *native)
{
    fixture *f = user;
    pthread_mutex_lock(&f->lock);
    assert(native == f);
    ++f->release_calls;
    --f->acquire_refs;
    assert(f->acquire_refs >= 0);
    if (f->block_release) {
        f->release_entered = 1;
        pthread_cond_broadcast(&f->changed);
        while (f->block_release) pthread_cond_wait(&f->changed, &f->lock);
    }
    pthread_mutex_unlock(&f->lock);
    return 0;
}

static fixture g = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .changed = PTHREAD_COND_INITIALIZER,
    .generation = 1,
    .width = 2560,
    .height = 1600,
    .base_width = 2560,
    .base_height = 1600,
    .format = 1,
    .base_format = 1
};

static window_runtime_host host = {
    WINDOW_RUNTIME_HOST_ABI_VERSION,
    sizeof(window_runtime_host),
    &g,
    fixture_acquire,
    fixture_query,
    fixture_release,
    fixture_configure
};

static void wait_flag(int *flag)
{
    pthread_mutex_lock(&g.lock);
    while (!*flag) pthread_cond_wait(&g.changed, &g.lock);
    pthread_mutex_unlock(&g.lock);
}

typedef struct thread_result { void *handle; int32_t value; } thread_result;
static void *acquire_thread(void *arg)
{
    thread_result *r = arg;
    r->handle = window_runtime_acquire_current();
    return NULL;
}
static void *query_thread(void *arg)
{
    thread_result *r = arg;
    r->value = window_runtime_get_width(r->handle);
    return NULL;
}
static void *release_thread(void *arg)
{
    thread_result *r = arg;
    window_runtime_release(r->handle);
    return NULL;
}
static void *configure_thread(void *arg)
{
    thread_result *r = arg;
    r->value = window_runtime_set_buffers_geometry(r->handle, 1600, 900, 2);
    return NULL;
}

static void test_host_validation_and_no_window(void)
{
    window_runtime_host bad = host;
    bad.abi_version++;
    assert(window_runtime_set_host(&bad) == -EINVAL);
    bad = host;
    bad.struct_size = 1;
    assert(window_runtime_set_host(&bad) == -EINVAL);
    bad = host;
    bad.query = NULL;
    assert(window_runtime_set_host(&bad) == -EINVAL);
    assert(window_runtime_acquire_current() == NULL);
    assert(window_runtime_set_host(&host) == 0);
    pthread_mutex_lock(&g.lock);
    g.no_current = 1;
    pthread_mutex_unlock(&g.lock);
    assert(window_runtime_acquire_current() == NULL);
    pthread_mutex_lock(&g.lock);
    g.no_current = 0;
    pthread_mutex_unlock(&g.lock);
}

static void test_refs_stale_and_invalid(void)
{
    void *h = window_runtime_acquire_current();
    assert(h != NULL);
    assert(window_runtime_ref_count(h) == 1);
    assert(window_runtime_get_width(h) == 2560);
    assert(window_runtime_get_height(h) == 1600);
    uint64_t actual_generation=0;
    assert(window_runtime_acquire_alive(h,&actual_generation));
    assert(actual_generation==g.generation&&window_runtime_ref_count(h)==2);
    window_runtime_release(h);
    window_runtime_acquire(h);
    assert(window_runtime_ref_count(h) == 2);
    window_runtime_release(h);
    assert(window_runtime_ref_count(h) == 1);
    int before = g.release_calls;
    window_runtime_release(h);
    assert(g.release_calls == before + 1);
    assert(window_runtime_ref_count(h) == -EINVAL);
    window_runtime_release(h); /* stale release is ignored */
    assert(g.release_calls == before + 1);
    assert(window_runtime_get_width((void *)(uintptr_t)0x1234) == -EINVAL);
    assert(window_runtime_get_height(NULL) == -EINVAL);

    void *next = window_runtime_acquire_current();
    assert(next != NULL && next != h);
    assert(window_runtime_get_width(h) == -EINVAL);
    window_runtime_release(next);

    h = window_runtime_acquire_current();
    assert(h != NULL);
    pthread_mutex_lock(&g.lock);
    ++g.generation;
    pthread_mutex_unlock(&g.lock);
    assert(window_runtime_get_width(h) == -ESTALE);
    assert(!window_runtime_acquire_alive(h,NULL));
    assert(window_runtime_ref_count(h)==1);
    window_runtime_release(h);
}

static void test_capacity_and_reuse(void)
{
    void *handles[64];
    for (int i = 0; i < 64; ++i) {
        handles[i] = window_runtime_acquire_current();
        assert(handles[i] != NULL);
    }
    assert(window_runtime_acquire_current() == NULL);
    assert(window_runtime_set_host(&host) == -EBUSY);
    for (int i = 0; i < 64; ++i) window_runtime_release(handles[i]);
    for (int i = 0; i < 200; ++i) {
        void *h = window_runtime_acquire_current();
        assert(h != NULL);
        window_runtime_release(h);
    }
}

static void test_host_busy_during_acquire(void)
{
    pthread_mutex_lock(&g.lock);
    g.block_acquire = 1;
    g.acquire_entered = 0;
    pthread_mutex_unlock(&g.lock);
    thread_result r = {0};
    pthread_t t;
    assert(pthread_create(&t, NULL, acquire_thread, &r) == 0);
    wait_flag(&g.acquire_entered);
    assert(window_runtime_set_host(NULL) == -EBUSY);
    pthread_mutex_lock(&g.lock);
    g.block_acquire = 0;
    pthread_cond_broadcast(&g.changed);
    pthread_mutex_unlock(&g.lock);
    assert(pthread_join(t, NULL) == 0);
    assert(r.handle != NULL);
    window_runtime_release(r.handle);
}

static void test_last_release_waits_for_query(void)
{
    void *h = window_runtime_acquire_current();
    assert(h != NULL);
    pthread_mutex_lock(&g.lock);
    int release_before = g.release_calls;
    g.block_query = 1;
    g.query_entered = 0;
    pthread_mutex_unlock(&g.lock);

    thread_result q = {.handle = h};
    pthread_t query;
    assert(pthread_create(&query, NULL, query_thread, &q) == 0);
    wait_flag(&g.query_entered);
    window_runtime_release(h); /* final logical ref closes while query is active */
    assert(window_runtime_ref_count(h) == -EINVAL);
    assert(window_runtime_set_host(NULL) == -EBUSY);
    pthread_mutex_lock(&g.lock);
    assert(g.release_calls == release_before);
    g.block_query = 0;
    pthread_cond_broadcast(&g.changed);
    pthread_mutex_unlock(&g.lock);
    assert(pthread_join(query, NULL) == 0);
    assert(q.value == 2560);
    pthread_mutex_lock(&g.lock);
    assert(g.release_calls == release_before + 1);
    pthread_mutex_unlock(&g.lock);
}

static void test_host_busy_during_release_callback(void)
{
    void *h = window_runtime_acquire_current();
    assert(h != NULL);
    pthread_mutex_lock(&g.lock);
    g.block_release = 1;
    g.release_entered = 0;
    pthread_mutex_unlock(&g.lock);
    thread_result r = {.handle = h};
    pthread_t t;
    assert(pthread_create(&t, NULL, release_thread, &r) == 0);
    wait_flag(&g.release_entered);
    assert(window_runtime_set_host(NULL) == -EBUSY);
    pthread_mutex_lock(&g.lock);
    g.block_release = 0;
    pthread_cond_broadcast(&g.changed);
    pthread_mutex_unlock(&g.lock);
    assert(pthread_join(t, NULL) == 0);
    assert(window_runtime_set_host(NULL) == 0);
    assert(window_runtime_set_host(&host) == 0);
}

static void test_geometry_contract(void)
{
    void *h = window_runtime_acquire_current();
    assert(h != NULL);
    assert(window_runtime_set_buffers_geometry(h, 1600, 900, 2) == 0);
    assert(window_runtime_get_width(h) == 1600);
    assert(window_runtime_get_height(h) == 900);
    assert(window_runtime_set_buffers_geometry(h, 0, 0, 0) == 0);
    assert(window_runtime_get_width(h) == g.base_width);
    assert(window_runtime_get_height(h) == g.base_height);

    pthread_mutex_lock(&g.lock);
    int32_t old_width = g.width, old_height = g.height, old_format = g.format;
    int old_calls = g.configure_calls;
    pthread_mutex_unlock(&g.lock);
    assert(window_runtime_set_buffers_geometry(h, 0, 800, 1) == -EINVAL);
    assert(window_runtime_set_buffers_geometry(h, -1, -1, 1) == -EINVAL);
    assert(window_runtime_set_buffers_geometry(h, 800, 600, -1) == -EINVAL);
    assert(window_runtime_set_buffers_geometry(h, 1200, 700, 3) == -ENOTSUP);
    pthread_mutex_lock(&g.lock);
    assert(g.width == old_width && g.height == old_height && g.format == old_format);
    assert(g.configure_calls == old_calls);
    pthread_mutex_unlock(&g.lock);

    pthread_mutex_lock(&g.lock);
    ++g.generation;
    pthread_mutex_unlock(&g.lock);
    assert(window_runtime_set_buffers_geometry(h, 1200, 700, 1) == -ESTALE);
    assert(window_runtime_get_width(h) == -ESTALE);
    window_runtime_release(h);
}

static void test_geometry_without_host_callback(void)
{
    window_runtime_host no_configure = host;
    no_configure.configure = NULL;
    assert(window_runtime_set_host(&no_configure) == 0);
    void *h = window_runtime_acquire_current();
    assert(h != NULL);
    assert(window_runtime_set_buffers_geometry(h, 1200, 700, 1) == -ENOTSUP);
    window_runtime_release(h);
    assert(window_runtime_set_host(&host) == 0);
}

static void test_geometry_pin_waits_for_last_release(void)
{
    void *h = window_runtime_acquire_current();
    assert(h != NULL);
    pthread_mutex_lock(&g.lock);
    int release_before = g.release_calls;
    g.block_query = 1;
    g.query_entered = 0;
    pthread_mutex_unlock(&g.lock);

    thread_result op = {.handle = h};
    pthread_t configure;
    assert(pthread_create(&configure, NULL, configure_thread, &op) == 0);
    wait_flag(&g.query_entered);
    window_runtime_release(h);
    assert(window_runtime_set_host(NULL) == -EBUSY);
    pthread_mutex_lock(&g.lock);
    assert(g.release_calls == release_before);
    g.block_query = 0;
    pthread_cond_broadcast(&g.changed);
    pthread_mutex_unlock(&g.lock);
    assert(pthread_join(configure, NULL) == 0);
    assert(op.value == 0);
    pthread_mutex_lock(&g.lock);
    assert(g.release_calls == release_before + 1);
    pthread_mutex_unlock(&g.lock);
}

int main(void)
{
    test_host_validation_and_no_window();
    test_refs_stale_and_invalid();
    test_capacity_and_reuse();
    test_host_busy_during_acquire();
    test_last_release_waits_for_query();
    test_host_busy_during_release_callback();
    test_geometry_without_host_callback();
    test_geometry_contract();
    test_geometry_pin_waits_for_last_release();
    assert(window_runtime_set_host(NULL) == 0);
    assert(g.acquire_refs == 0);
    puts("window_runtime_test: PASS");
    return 0;
}
