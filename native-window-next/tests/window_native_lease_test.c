#define _POSIX_C_SOURCE 200809L
#include "../window_runtime.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>

typedef struct mock_host { int refs, releases; uint64_t generation; int stale; } mock_host;
static int acquire(void *u, void **n, uint64_t *g) { mock_host *m=u; ++m->refs; *n=m; *g=m->generation; return 0; }
static int query(void *u, void *n, uint64_t g, int32_t *w, int32_t *h) { mock_host *m=u; if (m->stale || n!=m || g!=m->generation) return -ESTALE; *w=640; *h=480; return 0; }
static int release_native(void *u, void *n) { mock_host *m=u; assert(n==m); --m->refs; ++m->releases; return 0; }

int main(void)
{
    mock_host m={.generation=7};
    window_runtime_host h={.abi_version=WINDOW_RUNTIME_HOST_ABI_VERSION,.struct_size=sizeof(h),.user=&m,.acquire_current=acquire,.query=query,.release=release_native};
    assert(window_runtime_set_host(&h)==0);
    void *token=window_runtime_acquire_current(); assert(token && m.refs==1);
    void *native=(void *)(uintptr_t)1; uint64_t gen=999;
    assert(window_runtime_acquire_native_lease(token,&native,&gen));
    assert(native==&m && gen==7 && window_runtime_ref_count(token)==2);
    window_runtime_release(token);
    assert(m.refs==1 && m.releases==0);
    assert(window_runtime_set_host(NULL)==-EBUSY);
    window_runtime_release(token);
    assert(m.refs==0 && m.releases==1);
    assert(window_runtime_set_host(NULL)==0);

    m.generation=8; m.stale=1;
    assert(window_runtime_set_host(&h)==0);
    token=window_runtime_acquire_current(); assert(token && m.refs==1);
    native=(void *)(uintptr_t)1; gen=999;
    assert(!window_runtime_acquire_native_lease(token,&native,&gen));
    assert(native==NULL && gen==0 && window_runtime_ref_count(token)==1);
    window_runtime_release(token);
    assert(m.refs==0 && m.releases==2);
    assert(window_runtime_set_host(NULL)==0);
    puts("window_native_lease_test: PASS");
    return 0;
}
