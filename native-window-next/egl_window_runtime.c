#include "egl_window_runtime.h"
#include "window_runtime.h"
#include "perf_attr.h"
#include <EGL/egl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <errno.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <time.h>
#include <stdbool.h>

/* Compatibility entry points retained for existing controls. Submit-side
 * pacing is removed: configuration cannot create a clock, acquire a pacing
 * mutex, sleep, or defer eglSwapBuffers. Keep native driver swap semantics. */
void egl_jitter_config(int divisor,int background){
 (void)divisor;(void)background;
}
void egl_jitter_selftest(void){
 FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/jitter-selftest.txt","w");
 if(f){fprintf(f,"pid=%d submit_wait_removed=1 effective_divisor=0 config_is_noop=1 moving=0\n",getpid());fclose(f);}
}
extern int sdk_trace_fprintf(FILE *, const char *, ...);

typedef struct entry { EGLDisplay display; EGLSurface surface; void *token; int busy,render_buffer; struct entry *next; } entry;
static atomic_int observed_swap_interval=-999; /* Unknown until a successful intercepted request. */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static entry *surfaces;
static _Atomic unsigned long long swap_count;
static _Atomic unsigned long long successful_submissions;
static _Atomic unsigned long long last_submission_ns;
static void result_black_egl(const char *event,uintptr_t first,uintptr_t second,int ok){
 FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/result-black-egl.txt","a");
 if(f){fprintf(f,"pid=%d mono_ns=%llu tid=%ld event=%s a=0x%llx b=0x%llx ok=%d\n",getpid(),(unsigned long long)perf_now(),(long)syscall(SYS_gettid),event,(unsigned long long)first,(unsigned long long)second,ok);fclose(f);}
}
unsigned long long egl_window_runtime_last_submission_ns(void){return atomic_load_explicit(&last_submission_ns,memory_order_relaxed);}
unsigned long long egl_window_runtime_successful_submissions(void){return atomic_load_explicit(&successful_submissions,memory_order_relaxed);}
int egl_window_runtime_touch_dimensions(int *width,int *height){
 if(!width||!height)return 0;*width=0;*height=0;
 pthread_mutex_lock(&lock);entry *e=surfaces;EGLint w=0,h=0;
 int ok=e&&eglQuerySurface(e->display,e->surface,EGL_WIDTH,&w)&&eglQuerySurface(e->display,e->surface,EGL_HEIGHT,&h)&&w>0&&h>0;
 if(ok){*width=w;*height=h;}pthread_mutex_unlock(&lock);return ok;
}
static _Thread_local EGLint bridge_error = EGL_SUCCESS;
static void egl_probe_log(const char *format, ...) {
    int saved_errno = errno;
    char message[768];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof message, format, args);
    va_end(args);
    if(!perf_trace_keep(message)){errno=saved_errno;return;}
    sdk_trace_fprintf(stderr, "%s\n", message);
    uint64_t trace_start=perf_now();
    FILE *file = fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt", "a");
    if (file) {
        fprintf(file, "%s\n", message);
        fclose(file);
    }
    errno = saved_errno;
    perf_trace_done(trace_start);
}
static EGLDisplay EGLAPIENTRY bridge_display(EGLNativeDisplayType native) {
    EGLDisplay result = eglGetDisplay(native);
    egl_probe_log("EGL_TRACE get_display native=%p display=%p thread=%llu",
                  (void *)native, (void *)result, (unsigned long long)(uintptr_t)pthread_self());
    return result;
}
static EGLBoolean EGLAPIENTRY bridge_initialize(EGLDisplay display, EGLint *major, EGLint *minor) {
    EGLBoolean result = eglInitialize(display, major, minor);
    egl_probe_log("EGL_TRACE initialize display=%p success=%d major=%d minor=%d thread=%llu",
                  (void *)display, result == EGL_TRUE, result && major ? *major : -1,
                  result && minor ? *minor : -1, (unsigned long long)(uintptr_t)pthread_self());
    return result;
}
static EGLBoolean EGLAPIENTRY bridge_bind_api(EGLenum api) {
    EGLBoolean result = eglBindAPI(api);
    egl_probe_log("EGL_TRACE bind_api api=0x%x success=%d thread=%llu",
                  api, result == EGL_TRUE, (unsigned long long)(uintptr_t)pthread_self());
    return result;
}
static EGLBoolean EGLAPIENTRY bridge_choose_config(EGLDisplay display, const EGLint *attrs,
                                                   EGLConfig *configs, EGLint size, EGLint *count) {
    EGLBoolean result = eglChooseConfig(display, attrs, configs, size, count);
    egl_probe_log("EGL_TRACE choose_config display=%p size=%d count=%d success=%d thread=%llu",
                  (void *)display, size, result && count ? *count : -1, result == EGL_TRUE,
                  (unsigned long long)(uintptr_t)pthread_self());
    return result;
}
static EGLContext EGLAPIENTRY bridge_context(EGLDisplay display, EGLConfig config,
                                             EGLContext share, const EGLint *attrs) {
    EGLContext result = eglCreateContext(display, config, share, attrs);
    result_black_egl("create_context",(uintptr_t)result,(uintptr_t)share,result!=EGL_NO_CONTEXT);
    char attributes[320] = {0};
    size_t used = 0;
    if (attrs) {
        for (unsigned i = 0; i < 32 && attrs[i] != EGL_NONE; i += 2) {
            int count = snprintf(attributes + used, sizeof attributes - used, "0x%x=%d,",
                                 attrs[i], attrs[i + 1]);
            if (count < 0 || (size_t)count >= sizeof attributes - used) break;
            used += (size_t)count;
        }
    }
    egl_probe_log("EGL_TRACE create_context display=%p config=%p share=%p context=%p success=%d thread=%llu",
                  (void *)display, (void *)config, (void *)share, (void *)result,
                  result != EGL_NO_CONTEXT, (unsigned long long)(uintptr_t)pthread_self());
    egl_probe_log("EGL_TRACE context_attributes context=%p attrs=%s", (void *)result, attributes);
    return result;
}
static EGLBoolean EGLAPIENTRY bridge_make_current(EGLDisplay display, EGLSurface draw,
                                                  EGLSurface read, EGLContext context) {
    EGLBoolean result = eglMakeCurrent(display, draw, read, context);
    result_black_egl("make_current",(uintptr_t)context,(uintptr_t)draw,result==EGL_TRUE);
    egl_probe_log("EGL_TRACE make_current display=%p draw=%p read=%p context=%p current=%p success=%d thread=%llu",
                  (void *)display, (void *)draw, (void *)read, (void *)context,
                  (void *)eglGetCurrentContext(), result == EGL_TRUE,
                  (unsigned long long)(uintptr_t)pthread_self());
    if (result && context != EGL_NO_CONTEXT) {
        typedef const unsigned char *(*get_string_fn)(unsigned);
        __eglMustCastToProperFunctionPointerType symbol = eglGetProcAddress("glGetString");
        get_string_fn get_string = NULL;
        _Static_assert(sizeof(get_string) == sizeof(symbol), "GL function pointer size");
        memcpy(&get_string, &symbol, sizeof get_string);
        const unsigned char *renderer = get_string ? get_string(0x1f01) : NULL;
        const unsigned char *version = get_string ? get_string(0x1f02) : NULL;
        egl_probe_log("EGL_TRACE driver_strings symbol=%p renderer=%s version=%s api=0x%x thread=%llu",
                      (void *)symbol, renderer ? (const char *)renderer : "<null>",
                      version ? (const char *)version : "<null>", eglQueryAPI(),
                      (unsigned long long)(uintptr_t)pthread_self());
    }
    return result;
}
static EGLSurface EGLAPIENTRY bridge_create(EGLDisplay d, EGLConfig c, EGLNativeWindowType w, const EGLint *a) {
    void *native = NULL; uint64_t generation;
    if (!window_runtime_acquire_native_lease((void *)w, &native, &generation)) {
        bridge_error = EGL_BAD_NATIVE_WINDOW;
        egl_probe_log("EGL_WINDOW create token=%p success=0 reason=invalid_native_window", (void *)w);
        return EGL_NO_SURFACE;
    }
    entry *e = malloc(sizeof(*e));
    if (!e) { window_runtime_release((void *)w); bridge_error = EGL_BAD_ALLOC; return EGL_NO_SURFACE; }
    EGLSurface s = eglCreateWindowSurface(d, c, (EGLNativeWindowType)native, a);
    result_black_egl("create_window",(uintptr_t)s,(uintptr_t)native,s!=EGL_NO_SURFACE);
    egl_probe_log("EGL_WINDOW create generation=%llu native=%p surface=%p success=%d thread=%llu",(unsigned long long)generation,native,(void *)s,s!=EGL_NO_SURFACE,(unsigned long long)(uintptr_t)pthread_self());
    if (s == EGL_NO_SURFACE) { free(e); window_runtime_release((void *)w); return s; }
    e->display=d; e->surface=s; e->token=(void *)w; e->busy=0;
    EGLint render_buffer=-1;e->render_buffer=eglQuerySurface(d,s,EGL_RENDER_BUFFER,&render_buffer)?render_buffer:-1;
    pthread_mutex_lock(&lock); e->next=surfaces; surfaces=e; pthread_mutex_unlock(&lock);
    return s;
}
static EGLBoolean EGLAPIENTRY bridge_destroy(EGLDisplay d, EGLSurface s) {
    pthread_mutex_lock(&lock);
    entry *e=surfaces; while (e && (e->surface!=s || e->display!=d)) e=e->next;
    if (!e) { pthread_mutex_unlock(&lock); return eglDestroySurface(d,s); }
    if (e->busy) { pthread_mutex_unlock(&lock); bridge_error=EGL_BAD_SURFACE; return EGL_FALSE; }
    e->busy=1;
    pthread_mutex_unlock(&lock);
    EGLBoolean ok=eglDestroySurface(d,s);
    result_black_egl("destroy_window",(uintptr_t)s,0,ok==EGL_TRUE);
    pthread_mutex_lock(&lock);
    if (ok) {
        entry **p=&surfaces; while (*p && *p!=e) p=&(*p)->next;
        if (*p) *p=e->next;
    } else e->busy=0;
    pthread_mutex_unlock(&lock);
    if (ok) {
        /* A different thread can still have this destroyed surface current. */
        sdk_trace_fprintf(stderr,"EGL_WINDOW destroy quarantined_lease=1 surface=%p\n",(void *)s);
        free(e);
    }
    return ok;
}
static EGLBoolean EGLAPIENTRY bridge_swap(EGLDisplay d, EGLSurface s) {
    pthread_mutex_lock(&lock);
    entry *e=surfaces;while(e&&(e->display!=d||e->surface!=s))e=e->next;
    int tracked=e!=NULL,render_buffer=e?e->render_buffer:-1;
    pthread_mutex_unlock(&lock);
    uint64_t perf_begin=perf_now();
    perf_result_event(1,(uintptr_t)s,(uintptr_t)eglGetCurrentContext());
    perf_result_event(22,(uint64_t)(int64_t)atomic_load(&observed_swap_interval),(uint64_t)(int64_t)render_buffer);
    EGLBoolean ok=eglSwapBuffers(d,s);
    perf_result_event(2,(uintptr_t)s,ok==EGL_TRUE);
    perf_egl_done(perf_begin);
    if(ok==EGL_TRUE&&tracked){atomic_store_explicit(&last_submission_ns,perf_now(),memory_order_relaxed);atomic_fetch_add_explicit(&successful_submissions,1,memory_order_relaxed);}
    unsigned long long count=atomic_fetch_add_explicit(&swap_count,1,memory_order_relaxed)+1;
    egl_probe_log("EGL_WINDOW swap attempt=%llu thread=%llu display=%p surface=%p success=%d tracked_window=%d submission_only=1",count,(unsigned long long)(uintptr_t)pthread_self(),(void *)d,(void *)s,ok==EGL_TRUE,tracked);
    return ok;
}
static EGLint EGLAPIENTRY bridge_error_get(void) {
    if (bridge_error != EGL_SUCCESS) { EGLint e=bridge_error; bridge_error=EGL_SUCCESS; return e; }
    EGLint error = eglGetError();
    egl_probe_log("EGL_TRACE get_error value=0x%x thread=%llu",error,(unsigned long long)(uintptr_t)pthread_self());
    return error;
}
static __eglMustCastToProperFunctionPointerType EGLAPIENTRY bridge_getproc(const char *name);
static EGLBoolean EGLAPIENTRY result_black_swap_interval(EGLDisplay d,EGLint interval){
 EGLBoolean ok=eglSwapInterval(d,interval);if(ok==EGL_TRUE)atomic_store(&observed_swap_interval,interval);perf_result_event(23,(uint64_t)(int64_t)interval,ok==EGL_TRUE);result_black_egl("swap_interval",(uintptr_t)d,(uint64_t)(int64_t)interval,ok==EGL_TRUE);return ok;
}
static EGLBoolean EGLAPIENTRY result_black_destroy_context(EGLDisplay d,EGLContext c){
 EGLBoolean ok=eglDestroyContext(d,c);result_black_egl("destroy_context",(uintptr_t)c,0,ok==EGL_TRUE);return ok;
}
void *egl_window_runtime_symbol(const char *name) {
    if (!name) return NULL;
    if (!strcmp(name,"eglGetDisplay")) return (void *)bridge_display;
    if (!strcmp(name,"eglInitialize")) return (void *)bridge_initialize;
    if (!strcmp(name,"eglBindAPI")) return (void *)bridge_bind_api;
    if (!strcmp(name,"eglChooseConfig")) return (void *)bridge_choose_config;
    if (!strcmp(name,"eglCreateContext")) return (void *)bridge_context;
    if (!strcmp(name,"eglDestroyContext")) return (void *)result_black_destroy_context;
    if (!strcmp(name,"eglSwapInterval")) return (void *)result_black_swap_interval;
    if (!strcmp(name,"eglMakeCurrent")) return (void *)bridge_make_current;
    if (!strcmp(name,"eglCreateWindowSurface")) return (void *)bridge_create;
    if (!strcmp(name,"eglDestroySurface")) return (void *)bridge_destroy;
    if (!strcmp(name,"eglSwapBuffers")) return (void *)bridge_swap;
    if (!strcmp(name,"eglGetError")) return (void *)bridge_error_get;
    if (!strcmp(name,"eglGetProcAddress")) return (void *)bridge_getproc;
    return NULL;
}
static __eglMustCastToProperFunctionPointerType EGLAPIENTRY bridge_getproc(const char *name) {
    void *p=egl_window_runtime_symbol(name);
    __eglMustCastToProperFunctionPointerType result =
        p ? (__eglMustCastToProperFunctionPointerType)p : eglGetProcAddress(name);
    if (name && !strcmp(name, "glGetString"))
        egl_probe_log("GL_SYMBOL api=eglGetProcAddress name=%s symbol=%p context=%p thread=%llu",
                      name, (void *)result, (void *)eglGetCurrentContext(),
                      (unsigned long long)(uintptr_t)pthread_self());
    return result;
}

