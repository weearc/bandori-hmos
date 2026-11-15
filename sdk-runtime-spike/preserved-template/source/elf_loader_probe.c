/*
 * 自定义 ELF 加载器的运行探针（简体中文注释）。
 *
 * 只做真实加载与符号查找：
 *   - 不调用任何 Java_ 函数；
 *   - 不调用 JNI_OnLoad；
 *   - 不把“文件存在”当作“加载成功”；
 *   - 失败时保留首个失败阶段、符号和原始错误。
 */

#define _GNU_SOURCE 1

#include "elf_loader_probe.h"
#include "elf_loader.h"

#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define PROBE_LIB_COUNT 4u
#define PROBE_EXPORT_COUNT 4u
#define PROBE_ERROR_MAX 512
#define PROBE_STATUS_MAX 2048
#define PROBE_JSON_MAX 32768
#define PROBE_DEFAULT_DIR "/data/storage/el1/bundle/libs/arm64"

struct probe_lib {
    const char *name;
    char path[PATH_MAX];
    int ok;
    char error[PROBE_ERROR_MAX];
    char status[PROBE_STATUS_MAX];
};

struct probe_export {
    const char *name;
    int found;
};

struct probe_buf {
    char *buf;
    size_t cap;
    size_t len;
    int overflow;
};

static const char *const g_lib_names[PROBE_LIB_COUNT] = {
    "libcrypto_1_1.so",
    "libssl_1_1.so",
    "libcustom_rules.so",
    "libblkv.so",
};

static const char *const g_export_names[PROBE_EXPORT_COUNT] = {
    "Java_com_base_crules_CustomRules_decodeStr",
    "Java_com_base_crules_CustomRules_encodeStr",
    "Java_com_base_crules_CustomRules_getVersion",
    "Java_com_base_crules_CustomRules_getTokens",
};

static pthread_mutex_t g_probe_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_running;
static char g_dir[PATH_MAX];
static struct probe_lib g_libs[PROBE_LIB_COUNT];
static struct probe_export g_exports[PROBE_EXPORT_COUNT];
static char g_last_error[PROBE_ERROR_MAX];
static char g_json[PROBE_JSON_MAX] =
    "{\"version\":1,\"ran\":false,\"reason\":\"not-run\"}";

static void probe_copy(char *dst, size_t cap, const char *src)
{
    if (!dst || cap == 0) return;
    if (!src) src = "";
    snprintf(dst, cap, "%s", src);
    dst[cap - 1] = '\0';
}

static int probe_join_path(char *out, size_t cap, const char *dir,
                           const char *name)
{
    int n;
    size_t dlen;
    if (!out || cap == 0 || !dir || !name) return 0;
    dlen = strlen(dir);
    if (dlen && dir[dlen - 1] == '/')
        n = snprintf(out, cap, "%s%s", dir, name);
    else
        n = snprintf(out, cap, "%s/%s", dir, name);
    if (n < 0 || (size_t)n >= cap) {
        out[cap - 1] = '\0';
        return 0;
    }
    return 1;
}

static void probe_extract_stage(const char *err, char *out, size_t cap)
{
    const char *p;
    size_t n;
    if (!out || cap == 0) return;
    out[0] = '\0';
    if (!err) return;
    p = strstr(err, "stage=");
    if (!p) {
        probe_copy(out, cap, "open");
        return;
    }
    p += 6;
    n = strcspn(p, ";");
    if (n >= cap) n = cap - 1;
    memcpy(out, p, n);
    out[n] = '\0';
}

static void probe_extract_symbol(const char *err, char *out, size_t cap)
{
    static const char *const markers[] = {
        "未解析符号：",
        "不支持 TLS 符号：",
        "不支持 GNU IFUNC 符号：",
        "不支持 SHN_COMMON 符号：",
        "加载依赖 ",
        NULL,
    };
    const char *start = NULL;
    size_t i;
    size_t n = 0;
    if (!out || cap == 0) return;
    out[0] = '\0';
    if (!err) return;
    for (i = 0; markers[i]; ++i) {
        const char *p = strstr(err, markers[i]);
        if (p) {
            start = p + strlen(markers[i]);
            break;
        }
    }
    if (!start) return;
    while (start[n] && n + 1 < cap) {
        unsigned char c = (unsigned char)start[n];
        if (c == ';' || c == ' ' || c == '\n' || c == '\r' ||
            c == '\t' || c == 0xef /* 全角括号的首字节 */)
            break;
        out[n] = (char)c;
        ++n;
    }
    out[n] = '\0';
}

