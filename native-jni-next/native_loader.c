/*
 * 原生库加载、JNI_OnLoad、RegisterNatives 注册表与 Java_ 符号查找。
 *
 * 边界：
 *   - 本文件只拥有自己 dlopen 的句柄，不碰原 libcustom_rules.so；
 *   - 加载失败保留 dlerror()/JNI_OnLoad 返回值原文，不伪造成功；
 *   - 不自动调用任何 Java_ 函数，只登记/查找；
 *   - 注册表清空先于 dlclose，避免已注册函数指针悬空；
 *   - 每个库名单一加载：未加载记录先以“加载中”发布，其他线程等待同一
 *     结果；JNI_OnLoad 只由获得加载权的线程执行一次，失败结果会广播并
 *     缓存，避免静默重试造成重复副作用；
 *   - 库记录/句柄的登记与释放由 state->mu 保护，destroy 等待在途加载结束。
 */

#include "native_loader.h"

#include "jni_bridge.h"
#include "jni_util.h"
#include "elf_loader.h"
/* Implemented once in android_dlfcn.c; shared name/cache/refcount semantics. */
extern elf_loader_t *elf_android_open_bundled_library(const char *,int,char *,size_t);

#include <dlfcn.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 自定义 ELF64 加载器：系统 dlopen 无法解析原 Android 库的 LIBC 版本化
 * 符号时，用它真实 mmap/重定位原库。返回值与句柄所有权由 elf_loader 管理。 */

#define JNI_NATIVE_CLASS_MAX 1024
#define JNI_NATIVE_ERROR_MAX 512

/* 库记录的加载状态：记录一旦进入 LOADED/FAILED 就不再改变。 */
enum jni_native_library_status {
    JNI_NATIVE_LIB_LOADING = 0, /* 记录已入链表，加载者正在 dlopen/JNI_OnLoad */
    JNI_NATIVE_LIB_LOADED,      /* 加载成功，句柄可登记/查找 */
    JNI_NATIVE_LIB_FAILED       /* 加载失败，错误文本已发布 */
};

struct jni_native_library {
    void *handle;
    elf_loader_t *custom;   /* 非 NULL 时由自定义加载器拥有 */
    char *request;
    char *path;
    jint onload_version;      /* 0 表示未导出 JNI_OnLoad */
    bool onload_called;
    enum jni_native_library_status status;
    pthread_t loading_thread; /* 仅 LOADING 有效，用于检测同线程重入 */
    char error[JNI_NATIVE_ERROR_MAX]; /* 失败时发布给等待者的原文 */
    struct jni_native_library *next;
};

struct jni_native_registration {
    char *class_name;
    char *method;
    char *sig;
    void *fn;
    struct jni_native_registration *next;
};

struct jni_native_state {
    struct jni_bridge *bridge;
    struct jni_native_library *libs;
    struct jni_native_registration *regs;
    uint32_t library_count;
    uint32_t registration_count;
    pthread_mutex_t mu;
    pthread_cond_t load_cond; /* 加载状态变化：发布结果或开始销毁 */
    uint32_t active_loads;    /* 正在加载或等待加载结果的调用数 */
    bool destroying;
    char last_error[JNI_NATIVE_ERROR_MAX];
};

/* 一次加载尝试的三种结果：
 * - OK：句柄已装载且 JNI_OnLoad（如有）成功，可以发布成功；
 * - FAILED：句柄已装载或 JNI_OnLoad 已执行后失败，必须发布失败并保留句柄；
 * - OPEN_FAILED：dlopen 与自定义加载器都失败，没有产生任何可执行副作用，
 *   调用方可以在同一记录上安全地尝试下一个候选路径。 */
enum jni_native_attempt_result {
    JNI_NATIVE_ATTEMPT_OK = 0,
    JNI_NATIVE_ATTEMPT_FAILED,
    JNI_NATIVE_ATTEMPT_OPEN_FAILED
};

typedef jint (*jni_onload_fn)(JavaVM *vm, void *reserved);

