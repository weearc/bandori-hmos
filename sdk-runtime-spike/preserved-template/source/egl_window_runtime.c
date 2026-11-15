#include "egl_window_runtime.h"
#include "window_runtime.h"
#include <EGL/egl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <errno.h>
extern int sdk_trace_fprintf(FILE *, const char *, ...);

typedef struct entry { EGLDisplay display; EGLSurface surface; void *token; int busy; struct entry *next; } entry;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static entry *surfaces;
static _Atomic unsigned long long swap_count;
static _Atomic unsigned long long successful_submissions;
unsigned long long egl_window_runtime_successful_submissions(void){return atomic_load_explicit(&successful_submissions,memory_order_relaxed);}
static _Thread_local EGLint bridge_error = EGL_SUCCESS;
static void egl_probe_log(const char *format, ...) {
    int saved_errno = errno;
    char message[768];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof message, format, args);
    va_end(args);
    sdk_trace_fprintf(stderr, "%s\n", message);
    FILE *file = fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt", "a");
    if (file) {
        fprintf(file, "%s\n", message);
        fclose(file);
    }
    errno = saved_errno;
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
    egl_probe_log("EGL_WINDOW create generation=%llu native=%p surface=%p success=%d thread=%llu",(unsigned long long)generation,native,(void *)s,s!=EGL_NO_SURFACE,(unsigned long long)(uintptr_t)pthread_self());
    if (s == EGL_NO_SURFACE) { free(e); window_runtime_release((void *)w); return s; }
    e->display=d; e->surface=s; e->token=(void *)w; e->busy=0;
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
    int tracked=e!=NULL;
    pthread_mutex_unlock(&lock);
    EGLBoolean ok=eglSwapBuffers(d,s);
    if(ok==EGL_TRUE&&tracked)atomic_fetch_add_explicit(&successful_submissions,1,memory_order_relaxed);
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
void *egl_window_runtime_symbol(const char *name) {
    if (!name) return NULL;
    if (!strcmp(name,"eglGetDisplay")) return (void *)bridge_display;
    if (!strcmp(name,"eglInitialize")) return (void *)bridge_initialize;
    if (!strcmp(name,"eglBindAPI")) return (void *)bridge_bind_api;
    if (!strcmp(name,"eglChooseConfig")) return (void *)bridge_choose_config;
    if (!strcmp(name,"eglCreateContext")) return (void *)bridge_context;
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