static void pb_append(struct probe_buf *b, const char *s)
{
    size_t n;
    if (!b || !b->buf || b->cap == 0 || b->overflow) return;
    if (!s) s = "";
    n = strlen(s);
    if (b->len >= b->cap || n >= b->cap - b->len) {
        b->overflow = 1;
        b->buf[b->len < b->cap ? b->len : b->cap - 1] = '\0';
        return;
    }
    memcpy(b->buf + b->len, s, n);
    b->len += n;
    b->buf[b->len] = '\0';
}

static void pb_printf(struct probe_buf *b, const char *fmt, ...)
{
    va_list ap;
    int n;
    size_t avail;
    if (!b || !b->buf || b->cap == 0 || b->overflow) return;
    if (b->len >= b->cap) {
        b->overflow = 1;
        return;
    }
    avail = b->cap - b->len;
    va_start(ap, fmt);
    n = vsnprintf(b->buf + b->len, avail, fmt, ap);
    va_end(ap);
    if (n < 0) {
        b->overflow = 1;
        return;
    }
    if ((size_t)n >= avail) {
        b->overflow = 1;
        b->len = b->cap - 1;
        b->buf[b->len] = '\0';
        return;
    }
    b->len += (size_t)n;
}

static void pb_json_string(struct probe_buf *b, const char *s)
{
    size_t i;
    static const char hex[] = "0123456789abcdef";
    if (!b || b->overflow) return;
    pb_append(b, "\"");
    if (!s) s = "";
    for (i = 0; s[i] && !b->overflow; ++i) {
        unsigned char c = (unsigned char)s[i];
        char esc[7];
        switch (c) {
        case '"': pb_append(b, "\\\""); break;
        case '\\': pb_append(b, "\\\\"); break;
        case '\b': pb_append(b, "\\b"); break;
        case '\f': pb_append(b, "\\f"); break;
        case '\n': pb_append(b, "\\n"); break;
        case '\r': pb_append(b, "\\r"); break;
        case '\t': pb_append(b, "\\t"); break;
        default:
            if (c < 0x20) {
                esc[0] = '\\';
                esc[1] = 'u';
                esc[2] = '0';
                esc[3] = '0';
                esc[4] = hex[(c >> 4) & 0x0f];
                esc[5] = hex[c & 0x0f];
                esc[6] = '\0';
                pb_append(b, esc);
            } else {
                char one[2];
                one[0] = (char)c;
                one[1] = '\0';
                pb_append(b, one);
            }
            break;
        }
    }
    pb_append(b, "\"");
}

static void probe_build_json(char *dst, size_t cap,
                             const struct probe_lib *libs,
                             const struct probe_export *exports,
                             const char *dir, int all_loaded, int all_jni,
                             int first_idx, const char *first_stage,
                             const char *first_symbol,
                             const char *first_error)
{
    struct probe_buf b;
    unsigned i;
    if (!dst || cap == 0) return;
    dst[0] = '\0';
    b.buf = dst;
    b.cap = cap;
    b.len = 0;
    b.overflow = 0;

    pb_append(&b, "{\"version\":1,\"ran\":true,\"directory\":");
    pb_json_string(&b, dir);
    pb_printf(&b, ",\"all_loaded\":%s,\"all_jni_exports\":%s",
              all_loaded ? "true" : "false", all_jni ? "true" : "false");
    pb_append(&b, ",\"first_failure\":");
    if (first_idx >= 0) {
        pb_printf(&b, "{\"index\":%d,\"stage\":", first_idx);
        pb_json_string(&b, first_stage);
        pb_append(&b, ",\"symbol\":");
        pb_json_string(&b, first_symbol);
        pb_append(&b, ",\"error\":");
        pb_json_string(&b, first_error);
        pb_append(&b, "}");
    } else {
        pb_append(&b, "null");
    }
    pb_append(&b, ",\"libraries\":[");
    for (i = 0; i < PROBE_LIB_COUNT; ++i) {
        if (i) pb_append(&b, ",");
        pb_append(&b, "{\"name\":");
        pb_json_string(&b, libs[i].name);
        pb_append(&b, ",\"path\":");
        pb_json_string(&b, libs[i].path);
        pb_printf(&b, ",\"ok\":%s,\"error\":", libs[i].ok ? "true" : "false");
        pb_json_string(&b, libs[i].error);
        pb_append(&b, ",\"status\":");
        pb_json_string(&b, libs[i].status);
        pb_append(&b, "}");
    }
    pb_append(&b, "],\"jni_exports\":[");
    for (i = 0; i < PROBE_EXPORT_COUNT; ++i) {
        if (i) pb_append(&b, ",");
        pb_append(&b, "{\"name\":");
        pb_json_string(&b, exports[i].name);
        pb_printf(&b, ",\"found\":%s}",
                  exports[i].found ? "true" : "false");
    }
    pb_append(&b, "],\"notes\":[");
    pb_json_string(&b, "JNI 导出只查找，未调用");
    pb_append(&b, ",");
    pb_json_string(&b, "系统库仍由系统 dlopen 加载");
    pb_append(&b, ",");
    pb_json_string(&b, "构造器默认跳过，状态含 init_skipped=1");
    pb_append(&b, ",");
    pb_json_string(&b, "__sF 为不透明标准流槽，须配套 stdio 包装；FILE 字段访问未支持");
    pb_append(&b, ",");
    pb_json_string(&b, "本探针不证明 JNI 调用可用或 getVersion 成功");
    pb_append(&b, "]}");

    if (b.overflow) {
        snprintf(dst, cap, "{\"version\":1,\"ran\":true,\"truncated\":true}");
        dst[cap - 1] = '\0';
    }
}