static char *dup_cstr(const char *s)
{
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

static void set_error(struct jni_native_state *state, const char *fmt, ...)
{
    if (!state) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(state->last_error, sizeof(state->last_error), fmt, ap);
    va_end(ap);
}

static void set_error_locked(struct jni_native_state *state, const char *fmt, ...)
{
    if (!state) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(state->last_error, sizeof(state->last_error), fmt, ap);
    va_end(ap);
}

static bool load_name_matches(const char *a, const char *b)
{
    return a && b && strcmp(a, b) == 0;
}

static struct jni_native_library *find_library_locked(
    struct jni_native_state *state, const char *request)
{
    for (struct jni_native_library *lib = state->libs; lib; lib = lib->next) {
        if (load_name_matches(lib->request, request) ||
            load_name_matches(lib->path, request))
            return lib;
    }
    return NULL;
}

/* 按实际后端释放一次加载引用：
 * - custom 由 elf_loader_close 释放（内部只递减引用计数，映射保留）；
 * - 系统句柄只在非 NULL 时 dlclose；
 * - 两者互斥，绝不重复释放。 */
static void release_library_backend(void *handle, elf_loader_t *custom)
{
    if (custom) {
        (void)elf_loader_close(custom);
        return;
    }
    if (handle) (void)dlclose(handle);
}

/* 唤醒所有等待者并递减在途调用计数。调用方必须持有 state->mu。 */
static void finish_active_locked(struct jni_native_state *state)
{
    if (state->active_loads > 0) --state->active_loads;
    pthread_cond_broadcast(&state->load_cond);
}

static jni_onload_fn onload_from_handle(void *handle)
{
    dlerror();
    void *sym = dlsym(handle, "JNI_OnLoad");
    if (!sym) {
        /* 未导出 JNI_OnLoad 是正常情况，但清掉可能的 dlerror。 */
        (void)dlerror();
        return NULL;
    }
    jni_onload_fn fn = NULL;
    if (sizeof(fn) != sizeof(sym)) return NULL;
    memcpy(&fn, &sym, sizeof(fn));
    return fn;
}

static jni_onload_fn onload_from_library(const struct jni_native_library *lib)
{
    void *sym = NULL;
    if (!lib) return NULL;
    if (lib->handle) {
        dlerror();
        sym = dlsym(lib->handle, "JNI_OnLoad");
        if (!sym) { (void)dlerror(); return NULL; }
    } else if (lib->custom) {
        sym = elf_loader_symbol(lib->custom, "JNI_OnLoad");
        if (!sym) return NULL;
    } else return NULL;
    jni_onload_fn fn = NULL;
    if (sizeof(fn) != sizeof(sym)) return NULL;
    memcpy(&fn, &sym, sizeof(fn));
    return fn;
}

static bool version_supported(jint version)
{
    return version == JNI_VERSION_1_2 || version == JNI_VERSION_1_4 || version == JNI_VERSION_1_6;
}

/* 调用时持有 state->mu。分配并发布 LOADING 记录；失败返回 NULL 并把
 * 错误文本写入 err，由调用方决定 set_error 与广播。 */
static struct jni_native_library *load_record_create_locked(
    struct jni_native_state *state, const char *request, const char *path,
    char *err, size_t errlen)
{
    struct jni_native_library *lib =
        (struct jni_native_library *)calloc(1, sizeof(*lib));
    if (!lib) {
        snprintf(err, errlen, "load(%s) 分配库记录失败", path);
        return NULL;
    }
    lib->request = dup_cstr(request);
    lib->path = dup_cstr(path);
    if (!lib->request || !lib->path) {
        free(lib->request);
        free(lib->path);
        free(lib);
        snprintf(err, errlen, "load(%s) 分配路径字符串失败", path);
        return NULL;
    }
    lib->status = JNI_NATIVE_LIB_LOADING;
    lib->loading_thread = pthread_self();
    lib->next = state->libs;
    state->libs = lib;
    return lib;
}

/*
 * 结束一个已经释放 GIL 的等待调用。调用时持有 state->mu，且本线程的
 * active_loads 尚未递减。必须先释放 state->mu 再恢复 GIL（全模块锁序
 * 为 GIL -> state->mu）；active_loads 保证在此期间 state/bridge 不会被
 * jni_native_state_destroy 回收。error_text 非 NULL 时在重新持锁后写入
 * last_error。返回后调用方不得再访问 state。
 */
static void finish_waiting_locked(struct jni_native_state *state,
                                  unsigned gil_depth, const char *error_text)
{
    pthread_mutex_unlock(&state->mu);
    jni_bridge_gil_restore(state->bridge, gil_depth);
    pthread_mutex_lock(&state->mu);
    if (error_text) set_error(state, "%s", error_text);
    finish_active_locked(state);
    pthread_mutex_unlock(&state->mu);
}

/*
 * 慢路径：已确定需要等待 lib 发布。进入时持有 active_loads 计数但不持有
 * state->mu。直接等待传进来的记录指针，而不是重新按名字查找：加载流程
 * 有 request/path 两个别名键，重查依赖键在等待期间保持不变，容易因后续
 * 改动产生“查不到旧记录 -> 再建一条 -> 重复 JNI_OnLoad”的窗口。记录在
 * destroy 前不会被移除，且本线程的 active_loads 保证 destroy 不会回收它。
 *
 * 先释放当前线程可能持有的 DVM GIL 再睡眠：若等待者持 GIL 睡眠，加载者
 * JNI_OnLoad 后的 jni_bridge_gil_restore() 会一直等锁，形成死锁。
 */
static int load_begin_wait(struct jni_native_state *state,
                           struct jni_native_library *lib, const char *request,
                           bool *result)
{
    unsigned gil_depth = jni_bridge_gil_drop(state->bridge);
    char text[JNI_NATIVE_ERROR_MAX];

    pthread_mutex_lock(&state->mu);
    for (;;) {
        if (state->destroying) {
            snprintf(text, sizeof(text), "load(%s)：native 状态正在销毁",
                     request);
            finish_waiting_locked(state, gil_depth, text);
            return -1;
        }

        if (lib->status != JNI_NATIVE_LIB_LOADING) {
            bool ok = lib->status == JNI_NATIVE_LIB_LOADED;
            if (ok) {
                text[0] = '\0';
            } else if (lib->error[0]) {
                snprintf(text, sizeof(text), "load(%s) 之前已失败：%s",
                         request, lib->error);
            } else {
                snprintf(text, sizeof(text),
                         "load(%s) 之前已失败，详见先前加载错误", request);
            }
            finish_waiting_locked(state, gil_depth, ok ? NULL : text);
            *result = ok;
            return 0;
        }

        if (pthread_equal(lib->loading_thread, pthread_self())) {
            /* 同一线程在 JNI_OnLoad 内再次加载同一库：等待自己会死锁。 */
            snprintf(text, sizeof(text), "load(%s) 递归加载同一库，已拒绝",
                     request);
            finish_waiting_locked(state, gil_depth, text);
            return -1;
        }

        pthread_cond_wait(&state->load_cond, &state->mu);
    }
}

/*
 * 开始一次加载。返回值：
 *   1  = 当前线程获得加载权，*out 为已发布的 LOADING 记录；
 *   0  = 已有最终结果，*result 为该结果；
 *   -1 = 无法开始（分配/销毁中/递归），错误已写入 last_error。
 *
 * 记录在真正打开库之前入链表，其他线程通过 request 或 path 命中后等待，
 * 因此同一个库名不会并发执行两次 JNI_OnLoad。
 */
static int load_begin(struct jni_native_state *state, const char *request,
                      const char *path, struct jni_native_library **out,
                      bool *result)
{
    *out = NULL;
    *result = false;

    pthread_mutex_lock(&state->mu);
    if (state->destroying) {
        set_error(state, "load(%s)：native 状态正在销毁", request);
        pthread_mutex_unlock(&state->mu);
        return -1;
    }
    ++state->active_loads;

    struct jni_native_library *lib = find_library_locked(state, request);
    if (lib && lib->status == JNI_NATIVE_LIB_LOADING) {
        if (pthread_equal(lib->loading_thread, pthread_self())) {
            set_error(state, "load(%s) 递归加载同一库，已拒绝", request);
            finish_active_locked(state);
            pthread_mutex_unlock(&state->mu);
            return -1;
        }
        /* 需要等待：先释放 state->mu，慢路径再释放 GIL 并睡眠。 */
        pthread_mutex_unlock(&state->mu);
        return load_begin_wait(state, lib, request, result);
    }

    if (lib) {
        bool ok = lib->status == JNI_NATIVE_LIB_LOADED;
        if (!ok) {
            if (lib->error[0]) {
                set_error(state, "load(%s) 之前已失败：%s", request,
                          lib->error);
            } else {
                set_error(state, "load(%s) 之前已失败，详见先前加载错误",
                          request);
            }
        }
        finish_active_locked(state);
        pthread_mutex_unlock(&state->mu);
        *result = ok;
        return 0;
    }

    char text[JNI_NATIVE_ERROR_MAX];
    lib = load_record_create_locked(state, request, path, text, sizeof(text));
    if (!lib) {
        set_error(state, "%s", text);
        finish_active_locked(state);
        pthread_mutex_unlock(&state->mu);
        return -1;
    }
    pthread_mutex_unlock(&state->mu);
    *out = lib;
    return 1;
}

/*
 * 真正执行 dlopen + 自定义 ELF 加载 + JNI_OnLoad。调用方必须持有该记录的
 * 加载权且不得持有 state->mu（JNI_OnLoad 可能回调 JNI 桥）。
 *
 * 失败文本写入 err，由调用方在发布阶段复制进记录并广播。JNI_OnLoad 的
 * 版本校验、调用前/后 pending 异常检查顺序与语义保持不变。
 */
static enum jni_native_attempt_result load_attempt(
    struct jni_native_state *state, struct jni_native_library *lib,
    const char *path, char *err, size_t errlen)
{
    if (err && errlen) err[0] = '\0';

    {
        static _Atomic unsigned load_probe_count;
        if (atomic_fetch_add(&load_probe_count, 1) < 128) {
            FILE *lf = fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt", "a");
            if (lf) {
                fprintf(lf, "NATIVE_LOAD_ATTEMPT path=%s\n", path ? path : "(null)");
                fclose(lf);
            }
        }
    }
    bool force_android_main = false;
#ifdef LUNARIA_NATIVE_MAIN_LOAD_PROBE
    const char *basename = strrchr(path, '/');
    basename = basename ? basename + 1 : path;
    force_android_main = strcmp(basename, "libmain.so") == 0;
#endif
    char cache_error[128];
    elf_loader_t *custom = elf_android_open_bundled_library(path,1,cache_error,sizeof(cache_error));
    dlerror();
    void *handle = (force_android_main || custom) ? NULL : dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!handle && !custom) {
        const char *dlerr = dlerror();
        char elf_error[JNI_NATIVE_ERROR_MAX] = {0};
        /* System.load/loadLibrary is a real load for every library. A map-only
         * diagnostic must never be published as a Java native library. */
        custom = elf_android_open_bundled_library(path,0,elf_error,sizeof(elf_error));
        if (!custom) {
            snprintf(err, errlen,
                     "dlopen(%s) 失败：%s；自定义 ELF 加载失败：%s", path,
                     dlerr ? dlerr : "unknown dlerror",
                     elf_error[0] ? elf_error : "unknown error");
#ifdef LUNARIA_NATIVE_MAIN_LOAD_PROBE
            if(force_android_main){
                extern int sdk_trace_fprintf(FILE *,const char *,...);
                sdk_trace_fprintf(stderr,"NATIVE_MAIN_BOOTSTRAP_MAP_FAILED path=%s error=%s\n",path,elf_error);
                return JNI_NATIVE_ATTEMPT_FAILED;
            }
#endif
            return JNI_NATIVE_ATTEMPT_OPEN_FAILED;
        }
    }

    /* 句柄先记在记录上；成功/失败发布前该记录对其他查找者不可见。 */
    lib->handle = handle;
    lib->custom = custom;
    if (custom) {
        FILE *lf = fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt", "a");
        if (lf) { fprintf(lf, "NATIVE_LOAD_CUSTOM path=%s status=%s\n", path, elf_loader_status(custom)); fclose(lf); }
    }

    jni_onload_fn onload = onload_from_library(lib);
#ifdef LUNARIA_NATIVE_MAIN_LOAD_PROBE
    if(force_android_main){
        extern int sdk_trace_fprintf(FILE *,const char *,...);
        void *own=custom?elf_loader_own_executable_symbol(custom,"JNI_OnLoad"):NULL;
        sdk_trace_fprintf(stderr,"NATIVE_MAIN_BOOTSTRAP_SOURCE path=%s custom=%d own=%p scoped=%p status=%s\n",path,custom!=NULL,own,custom?elf_loader_symbol(custom,"JNI_OnLoad"):NULL,custom?elf_loader_status(custom):"no custom map");
        if(!own){snprintf(err,errlen,"original main own JNI_OnLoad unavailable");return JNI_NATIVE_ATTEMPT_FAILED;}
        memcpy(&onload,&own,sizeof(onload));
    }
#endif
    if (onload) {
        JNIEnv *env = jni_bridge_env(state->bridge);
        if (!env) {
            snprintf(err, errlen, "JNI_OnLoad(%s)：当前线程 attach 失败",
                     path);
            return JNI_NATIVE_ATTEMPT_FAILED;
        }
        if (jni_bridge_pending(state->bridge)) {
            snprintf(err, errlen, "JNI_OnLoad(%s)：调用前已有未处理异常",
                     path);
            return JNI_NATIVE_ATTEMPT_FAILED;
        }
        JavaVM *vm = jni_bridge_java_vm(state->bridge);
        if (!vm) {
            snprintf(err, errlen, "JNI_OnLoad(%s)：JavaVM 不可用", path);
            return JNI_NATIVE_ATTEMPT_FAILED;
        }
        lib->onload_called = true;
        unsigned depth = jni_bridge_gil_drop(state->bridge);
        jint version = onload(vm, NULL);
        jni_bridge_gil_restore(state->bridge, depth);
        lib->onload_version = version;
        {
            FILE *lf = fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt", "a");
            if (lf) { fprintf(lf, "NATIVE_JNI_ONLOAD path=%s version=0x%x supported=%d pending=%d\n", path, (unsigned)version, version_supported(version), jni_bridge_pending(state->bridge)); fclose(lf); }
        }
#ifdef LUNARIA_NATIVE_MAIN_LOAD_PROBE
        if(force_android_main){
            extern int sdk_trace_fprintf(FILE *,const char *,...);
            sdk_trace_fprintf(stderr,"NATIVE_MAIN_BOOTSTRAP_RETURN version=0x%x registrations=%u pending=%d\n",(unsigned)version,jni_bridge_native_registration_count(jni_bridge_vm(state->bridge)),jni_bridge_pending(state->bridge));
        }
#endif
        if (version == JNI_ERR || !version_supported(version)) {
            snprintf(err, errlen,
                     "JNI_OnLoad(%s) 返回不兼容版本 0x%08x（仅接受 1.2/1.4/1.6）",
                     path, (unsigned)version);
            return JNI_NATIVE_ATTEMPT_FAILED;
        }
        if (jni_bridge_pending(state->bridge)) {
            snprintf(err, errlen, "JNI_OnLoad(%s) 留下了未处理 Java 异常",
                     path);
            return JNI_NATIVE_ATTEMPT_FAILED;
        }
    }

    return JNI_NATIVE_ATTEMPT_OK;
}

/*
 * 发布最终结果并唤醒等待者。失败记录保留句柄直到 state destroy：
 * JNI_OnLoad 内可能已经 RegisterNatives 注册了函数指针，立即释放会悬空。
 * 必须在释放最后一个 active_loads 之前把 last_error 写好，避免 destroy
 * 释放 state 后仍写入。返回本次加载是否成功。
 */
static bool load_publish(struct jni_native_state *state,
                         struct jni_native_library *lib, bool ok,
                         const char *error_text)
{
    pthread_mutex_lock(&state->mu);
    lib->status = ok ? JNI_NATIVE_LIB_LOADED : JNI_NATIVE_LIB_FAILED;
    if (!ok) {
        snprintf(lib->error, sizeof(lib->error), "%s",
                 error_text && *error_text ? error_text
                                           : "load 失败（无错误文本）");
        set_error(state, "%s", lib->error);
    }
    ++state->library_count;
    finish_active_locked(state);
    pthread_mutex_unlock(&state->mu);
    return ok;
}

/* 单路径加载：一个 request 对应唯一的记录与唯一一次 JNI_OnLoad。 */
static bool load_common(struct jni_native_state *state, const char *request,
                        const char *path)
{
    if (!state || !path || !*path) {
        if (state) set_error(state, "load: 路径为空");
        return false;
    }

    const char *key = request ? request : path;
    struct jni_native_library *lib = NULL;
    bool result = false;
    int begun = load_begin(state, key, path, &lib, &result);
    if (begun < 0) return false;
    if (begun == 0) return result;

    char err[JNI_NATIVE_ERROR_MAX] = {0};
    enum jni_native_attempt_result attempt =
        load_attempt(state, lib, path, err, sizeof(err));
    if (attempt != JNI_NATIVE_ATTEMPT_OK) {
        FILE *lf = fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt", "a");
        if (lf) {
            fprintf(lf, "NATIVE_LOAD_FAILED path=%s attempt=%d error=%s\n",
                    path, (int)attempt, err[0] ? err : "(no error)");
            fclose(lf);
        }
    }
    return load_publish(state, lib, attempt == JNI_NATIVE_ATTEMPT_OK, err);
}