static void probe_build_invalid_json(char *dst, size_t cap, const char *error)
{
    struct probe_buf b;
    if (!dst || cap == 0) return;
    dst[0] = '\0';
    b.buf = dst;
    b.cap = cap;
    b.len = 0;
    b.overflow = 0;
    pb_append(&b, "{\"version\":1,\"ran\":false,\"error\":");
    pb_json_string(&b, error ? error : "invalid directory");
    pb_append(&b, "}");
}

int elf_loader_probe_run(const char *dir)
{
    struct probe_lib libs[PROBE_LIB_COUNT];
    struct probe_export exports[PROBE_EXPORT_COUNT];
    elf_loader_t *handles[PROBE_LIB_COUNT];
    char use_dir[PATH_MAX];
    const char *src_dir;
    char first_stage[64];
    char first_symbol[256];
    char first_error[PROBE_ERROR_MAX];
    int first_idx = -1;
    int all_loaded = 1;
    int all_jni = 1;
    int invalid = 0;
    unsigned i;
    struct stat st;

    pthread_mutex_lock(&g_probe_mu);
    if (g_running) {
        pthread_mutex_unlock(&g_probe_mu);
        return -1;
    }
    g_running = 1;
    pthread_mutex_unlock(&g_probe_mu);

    memset(libs, 0, sizeof(libs));
    memset(exports, 0, sizeof(exports));
    memset(handles, 0, sizeof(handles));
    use_dir[0] = '\0';
    first_stage[0] = '\0';
    first_symbol[0] = '\0';
    first_error[0] = '\0';

    src_dir = (dir && *dir) ? dir : PROBE_DEFAULT_DIR;
    {
        size_t n = strlen(src_dir);
        while (n > 1 && src_dir[n - 1] == '/') --n;
        if (n >= sizeof(use_dir)) {
            invalid = 1;
        } else {
            memcpy(use_dir, src_dir, n);
            use_dir[n] = '\0';
        }
    }
    if (!invalid) {
        if (stat(use_dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
            invalid = 1;
        }
    }

    if (invalid) {
        char msg[PROBE_ERROR_MAX];
        snprintf(msg, sizeof(msg), "目录不存在或不是目录：%s",
                 use_dir[0] ? use_dir : (src_dir ? src_dir : "(null)"));
        msg[sizeof(msg) - 1] = '\0';
        pthread_mutex_lock(&g_probe_mu);
        probe_copy(g_last_error, sizeof(g_last_error), msg);
        probe_copy(g_dir, sizeof(g_dir), use_dir[0] ? use_dir : "-");
        probe_build_invalid_json(g_json, sizeof(g_json), msg);
        g_running = 0;
        pthread_mutex_unlock(&g_probe_mu);
        printf("ELF_LOADER_PROBE error=invalid_directory dir=%s\n",
               use_dir[0] ? use_dir : "-");
        printf("ELF_LOADER_PROBE_JSON %s\n", g_json);
        fflush(stdout);
        return -1;
    }

    for (i = 0; i < PROBE_LIB_COUNT; ++i) {
        char path[PATH_MAX];
        char err[PROBE_ERROR_MAX];
        libs[i].name = g_lib_names[i];
        libs[i].ok = 0;
        libs[i].error[0] = '\0';
        libs[i].status[0] = '\0';
        err[0] = '\0';
        if (!probe_join_path(path, sizeof(path), use_dir, g_lib_names[i])) {
            probe_copy(libs[i].path, sizeof(libs[i].path), "-");
            probe_copy(libs[i].error, sizeof(libs[i].error), "路径过长");
            all_loaded = 0;
            if (first_idx < 0) {
                first_idx = (int)i;
                probe_copy(first_stage, sizeof(first_stage), "path");
                probe_copy(first_symbol, sizeof(first_symbol), g_lib_names[i]);
                probe_copy(first_error, sizeof(first_error), "joined path too long");
            }
            continue;
        }
        probe_copy(libs[i].path, sizeof(libs[i].path), path);
        handles[i] = elf_loader_open(path, err, sizeof(err));
        if (handles[i]) {
            libs[i].ok = 1;
            probe_copy(libs[i].status, sizeof(libs[i].status),
                         elf_loader_status(handles[i]));
            probe_copy(libs[i].error, sizeof(libs[i].error), "-");
        } else {
            libs[i].ok = 0;
            probe_copy(libs[i].error, sizeof(libs[i].error),
                       err[0] ? err : "unknown");
            probe_copy(libs[i].status, sizeof(libs[i].status), "-");
            all_loaded = 0;
            if (first_idx < 0) {
                first_idx = (int)i;
                probe_extract_stage(libs[i].error, first_stage,
                                    sizeof(first_stage));
                probe_extract_symbol(libs[i].error, first_symbol,
                                     sizeof(first_symbol));
                probe_copy(first_error, sizeof(first_error), libs[i].error);
            }
        }
    }

    /* 只在 libcustom_rules 已加载时查找导出；绝不调用。 */
    for (i = 0; i < PROBE_EXPORT_COUNT; ++i)
        exports[i].name = g_export_names[i];
    if (handles[2]) {
        for (i = 0; i < PROBE_EXPORT_COUNT; ++i) {
            void *p = elf_loader_symbol(handles[2], g_export_names[i]);
            exports[i].found = (p != NULL) ? 1 : 0;
            if (!exports[i].found) all_jni = 0;
        }
    } else {
        all_jni = 0;
    }

    if (first_idx < 0 && !all_jni) {
        first_idx = 2;
        probe_copy(first_stage, sizeof(first_stage), "jni_export");
        probe_copy(first_error, sizeof(first_error),
                   "libcustom_rules Java_ export 缺失或库未加载");
        for (i = 0; i < PROBE_EXPORT_COUNT; ++i) {
            if (!exports[i].found) {
                probe_copy(first_symbol, sizeof(first_symbol),
                           g_export_names[i]);
                break;
            }
        }
    }

    for (i = 0; i < PROBE_LIB_COUNT; ++i) {
        if (libs[i].ok) {
            printf("ELF_LOADER_PROBE lib=%s ok=1 path=%s status=%s\n",
                   libs[i].name, libs[i].path, libs[i].status);
        } else {
            printf("ELF_LOADER_PROBE lib=%s ok=0 path=%s error=%s\n",
                   libs[i].name, libs[i].path, libs[i].error);
        }
    }
    for (i = 0; i < PROBE_EXPORT_COUNT; ++i) {
        printf("ELF_LOADER_PROBE export=%s found=%d\n",
               exports[i].name, exports[i].found ? 1 : 0);
    }
    printf("ELF_LOADER_PROBE summary all_loaded=%d all_jni_exports=%d "
           "first_failure_index=%d first_failure_stage=%s "
           "first_failure_symbol=%s\n",
           all_loaded ? 1 : 0, all_jni ? 1 : 0, first_idx,
           first_stage[0] ? first_stage : "-",
           first_symbol[0] ? first_symbol : "-");
    fflush(stdout);

    pthread_mutex_lock(&g_probe_mu);
    probe_copy(g_dir, sizeof(g_dir), use_dir);
    for (i = 0; i < PROBE_LIB_COUNT; ++i) g_libs[i] = libs[i];
    for (i = 0; i < PROBE_EXPORT_COUNT; ++i) g_exports[i] = exports[i];
    if (first_idx >= 0)
        probe_copy(g_last_error, sizeof(g_last_error), first_error);
    else
        g_last_error[0] = '\0';
    probe_build_json(g_json, sizeof(g_json), libs, exports, use_dir,
                     all_loaded, all_jni, first_idx, first_stage,
                     first_symbol, first_error);
    printf("ELF_LOADER_PROBE_JSON %s\n", g_json);
    fflush(stdout);
    g_running = 0;
    pthread_mutex_unlock(&g_probe_mu);

    /* 释放调用方引用；加载器按契约保留映射到进程结束。 */
    for (i = 0; i < PROBE_LIB_COUNT; ++i) {
        if (handles[i]) (void)elf_loader_close(handles[i]);
    }

    return (all_loaded && all_jni) ? 0 : 1;
}

const char *elf_loader_probe_json(void)
{
    return g_json;
}