struct jni_native_state *jni_native_state_create(struct jni_bridge *bridge)
{
    if (!bridge) return NULL;
    struct jni_native_state *state = (struct jni_native_state *)calloc(
        1, sizeof(*state));
    if (!state) return NULL;
    state->bridge = bridge;
    if (pthread_mutex_init(&state->mu, NULL) != 0) {
        free(state);
        return NULL;
    }
    if (pthread_cond_init(&state->load_cond, NULL) != 0) {
        pthread_mutex_destroy(&state->mu);
        free(state);
        return NULL;
    }
    return state;
}

static void free_registration(struct jni_native_registration *reg)
{
    if (!reg) return;
    free(reg->class_name);
    free(reg->method);
    free(reg->sig);
    free(reg);
}

void jni_native_state_destroy(struct jni_native_state *state)
{
    if (!state) return;
    pthread_mutex_lock(&state->mu);
    state->destroying = true;
    pthread_cond_broadcast(&state->load_cond);

    /* 等待在途加载/等待者退出：它们可能仍持有库记录指针或正在回调桥。
     * 本函数不得从加载线程自身（例如 JNI_OnLoad 内）调用。
     *
     * 等待者退出前必须先恢复 DVM GIL（finish_waiting_locked），若本线程
     * 持 GIL 在此等待，双方会互相阻塞。因此在进入 cond_wait 前释放本
     * 线程的 GIL；这是在持 state->mu 时只“释放”、不会阻塞，且与全模块
     * GIL -> state->mu 锁序一致。 */
    unsigned gil_depth = 0;
    bool gil_dropped = false;
    if (state->active_loads > 0) {
        gil_depth = jni_bridge_gil_drop(state->bridge);
        gil_dropped = true;
    }
    while (state->active_loads > 0)
        pthread_cond_wait(&state->load_cond, &state->mu);
    if (gil_dropped) {
        pthread_mutex_unlock(&state->mu);
        jni_bridge_gil_restore(state->bridge, gil_depth);
        pthread_mutex_lock(&state->mu);
    }

    struct jni_native_registration *reg = state->regs;
    state->regs = NULL;
    state->registration_count = 0;
    struct jni_native_library *lib = state->libs;
    state->libs = NULL;
    state->library_count = 0;
    pthread_mutex_unlock(&state->mu);

    /* 先清注册表，再释放库：注册的函数指针必须先失效。 */
    while (reg) {
        struct jni_native_registration *next = reg->next;
        free_registration(reg);
        reg = next;
    }
    while (lib) {
        struct jni_native_library *next = lib->next;
        release_library_backend(lib->handle, lib->custom);
        free(lib->request);
        free(lib->path);
        free(lib);
        lib = next;
    }
    pthread_cond_destroy(&state->load_cond);
    pthread_mutex_destroy(&state->mu);
    free(state);
}

bool jni_native_load_name(struct jni_native_state *state, const char *name)
{
    if (!state || !name || !*name) {
        if (state) set_error(state, "loadLibrary: 名称为空");
        return false;
    }
    if (strchr(name, '/') || strchr(name, '\\')) {
        return load_common(state, name, name);
    }

    char candidate[1024];
    /* System.loadLibrary("foo")：先找 libfoo.so，再尝试原始名。 */
    if (strncmp(name, "lib", 3) == 0 && strstr(name, ".so")) {
        snprintf(candidate, sizeof(candidate), "%s", name);
    } else {
        snprintf(candidate, sizeof(candidate), "lib%s.so", name);
    }

    /* 整个“候选名 -> 原始名”解析只由一个线程执行，等待者共享最终结果。 */
    struct jni_native_library *lib = NULL;
    bool result = false;
    int begun = load_begin(state, name, candidate, &lib, &result);
    if (begun < 0) return false;
    if (begun == 0) return result;

    char first_error[JNI_NATIVE_ERROR_MAX] = {0};
    char err[JNI_NATIVE_ERROR_MAX] = {0};
    enum jni_native_attempt_result attempt =
        load_attempt(state, lib, candidate, err, sizeof(err));
    if (attempt == JNI_NATIVE_ATTEMPT_OK)
        return load_publish(state, lib, true, NULL);
    if (attempt == JNI_NATIVE_ATTEMPT_FAILED)
        return load_publish(state, lib, false, err);

    /* 第一候选在打开阶段失败（未产生句柄，也未执行 JNI_OnLoad），
     * 与旧行为一致地尝试原始名；副作用不会重复。
     *
     * 不修改记录的 path：request 是原始名、path 是第一候选名，两者正好
     * 覆盖先后使用这两个名字的并发调用；在加载中改 path 会造成别名窗口，
     * 使并发调用匹配不到记录而重复加载。 */
    snprintf(first_error, sizeof(first_error), "%s", err);
    err[0] = '\0';
    attempt = load_attempt(state, lib, name, err, sizeof(err));
    if (attempt == JNI_NATIVE_ATTEMPT_OK)
        return load_publish(state, lib, true, NULL);

    char combined[JNI_NATIVE_ERROR_MAX];
    snprintf(combined, sizeof(combined), "%s；重试 %s 也失败：%s", first_error,
             name, err);
    return load_publish(state, lib, false, combined);
}

bool jni_native_load_path(struct jni_native_state *state, const char *path)
{
    return load_common(state, path, path);
}

static struct jni_native_registration *find_registration_locked(
    struct jni_native_state *state, const char *class_name, const char *method,
    const char *sig)
{
    for (struct jni_native_registration *r = state->regs; r; r = r->next) {
        if (strcmp(r->class_name, class_name) == 0 &&
            strcmp(r->method, method) == 0 &&
            strcmp(r->sig, sig) == 0)
            return r;
    }
    return NULL;
}

static void *find_symbol_in_all_locked(struct jni_native_state *state,
                                       const char *symbol)
{
    for (struct jni_native_library *lib = state->libs; lib; lib = lib->next) {
        /* 加载中/失败的记录不参与符号查找，避免读到半初始化句柄。 */
        if (lib->status != JNI_NATIVE_LIB_LOADED) continue;
        if (lib->handle) {
            dlerror();
            void *sym = dlsym(lib->handle, symbol);
            if (sym) return sym;
            const char *err = dlerror();
            if (err) {
                /* 只保留最后一个真实 dlsym 错误，未找到符号通常 err 为 NULL。 */
                set_error_locked(state, "dlsym(%s) 失败：%s", symbol, err);
            }
        } else if (lib->custom) {
            void *sym = elf_loader_symbol(lib->custom, symbol);
            if (sym) return sym;
        }
    }
    return NULL;
}

bool jni_native_is_registered(struct jni_native_state *state,
 const char *class_name, const char *method, const char *sig)
{
    if (!state || !class_name || !method || !sig) return false;
    pthread_mutex_lock(&state->mu);
    bool found = find_registration_locked(state, class_name, method, sig) != NULL;
    pthread_mutex_unlock(&state->mu);
    return found;
}

void *jni_native_find(struct jni_native_state *state, const char *class_name,
                      const char *method, const char *sig)
{
    if (!state || !class_name || !method || !sig) return NULL;

    pthread_mutex_lock(&state->mu);
    struct jni_native_registration *reg = find_registration_locked(
        state, class_name, method, sig);
    if (reg) {
        void *fn = reg->fn;
        pthread_mutex_unlock(&state->mu);
        return fn;
    }

    char long_name[JNI_MANGLE_MAX];
    char short_name[JNI_MANGLE_MAX];
    bool have_long = jni_java_symbol_long(class_name, method, sig, long_name,
                                          sizeof(long_name));
    bool have_short = jni_java_symbol_short(class_name, method, short_name,
                                            sizeof(short_name));
    if (!have_long && !have_short) {
        set_error_locked(state, "无法为 %s.%s%s 生成 JNI 符号名", class_name,
                         method, sig);
        pthread_mutex_unlock(&state->mu);
        return NULL;
    }

    /* 先查长名，再查短名；短名只在没有长名匹配时使用。 */
    if (have_long) {
        void *sym = find_symbol_in_all_locked(state, long_name);
        if (sym) {
            pthread_mutex_unlock(&state->mu);
            return sym;
        }
    }
    if (have_short) {
        void *sym = find_symbol_in_all_locked(state, short_name);
        if (sym) {
            pthread_mutex_unlock(&state->mu);
            return sym;
        }
    }
    set_error_locked(state, "找不到 native 符号 %s 或 %s", long_name,
                     short_name);
    pthread_mutex_unlock(&state->mu);
    return NULL;
}

static bool validate_method(struct jni_native_state *state, jclass cls,
                            const JNINativeMethod *m)
{
    if (!m->name || !*m->name || !m->signature || !*m->signature ||
        !m->fnPtr) {
        set_error(state,
                  "RegisterNatives 条目缺少 name/signature/fnPtr");
        return false;
    }
    if (m->signature[0] != '(') {
        set_error(state, "RegisterNatives %s 的签名不是方法描述符：%s",
                  m->name, m->signature);
        return false;
    }
    jni_sig_info info;
    if (!jni_sig_parse(m->signature, &info)) {
        set_error(state, "RegisterNatives %s 的签名非法：%s", m->name,
                  m->signature);
        return false;
    }
    if (!jni_bridge_validate_native(state->bridge, cls, m->name,
                                    m->signature)) {
        set_error(state,
                  "RegisterNatives %s%s 不是该类的 DEX native 方法",
                  m->name, m->signature);
        return false;
    }
    return true;
}

bool jni_native_register(struct jni_native_state *state, jclass cls,
                         const JNINativeMethod *methods, jint count)
{
    if (!state || !methods || count < 0) {
        if (state) set_error(state, "RegisterNatives 参数无效");
        return false;
    }
    if (count == 0) return true;

    char class_name[JNI_NATIVE_CLASS_MAX];
    if (!jni_bridge_class_name(state->bridge, cls, class_name,
                               sizeof(class_name))) {
        set_error(state, "RegisterNatives 无法解析类 token");
        return false;
    }

    /* 第一阶段：只校验，不写入，保证失败不部分提交。 */
    for (jint i = 0; i < count; ++i) {
        if (!validate_method(state, cls, &methods[i])) return false;
        for (jint j = 0; j < i; ++j) {
            if (strcmp(methods[i].name, methods[j].name) == 0 &&
                strcmp(methods[i].signature, methods[j].signature) == 0) {
                set_error(state, "RegisterNatives 输入含重复方法 %s%s",
                          methods[i].name, methods[i].signature);
                return false;
            }
        }
    }

    /* 第二阶段：先分配全部节点；任何分配失败都整体回滚。 */
    struct jni_native_registration *head = NULL;
    struct jni_native_registration *tail = NULL;
    for (jint i = 0; i < count; ++i) {
        struct jni_native_registration *r =
            (struct jni_native_registration *)calloc(1, sizeof(*r));
        if (!r) {
            set_error(state, "RegisterNatives 内存不足");
            goto rollback;
        }
        r->class_name = dup_cstr(class_name);
        r->method = dup_cstr(methods[i].name);
        r->sig = dup_cstr(methods[i].signature);
        r->fn = methods[i].fnPtr;
        if (!r->class_name || !r->method || !r->sig) {
            free_registration(r);
            set_error(state, "RegisterNatives 内存不足");
            goto rollback;
        }
        if (tail) tail->next = r; else head = r;
        tail = r;
    }

    pthread_mutex_lock(&state->mu);
    while (head) {
        struct jni_native_registration *next = head->next;
        struct jni_native_registration *existing = find_registration_locked(
            state, head->class_name, head->method, head->sig);
        if (existing) {
            /* 同一类/方法/签名重复 RegisterNatives：更新函数指针。 */
            existing->fn = head->fn;
            head->next = NULL;
            free_registration(head);
        } else {
            head->next = state->regs;
            state->regs = head;
            ++state->registration_count;
        }
        head = next;
    }
    pthread_mutex_unlock(&state->mu);
    return true;

rollback:
    while (head) {
        struct jni_native_registration *next = head->next;
        free_registration(head);
        head = next;
    }
    return false;
}

bool jni_native_unregister(struct jni_native_state *state, jclass cls)
{
    if (!state) return false;
    char class_name[JNI_NATIVE_CLASS_MAX];
    if (!jni_bridge_class_name(state->bridge, cls, class_name,
                               sizeof(class_name))) {
        set_error(state, "UnregisterNatives 无法解析类 token");
        return false;
    }
    struct jni_native_registration *removed = NULL;
    pthread_mutex_lock(&state->mu);
    struct jni_native_registration **pp = &state->regs;
    while (*pp) {
        struct jni_native_registration *r = *pp;
        if (strcmp(r->class_name, class_name) == 0) {
            *pp = r->next;
            r->next = removed;
            removed = r;
            if (state->registration_count > 0) --state->registration_count;
            continue;
        }
        pp = &r->next;
    }
    pthread_mutex_unlock(&state->mu);
    while (removed) {
        struct jni_native_registration *next = removed->next;
        free_registration(removed);
        removed = next;
    }
    return true;
}

const char *jni_native_last_error(struct jni_native_state *state)
{
    return state ? state->last_error : "jni_native_state is NULL";
}

uint32_t jni_native_library_count(struct jni_native_state *state)
{
    if (!state) return 0;
    pthread_mutex_lock(&state->mu);
    uint32_t n = state->library_count;
    pthread_mutex_unlock(&state->mu);
    return n;
}

uint32_t jni_native_registration_count(struct jni_native_state *state)
{
    if (!state) return 0;
    pthread_mutex_lock(&state->mu);
    uint32_t n = state->registration_count;
    pthread_mutex_unlock(&state->mu);
    return n;
}
