#include "native_asset_bridge.h"
#include "egl_window_runtime.h"
/*
 * 最小自定义 ELF64 加载器（AArch64 / OHOS）。
 *
 * 真实行为边界：
 *   - 只用 mmap/mprotect 映射 PT_LOAD，不用系统 dlopen 加载目标自定义库；
 *   - 自己解析 PT_DYNAMIC、dynsym、RELATIVE/ABS64/GLOB_DAT/JUMP_SLOT；
 *   - 符号版本被忽略（这是绕过系统版本化 LIBC 限制的核心）；
 *   - 构造器默认跳过，状态显式记录 init_skipped=1；
 *   - __sF 只提供占位对象，任何真正使用 stdio 的路径都是 no-go；
 *   - 系统库仍然交给系统 dlopen，TLS 重定位不支持。
 *
 * 所有注释使用简体中文。
 */

#define _GNU_SOURCE 1

#include "elf_loader.h"
#include "android_dlfcn.h"
#include "android_sensor_shim.h"
#include "android_property_shim.h"
#include "android_looper_shim.h"
#include "bionic_ctype_data.h"
#include "window_runtime.h"
#include "media_format.h"
#include "media_extractor.h"
#include "media_codec.h"
#include "android_fdset_shim.h"
#include "android_semaphore_shim.h"
#include "android_signal_shim.h"
#include "android_stdio_shim.h"
#include "jni_abi.h"

/* Implemented in native_window_jni.c (JNI-facing guest Surface bridge). */
extern void *android_native_window_from_surface(JNIEnv *env, jobject surface);
extern jobject android_native_window_to_surface(JNIEnv *env, void *window);

#include <elf.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <locale.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <link.h>
#include <stddef.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#ifndef DT_GNU_HASH
#define DT_GNU_HASH 0x6ffffef5
#endif
#ifndef DF_SYMBOLIC
#define DF_SYMBOLIC 0x00000010
#endif
#ifndef DF_TEXTREL
#define DF_TEXTREL 0x00000004
#endif
#ifndef STT_GNU_IFUNC
#define STT_GNU_IFUNC 10
#endif
#ifndef STB_GNU_UNIQUE
#define STB_GNU_UNIQUE 10
#endif

#define ELF_LOADER_MAX_LOADERS 32
#define ELF_LOADER_MAX_DEPS 32
#define ELF_LOADER_MAX_SYSTEM 16
#define ELF_LOADER_MAX_SEGMENTS 16
#define ELF_LOADER_PATH_MAX 1024
#define ELF_LOADER_NAME_MAX 256
#define ELF_LOADER_ERROR_MAX 512
#define ELF_LOADER_STATUS_MAX 2048
#define ELF_LOADER_SYS_ERROR_MAX 192
#define ELF_LOADER_MAX_FILE_SIZE ((size_t)256 * 1024 * 1024)
#define ELF_LOADER_MAX_MAP_SIZE ((size_t)1024 * 1024 * 1024)

enum loader_state {
    LS_EMPTY = 0,
    LS_LOADING,
    LS_READY,
    LS_FAILED
};

struct load_segment {
    uintptr_t start;
    uintptr_t end;
    int prot;
};

struct system_dep {
    char name[ELF_LOADER_NAME_MAX];
    void *handle;
    int ok;
    char error[ELF_LOADER_SYS_ERROR_MAX];
};

struct elf_loader {
    enum loader_state state;
    unsigned refcount;
    pthread_t loading_thread;

    char path[ELF_LOADER_PATH_MAX];
    char file_name[ELF_LOADER_NAME_MAX];
    char soname[ELF_LOADER_NAME_MAX];

    void *map_base;
    size_t map_size;
    uintptr_t load_bias;
    /* Persistent, validated program headers for guest unwinders. The input
     * file mapping is released after PT_LOAD mapping. */
    Elf64_Phdr phdrs[256];
    Elf64_Half phnum;

    struct load_segment segments[ELF_LOADER_MAX_SEGMENTS];
    unsigned segment_count;

    uintptr_t dynamic_vaddr;
    size_t dynamic_size;

    const Elf64_Sym *dynsym;
    const char *dynstr;
    size_t dynstr_size;
    size_t nsyms;

    const Elf64_Rela *rela;
    size_t rela_count;
    const Elf64_Rela *jmprel;
    size_t jmprel_count;

    uintptr_t init_fn;
    uintptr_t init_array;
    size_t init_array_count;

    uint64_t flags;
    uint64_t flags1;

    struct elf_loader *deps[ELF_LOADER_MAX_DEPS];
    unsigned dep_count;

    struct system_dep sys[ELF_LOADER_MAX_SYSTEM];
    unsigned sys_count;
    unsigned sys_missing;

    unsigned rel_relative;
    unsigned rel_abs64;
    unsigned rel_glob_dat;
    unsigned rel_jump_slot;
    unsigned weak_unresolved;
    char weak_last[ELF_LOADER_NAME_MAX];

    unsigned shim_used;
    char last_shim[ELF_LOADER_NAME_MAX];
    int sf_placeholder_used;

    int init_skipped;
    int init_executed;

    char error[ELF_LOADER_ERROR_MAX];
    char status[ELF_LOADER_STATUS_MAX];
};

struct file_image {
    int fd;
    void *map;
    size_t size;
};

struct dyn_info {
    uintptr_t symtab;
    uintptr_t strtab;
    uintptr_t hash;
    uintptr_t gnu_hash;
    uintptr_t rela;
    uintptr_t jmprel;
    uintptr_t init;
    uintptr_t init_array;

    size_t strsz;
    size_t relasz;
    size_t relaent;
    size_t pltrelsz;
    size_t init_arraysz;
    size_t syment;

    uint64_t pltrel;
    uint64_t flags;
    uint64_t flags1;
    int has_soname;
    uint64_t soname_off;

    uint64_t needed[ELF_LOADER_MAX_DEPS];
    unsigned needed_count;
    int textrel;
};

static struct elf_loader g_loaders[ELF_LOADER_MAX_LOADERS];
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;

static int shim_dladdr(const void *address,Dl_info *info)
{
    if(!info)return 0;
    if(dladdr(address,info))return 1;
    uintptr_t pc=(uintptr_t)address;int found=0;
    pthread_mutex_lock(&g_mu);
    for(unsigned i=0;i<ELF_LOADER_MAX_LOADERS&&!found;++i){
        struct elf_loader *l=&g_loaders[i];
        if(l->state!=LS_READY)continue;
        for(unsigned s=0;s<l->segment_count;++s){
            if(pc<l->segments[s].start||pc>=l->segments[s].end)continue;
            memset(info,0,sizeof *info);info->dli_fname=l->path;info->dli_fbase=l->map_base;
            uintptr_t best=0;
            for(size_t n=0;n<l->nsyms;++n){
                const Elf64_Sym *sym=&l->dynsym[n];unsigned type=ELF64_ST_TYPE(sym->st_info);
                if(sym->st_shndx==SHN_UNDEF||sym->st_shndx==SHN_ABS||
                   (type!=STT_FUNC&&type!=STT_OBJECT&&type!=STT_NOTYPE)||
                   !sym->st_name||sym->st_name>=l->dynstr_size)continue;
                uintptr_t start=l->load_bias+sym->st_value;
                if(start>pc||start<best||(sym->st_size&&pc-start>=sym->st_size))continue;
                const char *name=l->dynstr+sym->st_name;
                if(!memchr(name,0,l->dynstr_size-sym->st_name))continue;
                best=start;info->dli_sname=name;info->dli_saddr=(void *)start;
            }
            found=1;break;
        }
    }
    pthread_mutex_unlock(&g_mu);return found;
}

static int shim_dl_iterate_phdr(int (*callback)(struct dl_phdr_info *,size_t,void *),void *data)
{
    if(!callback)return 0;
    int result=dl_iterate_phdr(callback,data);
    if(result)return result;
    struct elf_loader *ready[ELF_LOADER_MAX_LOADERS];unsigned count=0;
    pthread_mutex_lock(&g_mu);
    for(unsigned i=0;i<ELF_LOADER_MAX_LOADERS;++i)
        if(g_loaders[i].state==LS_READY&&g_loaders[i].phnum)
            ready[count++]=&g_loaders[i];
    pthread_mutex_unlock(&g_mu);
    /* close only drops refcounts: READY mappings and these static slots are
     * retained for process lifetime. Never hold g_mu across foreign code. */
    for(unsigned i=0;i<count;++i){
        struct dl_phdr_info info={0};struct elf_loader *l=ready[i];
        info.dlpi_addr=l->load_bias;info.dlpi_name=l->path;
        info.dlpi_phdr=l->phdrs;info.dlpi_phnum=l->phnum;
        /* Expose only the common AArch64 ABI prefix. Optional host-specific
         * fields (TLS, additions/removals) are not synthesized. */
        size_t prefix=(offsetof(struct dl_phdr_info,dlpi_phnum)+sizeof(info.dlpi_phnum)+7u)&~(size_t)7u;
        result=callback(&info,prefix,data);if(result)return result;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* 基础工具                                                           */
/* ------------------------------------------------------------------ */

static void set_text(char *dst, size_t cap, const char *fmt, ...)
{
    if (!dst || cap == 0) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(dst, cap, fmt, ap);
    va_end(ap);
    dst[cap - 1] = '\0';
}

/* 保留首个失败原因；后续失败不得覆盖它。 */
static void loader_fail(struct elf_loader *l, const char *stage,
                        const char *fmt, ...)
{
    if (!l || l->error[0]) return;
    char msg[ELF_LOADER_ERROR_MAX - 96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    msg[sizeof(msg) - 1] = '\0';
    set_text(l->error, sizeof(l->error), "stage=%s; %s", stage, msg);
}

static size_t page_size(void)
{
    long v = sysconf(_SC_PAGESIZE);
    if (v <= 0) return 4096u;
    return (size_t)v;
}

static int align_down_u64(uint64_t v, uint64_t a, uint64_t *out)
{
    if (!out || a == 0) return 0;
    *out = v & ~(a - 1u);
    return 1;
}

static int align_up_u64(uint64_t v, uint64_t a, uint64_t *out)
{
    uint64_t add;
    if (!out || a == 0) return 0;
    add = a - 1u;
    if (v > UINT64_MAX - add) return 0;
    *out = (v + add) & ~(a - 1u);
    return 1;
}

static int loader_range_ok(const struct elf_loader *l, uintptr_t addr,
                           size_t len)
{
    uintptr_t begin;
    uintptr_t end;
    if (!l || !l->map_base) return 0;
    begin = (uintptr_t)l->map_base;
    if (l->map_size == 0 || begin > UINTPTR_MAX - l->map_size) return 0;
    end = begin + l->map_size;
    if (addr < begin || addr > end) return 0;
    if (len == 0) return addr <= end;
    return len <= end - addr;
}

static void path_parent(const char *path, char *out, size_t cap)
{
    const char *slash;
    size_t n;
    if (!out || cap == 0) return;
    out[0] = '\0';
    if (!path || !*path) {
        set_text(out, cap, ".");
        return;
    }
    slash = strrchr(path, '/');
    if (!slash) {
        set_text(out, cap, ".");
        return;
    }
    n = (size_t)(slash - path);
    if (n == 0) {
        set_text(out, cap, "/");
        return;
    }
    if (n >= cap) n = cap - 1;
    memcpy(out, path, n);
    out[n] = '\0';
}

static void path_basename(const char *path, char *out, size_t cap)
{
    const char *slash;
    if (!out || cap == 0) return;
    if (!path || !*path) {
        out[0] = '\0';
        return;
    }
    slash = strrchr(path, '/');
    set_text(out, cap, "%s", slash ? slash + 1 : path);
}

static int file_is_regular(const char *path)
{
    struct stat st;
    if (!path || stat(path, &st) != 0) return 0;
    return S_ISREG(st.st_mode) ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* 内置 Bionic shim 表                                                */
/* ------------------------------------------------------------------ */

static int *shim_errno(void)
{
    /* OHOS musl 的 errno 宏展开为 __errno_location()；返回同一线程局部槽。 */
    return &errno;
}

/* ------------------------------------------------------------------ */
/* APK zip 路径 shim                                                   */
/*                                                                     */
/* 引擎把 dataPath 设为 base.apk（缺少 !/assets），随后直接 open        */
/* "<apk>/assets/..."，在真实 Android 上这类路径由 AssetManager/zip     */
/* 语义处理。这里补上该真实语义：从 APK 中央目录定位条目，解压/直取后  */
/* 写入匿名 memfd（失败退回已 unlink 的沙箱临时文件），把 fd 交给引擎。  */
/* 数据全部来自真实 APK 字节，不伪造内容。                              */
/* ------------------------------------------------------------------ */

typedef struct {
    void *next_in; unsigned avail_in; unsigned long total_in;
    void *next_out; unsigned avail_out; unsigned long total_out;
    const char *msg; void *state;
    void *zalloc; void *zfree; void *opaque;
    int data_type; unsigned long adler; unsigned long reserved;
} zip_zstream;

typedef int (*zip_inflate_init2_fn)(zip_zstream *, int, const char *, int);
typedef int (*zip_inflate_fn)(zip_zstream *, int);
typedef int (*zip_inflate_end_fn)(zip_zstream *);
typedef const char *(*zip_version_fn)(void);

static unsigned zip_rd16(const unsigned char *p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8);
}
static unsigned long long zip_rd32(const unsigned char *p)
{
    return (unsigned long long)p[0] | ((unsigned long long)p[1] << 8) |
           ((unsigned long long)p[2] << 16) | ((unsigned long long)p[3] << 24);
}
static unsigned long long zip_rd64(const unsigned char *p)
{
    return zip_rd32(p) | (zip_rd32(p + 4) << 32);
}
static int zip_sig(const unsigned char *p, unsigned b0, unsigned b1,
                   unsigned b2, unsigned b3)
{
    return p[0] == b0 && p[1] == b1 && p[2] == b2 && p[3] == b3;
}

static void apk_zip_log(const char *fmt, ...)
{
    static _Atomic unsigned count;
    if (atomic_fetch_add(&count, 1) >= 512) return;
    FILE *f = fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt", "a");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc(10, f);
    fclose(f);
}

static int zip_anon_fd(const char *tag)
{
    /* OHOS permits reading a raw memfd but rejects fstat on it with EACCES.
     * Use a private sandbox file so guest size/seek operations remain usable. */
    (void)tag;
    char tmp[256];
    static _Atomic unsigned seq;
    snprintf(tmp, sizeof tmp,
             "/data/storage/el2/base/haps/entry/files/sdkdex/.apkzip.%d.%u",
             (int)getpid(), (unsigned)atomic_fetch_add(&seq, 1));
    int tfd = open(tmp, O_CREAT | O_RDWR | O_CLOEXEC | O_TRUNC, 0600);
    if (tfd < 0) return -1;
    unlink(tmp);
    return tfd;
}

/* 成功返回 fd；-1 表示路径不匹配；-2 表示匹配但条目读取失败。 */
static int apk_zip_path_open(const char *path)
{
    const char *mark = strstr(path, ".apk!/");
    size_t skip;
    if (mark) {
        skip = 6; /* ".apk!/" */
    } else {
        mark = strstr(path, ".apk/");
        if (!mark) return -1;
        skip = 5; /* ".apk/" */
    }
    char apk[512];
    size_t apk_len = (size_t)(mark - path) + 4;
    if (apk_len >= sizeof apk) return -2;
    memcpy(apk, path, apk_len);
    apk[apk_len] = 0;
    const char *entry = mark + skip;
    size_t entry_len = strlen(entry);
    if (!entry_len) return -2;

    int fd = open(apk, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        apk_zip_log("APK_ZIP_OPEN apk=%s entry=%s result=apk_unreadable errno=%d",
                    apk, entry, errno);
        return -2;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < 22) {
        close(fd);
        apk_zip_log("APK_ZIP_OPEN apk=%s entry=%s result=apk_too_small", apk, entry);
        return -2;
    }
    unsigned long long fsize = (unsigned long long)st.st_size;

    size_t tail = fsize < 65557 ? (size_t)fsize : 65557;
    unsigned char *tailbuf = malloc(tail);
    if (!tailbuf) {
        close(fd);
        apk_zip_log("APK_ZIP_OPEN apk=%s entry=%s result=tail_malloc_failed", apk, entry);
        return -2;
    }
    if (pread(fd, tailbuf, tail, (off_t)(fsize - tail)) != (ssize_t)tail) {
        int saved = errno;
        free(tailbuf); close(fd);
        apk_zip_log("APK_ZIP_OPEN apk=%s entry=%s result=tail_read_failed errno=%d", apk, entry, saved);
        return -2;
    }
    long long eocd = -1;
    for (long long i = (long long)tail - 22; i >= 0; i--) {
        if (zip_sig(tailbuf + i, 0x50, 0x4b, 0x05, 0x06)) { eocd = i; break; }
    }
    if (eocd < 0) {
        free(tailbuf); close(fd);
        apk_zip_log("APK_ZIP_OPEN apk=%s entry=%s result=no_eocd", apk, entry);
        return -2;
    }
    unsigned long long cd_off = zip_rd32(tailbuf + eocd + 16);
    unsigned long long cd_size = zip_rd32(tailbuf + eocd + 12);
    unsigned long long count = zip_rd16(tailbuf + eocd + 10);
    if ((cd_off == 0xFFFFFFFFull || count == 0xFFFFull) && eocd >= 20) {
        const unsigned char *loc = tailbuf + eocd - 20;
        if (zip_sig(loc, 0x50, 0x4b, 0x06, 0x07)) {
            unsigned long long z64off = zip_rd64(loc + 8);
            unsigned char z64[56];
            if (z64off + 56 <= fsize &&
                pread(fd, z64, 56, (off_t)z64off) == 56 &&
                zip_sig(z64, 0x50, 0x4b, 0x06, 0x06)) {
                count = zip_rd64(z64 + 32);
                cd_size = zip_rd64(z64 + 40);
                cd_off = zip_rd64(z64 + 48);
            }
        }
    }
    free(tailbuf);
    if (!cd_size || cd_size > (256ull << 20) || cd_off + cd_size > fsize) {
        close(fd);
        apk_zip_log("APK_ZIP_OPEN apk=%s entry=%s result=bad_cd off=%llu size=%llu",
                    apk, entry, cd_off, cd_size);
        return -2;
    }
    unsigned char *cd = malloc((size_t)cd_size);
    if (!cd) {
        close(fd);
        apk_zip_log("APK_ZIP_OPEN apk=%s entry=%s result=cd_malloc_failed size=%llu", apk, entry, cd_size);
        return -2;
    }
    if (pread(fd, cd, (size_t)cd_size, (off_t)cd_off) != (ssize_t)cd_size) {
        int saved = errno;
        free(cd); close(fd);
        apk_zip_log("APK_ZIP_OPEN apk=%s entry=%s result=cd_read_failed errno=%d off=%llu size=%llu",
                    apk, entry, saved, cd_off, cd_size);
        return -2;
    }

    size_t pos = 0;
    int found = 0;
    unsigned method = 0;
    unsigned long long csize = 0, usize = 0, lho = 0;
    while (pos + 46 <= (size_t)cd_size) {
        const unsigned char *h = cd + pos;
        if (!zip_sig(h, 0x50, 0x4b, 0x01, 0x02)) break;
        unsigned nlen = zip_rd16(h + 28), elen = zip_rd16(h + 30), clen = zip_rd16(h + 32);
        if (pos + 46 + nlen + elen + clen > (size_t)cd_size) break;
        if (nlen == entry_len && !memcmp(h + 46, entry, nlen)) {
            method = zip_rd16(h + 10);
            csize = zip_rd32(h + 20);
            usize = zip_rd32(h + 24);
            lho = zip_rd32(h + 42);
            const unsigned char *ex = h + 46 + nlen;
            size_t exoff = 0;
            while (exoff + 4 <= elen) {
                unsigned id = zip_rd16(ex + exoff), sz = zip_rd16(ex + exoff + 2);
                if (id == 0x0001) {
                    size_t o = exoff + 4;
                    if (usize == 0xFFFFFFFFull && o + 8 <= exoff + 4 + sz) { usize = zip_rd64(ex + o); o += 8; }
                    if (csize == 0xFFFFFFFFull && o + 8 <= exoff + 4 + sz) { csize = zip_rd64(ex + o); o += 8; }
                    if (lho == 0xFFFFFFFFull && o + 8 <= exoff + 4 + sz) { lho = zip_rd64(ex + o); }
                    break;
                }
                exoff += 4 + sz;
            }
            found = 1;
            break;
        }
        pos += 46 + nlen + elen + clen;
    }
    free(cd);
    if (!found) {
        close(fd);
        apk_zip_log("APK_ZIP_OPEN apk=%s entry=%s result=entry_missing entries=%llu",
                    apk, entry, count);
        return -2;
    }

    unsigned char lh[30];
    if (lho + 30 > fsize || pread(fd, lh, 30, (off_t)lho) != 30 ||
        !zip_sig(lh, 0x50, 0x4b, 0x03, 0x04)) {
        close(fd);
        apk_zip_log("APK_ZIP_OPEN apk=%s entry=%s result=bad_local lho=%llu", apk, entry, lho);
        return -2;
    }
    unsigned long long data_off = lho + 30 + zip_rd16(lh + 26) + zip_rd16(lh + 28);
    if (data_off + csize > fsize) {
        close(fd);
        apk_zip_log("APK_ZIP_OPEN apk=%s entry=%s result=bad_range off=%llu csize=%llu",
                    apk, entry, data_off, csize);
        return -2;
    }

    int out = zip_anon_fd(entry);
    if (out < 0) {
        close(fd);
        apk_zip_log("APK_ZIP_OPEN apk=%s entry=%s result=no_anon_fd", apk, entry);
        return -2;
    }

    if (method == 0) {
        unsigned char tmp[65536];
        unsigned long long left = csize, off = data_off;
        while (left) {
            size_t n = left < sizeof tmp ? (size_t)left : sizeof tmp;
            ssize_t r = pread(fd, tmp, n, (off_t)off);
            if (r <= 0) break;
            ssize_t w = write(out, tmp, (size_t)r);
            if (w != r) break;
            off += (unsigned long long)r;
            left -= (unsigned long long)r;
        }
        if (left) {
            close(out); close(fd);
            apk_zip_log("APK_ZIP_OPEN apk=%s entry=%s result=stored_copy_failed", apk, entry);
            return -2;
        }
    } else if (method == 8) {
        if (!usize || usize > (256ull << 20) || !csize) {
            close(out); close(fd);
            apk_zip_log("APK_ZIP_OPEN apk=%s entry=%s result=deflate_size csize=%llu usize=%llu",
                        apk, entry, csize, usize);
            return -2;
        }
        void *libz = dlopen("libz.so", RTLD_NOW | RTLD_LOCAL);
        if (!libz) libz = dlopen("libz.so.1", RTLD_NOW | RTLD_LOCAL);
        zip_inflate_init2_fn zinit = libz ? (zip_inflate_init2_fn)dlsym(libz, "inflateInit2_") : NULL;
        zip_inflate_fn zinflate = libz ? (zip_inflate_fn)dlsym(libz, "inflate") : NULL;
        zip_inflate_end_fn zend = libz ? (zip_inflate_end_fn)dlsym(libz, "inflateEnd") : NULL;
        zip_version_fn zver = libz ? (zip_version_fn)dlsym(libz, "zlibVersion") : NULL;
        unsigned char *comp = malloc((size_t)csize);
        unsigned char *plain = malloc((size_t)usize);
        int ok = 0;
        if (zinit && zinflate && zend && zver && comp && plain &&
            pread(fd, comp, (size_t)csize, (off_t)data_off) == (ssize_t)csize) {
            zip_zstream zs;
            memset(&zs, 0, sizeof zs);
            if (zinit(&zs, -15, zver(), (int)sizeof zs) == 0) {
                zs.next_in = comp;
                zs.avail_in = (unsigned)csize;
                zs.next_out = plain;
                zs.avail_out = (unsigned)usize;
                int r = zinflate(&zs, 4 /* Z_FINISH */);
                if (r == 1 && zs.total_out == usize) {
                    if (write(out, plain, (size_t)usize) == (ssize_t)usize) ok = 1;
                }
                zend(&zs);
            }
        }
        free(comp);
        free(plain);
        if (libz) dlclose(libz);
        if (!ok) {
            close(out); close(fd);
            apk_zip_log("APK_ZIP_OPEN apk=%s entry=%s result=inflate_failed csize=%llu usize=%llu",
                        apk, entry, csize, usize);
            return -2;
        }
    } else {
        close(out); close(fd);
        apk_zip_log("APK_ZIP_OPEN apk=%s entry=%s result=unsupported_method method=%u",
                    apk, entry, method);
        return -2;
    }
    close(fd);
    lseek(out, 0, SEEK_SET);
    errno = 0;
    apk_zip_log("APK_ZIP_OPEN apk=%s entry=%s result=ok method=%u usize=%llu fd=%d",
                apk, entry, method, usize, out);
    return out;
}

/* 自实现前缀判断：不依赖 libc 的 strncmp/strstr（自定义 loader 下 IFUNC/版本
 * 符号可能解析失败，导致比较静默为假）。 */
static int probe_starts_with(const char *s, const char *prefix)
{
    if (!s || !prefix) return 0;
    while (*prefix) { if (*s != *prefix) return 0; ++s; ++prefix; }
    return 1;
}
static const char *probe_find(const char *s, const char *needle)
{
    if (!s || !needle) return 0;
    if (!*needle) return s;
    for (; *s; ++s) {
        const char *a = s, *b = needle;
        while (*a && *b && *a == *b) { ++a; ++b; }
        if (!*b) return s;
    }
    return 0;
}
static bool apk_virtual_path(const char *path)
{
    return path && (strstr(path, ".apk/") || strstr(path, ".apk!/"));
}

static void native_directory_probe(const char *op,const char *path,int result,unsigned mode)
{
    if(!path||!strstr(path,"/external_files"))return;
    int saved=errno;static _Atomic unsigned count;
    if(atomic_fetch_add(&count,1)<256){
        FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt","a");
        if(f){fprintf(f,"NATIVE_DIRECTORY op=%s path=%s result=%d errno=%d mode=0%o thread=%llu\n",op,path,result,result<0?saved:0,mode,(unsigned long long)(uintptr_t)pthread_self());fclose(f);}
    }
    errno=saved;
}
static int shim_guest_mkdir(const char *path,mode_t mode)
{
    int result=mkdir(path,mode);native_directory_probe("mkdir",path,result,(unsigned)mode);return result;
}
static DIR *shim_guest_opendir(const char *path)
{
    DIR *result=opendir(path);native_directory_probe("opendir",path,result?0:-1,0);return result;
}
static int shim_zip_stat(const char *path, struct stat *out)
{
    if (!apk_virtual_path(path)){int result=stat(path,out);native_directory_probe("stat",path,result,result==0?(unsigned)out->st_mode:0);return result;}
    if (!out) { errno = EFAULT; return -1; }
    errno = 0;
    int fd = apk_zip_path_open(path);
    if (fd < 0) {
        if (!errno) errno = ENOENT;
        apk_zip_log("APK_ZIP_STAT path=%s success=0 errno=%d", path, errno);
        return -1;
    }
    int result = fstat(fd, out);
    int saved_errno = errno;
    close(fd);
    apk_zip_log("APK_ZIP_STAT path=%s fd=%d out=%p result=%d errno=%d size=%lld",
                path, fd, (void *)out, result, result < 0 ? saved_errno : 0,
                result == 0 ? (long long)out->st_size : -1LL);
    errno = saved_errno;
    return result;
}

static int shim_zip_lstat(const char *path, struct stat *out)
{
    if(apk_virtual_path(path))return shim_zip_stat(path,out);
    int result=lstat(path,out);native_directory_probe("lstat",path,result,result==0?(unsigned)out->st_mode:0);return result;
}

static int shim_zip_access(const char *path, int mode)
{
    if (!apk_virtual_path(path)){int result=access(path,mode);native_directory_probe("access",path,result,(unsigned)mode);return result;}
    if (mode & ~(R_OK | W_OK | X_OK)) { errno = EINVAL; return -1; }
    struct stat info;
    if (shim_zip_stat(path, &info)) return -1;
    if (mode & (W_OK | X_OK)) { errno = EACCES; return -1; }
    return 0;
}

static int shim_zip_fstat(int fd, struct stat *out)
{
    int result = fstat(fd, out);
    int saved_errno = errno;
    static _Atomic unsigned count;
    if (atomic_fetch_add(&count, 1) < 128)
        apk_zip_log("APK_FSTAT fd=%d out=%p result=%d errno=%d size=%lld",
                    fd, (void *)out, result, result < 0 ? saved_errno : 0,
                    result == 0 ? (long long)out->st_size : -1LL);
    errno = saved_errno;
    return result;
}

static int shim_traced_open(const char *path,int flags,...)
{
    {
        static _Atomic unsigned entry_count;
        if (atomic_fetch_add(&entry_count, 1) < 8) {
            FILE *ef = fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt", "a");
            if (ef) {
                fprintf(ef, "OPEN_SHIM_ENTRY pid=%d path=%s\n", (int)getpid(), path ? path : "(null)");
                fclose(ef);
            }
        }
    }
    if (path && probe_starts_with(path, "/il2cpp")) {
        static _Atomic unsigned icount;
        if (atomic_fetch_add(&icount, 1) < 16) {
            FILE *f = fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt", "a");
            if (f) {
                fprintf(f, "OPEN_IL2CPP pid=%d path=%s caller=0x%llx\n",
                        (int)getpid(), path, (unsigned long long)(uintptr_t)__builtin_return_address(0));
                fclose(f);
            }
        }
    }
    if (path && !(flags & (O_CREAT | O_WRONLY | O_RDWR | O_TRUNC | O_APPEND))) {
        int zfd = apk_zip_path_open(path);
        if (path[0] && strstr(path, ".apk")) {
            static _Atomic unsigned seen;
            if (atomic_fetch_add(&seen, 1) < 16) {
                FILE *sf = fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt", "a");
                if (sf) { fprintf(sf, "OPEN_APK_PATH path=%s flags=0x%x zip_fd=%d\n", path, flags, zfd); fclose(sf); }
            }
        }
        if (zfd >= 0) {
            static _Atomic unsigned zcount;
            if (atomic_fetch_add(&zcount, 1) < 64) {
                FILE *f = fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt", "a");
                if (f) { fprintf(f, "NATIVE_FILE_OPEN path=%s flags=0x%x fd=%d errno=0 via=apk_zip\n", path, flags, zfd); fclose(f); }
            }
            return zfd;
        }
    }
    int needs_mode=(flags&O_CREAT)!=0;
#ifdef O_TMPFILE
    needs_mode=needs_mode||((flags&O_TMPFILE)==O_TMPFILE);
#endif
    mode_t mode=0;
    if(needs_mode){va_list a;va_start(a,flags);mode=(mode_t)va_arg(a,int);va_end(a);}
    int fd=needs_mode?open(path,flags,mode):open(path,flags);
    int saved_errno=errno;static _Atomic unsigned count;
    if(atomic_fetch_add(&count,1)<128){
        FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt","a");
        if(f){fprintf(f,"NATIVE_FILE_OPEN pid=%d path=%s flags=0x%x fd=%d errno=%d\n",(int)getpid(),path?path:"(null)",flags,fd,fd<0?saved_errno:0);fclose(f);}
    }
    errno=saved_errno;return fd;
}
static int shim_open_2(const char *path, int flags)
{
    if (flags & O_CREAT)
        return open(path, flags, 0);
    return open(path, flags);
}

static ssize_t shim_read_chk(int fd, void *buf, size_t count, size_t buflen)
{
    if (count > buflen) {
        errno = EINVAL;
        return -1;
    }
    return read(fd, buf, count);
}

static void *shim_memmove_chk(void *dest, const void *src, size_t len,
                              size_t destlen)
{
    if (len > destlen) abort();
    return memmove(dest, src, len);
}

static char *shim_strcpy_chk(char *dest, const char *src, size_t destlen)
{
    if (strlen(src) >= destlen) abort();
    return strcpy(dest, src);
}

static char *shim_strcat_chk(char *dest, const char *src, size_t destlen)
{
    size_t d = strlen(dest);
    size_t s = strlen(src);
    if (d > destlen || s >= destlen - d) abort();
    return strcat(dest, src);
}

static size_t shim_strlen_chk(const char *s, size_t maxlen)
{
    size_t n = strnlen(s, maxlen);
    if (n == maxlen) abort();
    return n;
}

/* Diagnostic wrapper: engine-created threads are the load/graphics workers.
 * Record the caller and start routine so a missing worker (nativeRender
 * waiting on FUTEX) can be distinguished from a failed pthread_create. */
static int shim_pthread_create(pthread_t *t, const pthread_attr_t *a,
                               void *(*fn)(void *), void *arg)
{
    int rc = pthread_create(t, a, fn, arg);
    static _Atomic unsigned count;
    if (atomic_fetch_add(&count, 1) < 128) {
        void *lr = __builtin_return_address(0);
        FILE *f = fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt", "a");
        if (f) {
            fprintf(f, "PTHREAD_CREATE caller=0x%llx fn=%p arg=%p rc=%d\n",
                    (unsigned long long)(uintptr_t)lr, (void *)fn, arg, rc);
            fclose(f);
        }
    }
    return rc;
}

/* Save the caller's registers before a C prologue can reuse them. */
__attribute__((used, noinline))
static void *trace_pthread_getspecific(pthread_key_t key, uintptr_t caller,
                                       uintptr_t saved_x22, uintptr_t saved_x21,
                                       uintptr_t saved_x19)
{
    void *value = pthread_getspecific(key);
    int saved_errno = errno;
    static _Atomic unsigned count;
    if (atomic_fetch_add_explicit(&count, 1, memory_order_relaxed) < 4096) {
        FILE *log = fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt", "a");
        if (log) {
            fprintf(log, "PTHREAD_GETSPECIFIC tid=%ld caller=%p key=%u result=%p"
                         " x22=%p x21=%p x19=%p\n",
                    (long)syscall(SYS_gettid), (void *)caller, (unsigned)key, value,
                    (void *)saved_x22, (void *)saved_x21, (void *)saved_x19);
            fclose(log);
        }
    }
    errno = saved_errno;
    return value;
}

__attribute__((naked))
static void *shim_pthread_getspecific(pthread_key_t key)
{
    __asm__ volatile(
        "mov x1, x30\n"
        "mov x2, x22\n"
        "mov x3, x21\n"
        "mov x4, x19\n"
        "b trace_pthread_getspecific\n");
}

/* Record condition waits and their caller so we can identify UnityMain's stop. */
static int shim_pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m)
{
    void *lr = __builtin_return_address(0);
    long tid = (long)syscall(SYS_gettid);
    static _Atomic unsigned count;
    if (atomic_fetch_add(&count, 1) < 256) {
        char bt[256] = {0};
        void **fp = __builtin_frame_address(0);
        for (int i = 0; i < 8 && fp; i++) {
            void *flr = fp[1];
            if (!flr) break;
            char tmp[32];
            snprintf(tmp, sizeof tmp, "%s0x%llx", i ? "," : "",
                     (unsigned long long)(uintptr_t)flr);
            if (strlen(bt) + strlen(tmp) < sizeof bt) strcat(bt, tmp);
            uintptr_t next = (uintptr_t)fp[0];
            if (next <= (uintptr_t)fp || next - (uintptr_t)fp > 0x100000) break;
            fp = (void **)next;
        }
        FILE *f = fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt", "a");
        if (f) {
            fprintf(f, "PTHREAD_COND_WAIT entering tid=%ld caller=0x%llx cond=%p bt=%s\n",
                    tid, (unsigned long long)(uintptr_t)lr, (void *)c, bt);
            fclose(f);
        }
    }
    int rc = pthread_cond_wait(c, m);
    return rc;
}
static int shim_pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m,
                                       const struct timespec *ts)
{
    void *lr = __builtin_return_address(0);
    long tid = (long)syscall(SYS_gettid);
    static _Atomic unsigned count;
    if (atomic_fetch_add(&count, 1) < 256) {
        FILE *f = fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt", "a");
        if (f) {
            fprintf(f, "PTHREAD_COND_TIMEDWAIT entering tid=%ld caller=0x%llx\n",
                    tid, (unsigned long long)(uintptr_t)lr);
            fclose(f);
        }
    }
    int rc = pthread_cond_timedwait(c, m, ts);
    return rc;
}
static int shim_pthread_cond_signal(pthread_cond_t *c)
{
    int rc = pthread_cond_signal(c);
    static _Atomic unsigned count;
    if (atomic_fetch_add(&count, 1) < 256) {
        void *lr = __builtin_return_address(0);
        long tid = (long)syscall(SYS_gettid);
        FILE *f = fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt", "a");
        if (f) {
            fprintf(f, "PTHREAD_COND_SIGNAL tid=%ld caller=0x%llx cond=%p rc=%d\n",
                    tid, (unsigned long long)(uintptr_t)lr, (void *)c, rc);
            fclose(f);
        }
    }
    return rc;
}
static int shim_pthread_cond_broadcast(pthread_cond_t *c)
{
    int rc = pthread_cond_broadcast(c);
    static _Atomic unsigned count;
    if (atomic_fetch_add(&count, 1) < 256) {
        void *lr = __builtin_return_address(0);
        long tid = (long)syscall(SYS_gettid);
        FILE *f = fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt", "a");
        if (f) {
            fprintf(f, "PTHREAD_COND_BROADCAST tid=%ld caller=0x%llx cond=%p rc=%d\n",
                    tid, (unsigned long long)(uintptr_t)lr, (void *)c, rc);
            fclose(f);
        }
    }
    return rc;
}

static int shim_vsnprintf_chk(char *dest, size_t supplied_size, int flags,
                              size_t dest_len_from_compiler,
                              const char *format, va_list va)
{
    (void)flags;
    if (supplied_size > dest_len_from_compiler) abort();
    return vsnprintf(dest, supplied_size, format, va);
}

static int shim_vsprintf_chk(char *dest, int flags,
                             size_t dest_len_from_compiler,
                             const char *format, va_list va)
{
    (void)flags;
    /* 不用无界 vsprintf：把编译器给出的对象大小作为硬上限。 */
    return vsnprintf(dest, dest_len_from_compiler, format, va);
}

static long long shim_strtoll_l(const char *nptr, char **endptr, int base,
                                locale_t locale)
{
    (void)locale;
    return strtoll(nptr, endptr, base);
}

static unsigned long long shim_strtoull_l(const char *nptr, char **endptr,
                                          int base, locale_t locale)
{
    (void)locale;
    return strtoull(nptr, endptr, base);
}

static long double shim_strtold_l(const char *nptr, char **endptr,
                                  locale_t locale)
{
    (void)locale;
    return strtold(nptr, endptr);
}

static size_t shim_ctype_get_mb_cur_max(void)
{
#ifdef MB_CUR_MAX
    /* MB_CUR_MAX 调用系统 __ctype_get_mb_cur_max，不是本 shim 自身。 */
    return (size_t)MB_CUR_MAX;
#else
    return 1u;
#endif
}

/*
 * Android 属性服务在 OHOS 上不存在。这里只返回 0（失败），并把 value 清空；
 * 不伪造属性值。若调用方依赖某个属性，必须自行在更高层提供映射。
 */
/* 真机 `param get const.product.*` 实测值：brand/manufacturer=HUAWEI、
 * model=MLR-AL00、name=HUAWEI MatePad Mini 悦读版。HarmonyOS 没有 Android
 * 属性服务，应用进程也无法读取 build.prop；这里把实测设备身份映射为引擎
 * 需要的 Android 属性名，其余属性仍返回失败而不是猜测。 */
static const struct { const char *name; const char *value; } g_host_properties[] = {
    {"ro.product.manufacturer", "HUAWEI"},
    {"ro.product.brand", "HUAWEI"},
    {"ro.product.model", "MLR-AL00"},
    {"ro.product.device", "MLR-AL00"},
    {"ro.product.name", "MLR-AL00"},
    {"ro.product.cpu.abi", "arm64-v8a"},
    {"ro.product.cpu.abilist", "arm64-v8a"},
};
static int shim_system_property_get(const char *name, char *value)
{
    const char *env;
    size_t n = 0;
    int hit = 0;
    if (value) value[0] = '\0';
    if (!name || !*name || !value) return 0;
    env = getenv(name);
    if (!env) {
        for (size_t i = 0; i < sizeof g_host_properties / sizeof g_host_properties[0]; ++i)
            if (!strcmp(name, g_host_properties[i].name)) { env = g_host_properties[i].value; break; }
    }
    if (env) {
        hit = 1;
        n = strlen(env);
        if (n > 91u) n = 91u;
        memcpy(value, env, n);
        value[n] = '\0';
    }
    {
        static _Atomic unsigned prop_count;
        if (atomic_fetch_add(&prop_count, 1) < 32) {
            FILE *f = fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt", "a");
            if (f) { fprintf(f, "PROPERTY_GET name=%s hit=%d len=%d\n", name, hit, (int)n); fclose(f); }
        }
    }
    return (int)n;
}

/* Bionic and musl assign different integers to the same sysconf query. */
#include "android_opensles_ids.inc"

static long shim_android_sysconf(int android_name)
{
    int host_name;
    switch (android_name) {
#include "android_sysconf_map.inc"
    default: errno = EINVAL; return -1;
    }
    long result = sysconf(host_name);
    int result_errno = errno;
    static _Atomic unsigned count;
    if (atomic_fetch_add(&count, 1) < 128) {
        FILE *file = fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt", "a");
        if (file) {
            fprintf(file, "ANDROID_SYSCONF guest=%d host=%d result=%ld errno=%d caller=%p\n",
                    android_name, host_name, result, result_errno, __builtin_return_address(0));
            fclose(file);
        }
    }
    errno = result_errno;
    return result;
}

static void shim_android_set_abort_message(const char *msg)
{
    static const char prefix[] = "android_set_abort_message: ";
    if (!msg) msg = "(null)";
    int saved_errno = errno;
    FILE *file = fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt", "a");
    if (file) {
        fprintf(file, "NATIVE_ABORT_MESSAGE thread=%llu text=%.4096s\n",
                (unsigned long long)(uintptr_t)pthread_self(), msg);
        fclose(file);
    }
    void (*host)(const char *) = (void (*)(const char *))dlsym(RTLD_DEFAULT, "android_set_abort_message");
    if (host && host != shim_android_set_abort_message) {
        host(msg);
        errno = saved_errno;
        return;
    }
    (void)write(STDERR_FILENO, prefix, sizeof(prefix) - 1u);
    (void)write(STDERR_FILENO, msg, strlen(msg));
    (void)write(STDERR_FILENO, "\n", 1);
    errno = saved_errno;
}

extern int sdk_trace_fprintf(FILE *,const char *,...);
static int shim_android_log_print(int prio, const char *tag,
                                  const char *fmt, ...)
{
    char buf[768];
    char msg[512];
    int n;
    va_list ap;
    if (!tag) tag = "android";
    if (!fmt) fmt = "";
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    msg[sizeof(msg) - 1] = '\0';
    if(!strcmp(tag,"Unity")||!strcmp(tag,"IL2CPP")){
        sdk_trace_fprintf(stderr,"ENGINE_LOG priority=%d tag=%s text=%s\n",prio,tag,msg);
        FILE *lf=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt","a");
        if(lf){fprintf(lf,"ENGINE_LOG tid=%llu priority=%d tag=%s text=%s\n",(unsigned long long)(uintptr_t)pthread_self(),prio,tag,msg);fclose(lf);}
    } else {
        static _Atomic unsigned other_log_count;
        if(atomic_fetch_add(&other_log_count,1)<256){
            FILE *lf=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt","a");
            if(lf){fprintf(lf,"ANDROID_LOG tid=%llu priority=%d tag=%s text=%s\n",(unsigned long long)(uintptr_t)pthread_self(),prio,tag,msg);fclose(lf);}
        }
    }
    n = snprintf(buf, sizeof(buf), "[android-log prio=%d tag=%s] %s\n",
                 prio, tag, msg);
    if (n < 0) return -1;
    if ((size_t)n > sizeof(buf)) n = (int)sizeof(buf);
    (void)write(STDERR_FILENO, buf, (size_t)n);
    return n;
}

static int shim_android_log_vprint(int prio,const char *tag,const char *fmt,va_list ap)
{
    char text[4096];if(!fmt)return -1;
    int count=vsnprintf(text,sizeof text,fmt,ap);
    if(count<0)return count;
    return shim_android_log_print(prio,tag,"%s",text);
}

static int shim_android_log_write(int prio, const char *tag,
                                  const char *text)
{
    return shim_android_log_print(prio, tag, "%s", text ? text : "");
}

/* Opaque Android standard-stream slots; only the paired stdio wrappers may
 * consume their addresses. Direct FILE field access remains unsupported. */

#define MEDIA_KEY(name, value) static const char *shim_##name = value;
#include "bionic_media_keys.inc"
#undef MEDIA_KEY

struct shim_entry {
    const char *name;
    void *addr;
    int object;
};

static const struct shim_entry g_shims[] = {
    {"stat", (void *)(uintptr_t)&shim_zip_stat, 0},
    {"fstat", (void *)(uintptr_t)&shim_zip_fstat, 0},
    {"lstat", (void *)(uintptr_t)&shim_zip_lstat, 0},
    {"access", (void *)(uintptr_t)&shim_zip_access, 0},
    {"mkdir", (void *)(uintptr_t)&shim_guest_mkdir, 0},
    {"opendir", (void *)(uintptr_t)&shim_guest_opendir, 0},
    {"open", (void *)(uintptr_t)&shim_traced_open, 0},
    {"pthread_create", (void *)(uintptr_t)&shim_pthread_create, 0},
    {"pthread_getspecific", (void *)(uintptr_t)&shim_pthread_getspecific, 0},
    {"pthread_cond_wait", (void *)(uintptr_t)&shim_pthread_cond_wait, 0},
    {"pthread_cond_timedwait", (void *)(uintptr_t)&shim_pthread_cond_timedwait, 0},
    {"pthread_cond_signal", (void *)(uintptr_t)&shim_pthread_cond_signal, 0},
    {"pthread_cond_broadcast", (void *)(uintptr_t)&shim_pthread_cond_broadcast, 0},
    {"fopen", (void *)(uintptr_t)&android_stdio_fopen, 0},
    {"fclose", (void *)(uintptr_t)&android_stdio_fclose, 0},
    {"fflush", (void *)(uintptr_t)&android_stdio_fflush, 0},
    {"fread", (void *)(uintptr_t)&android_stdio_fread, 0},
    {"fwrite", (void *)(uintptr_t)&android_stdio_fwrite, 0},
    {"fseek", (void *)(uintptr_t)&android_stdio_fseek, 0},
    {"fseeko", (void *)(uintptr_t)&android_stdio_fseeko, 0},
    {"ftello", (void *)(uintptr_t)&android_stdio_ftello, 0},
    {"fputc", (void *)(uintptr_t)&android_stdio_fputc, 0},
    {"fprintf", (void *)(uintptr_t)&android_stdio_fprintf, 0},
    {"vfprintf", (void *)(uintptr_t)&android_stdio_vfprintf, 0},
    {"fdopen", (void *)(uintptr_t)&android_stdio_fdopen, 0},
    {"fscanf", (void *)(uintptr_t)&android_stdio_fscanf, 0},
    {"vfscanf", (void *)(uintptr_t)&android_stdio_vfscanf, 0},
    {"fgets", (void *)(uintptr_t)&android_stdio_fgets, 0},
    {"fputs", (void *)(uintptr_t)&android_stdio_fputs, 0},
    {"feof", (void *)(uintptr_t)&android_stdio_feof, 0},
    {"ferror", (void *)(uintptr_t)&android_stdio_ferror, 0},
    {"clearerr", (void *)(uintptr_t)&android_stdio_clearerr, 0},
    {"fileno", (void *)(uintptr_t)&android_stdio_fileno, 0},
    {"ftell", (void *)(uintptr_t)&android_stdio_ftell, 0},
    {"setbuf", (void *)(uintptr_t)&android_stdio_setbuf, 0},
    {"setvbuf", (void *)(uintptr_t)&android_stdio_setvbuf, 0},
    {"strlen", (void *)(uintptr_t)&android_strlen_guard, 0},
    {"sem_init", (void *)(uintptr_t)&android_sem_init, 0},
    {"sem_destroy", (void *)(uintptr_t)&android_sem_destroy, 0},
    {"sem_post", (void *)(uintptr_t)&android_sem_post, 0},
    {"sem_wait", (void *)(uintptr_t)&android_sem_wait, 0},
    {"sem_trywait", (void *)(uintptr_t)&android_sem_trywait, 0},
    {"sem_timedwait", (void *)(uintptr_t)&android_sem_timedwait, 0},
    {"sem_getvalue", (void *)(uintptr_t)&android_sem_getvalue, 0},
    {"sigemptyset", (void *)(uintptr_t)&android_signal_sigemptyset, 0},
    {"sigaddset", (void *)(uintptr_t)&android_signal_sigaddset, 0},
    {"sigaction", (void *)(uintptr_t)&android_signal_sigaction, 0},
    {"pthread_sigmask", (void *)(uintptr_t)&android_signal_pthread_sigmask, 0},
    {"__FD_SET_chk", (void *)(uintptr_t)&__FD_SET_chk, 0},
    {"__FD_ISSET_chk", (void *)(uintptr_t)&__FD_ISSET_chk, 0},
    {"AMediaCodec_configure", (void *)(uintptr_t)&android_media_codec_configure, 0},
    {"AMediaCodec_createDecoderByType", (void *)(uintptr_t)&android_media_codec_create_decoder_by_type, 0},
    {"AMediaCodec_delete", (void *)(uintptr_t)&android_media_codec_delete, 0},
    {"AMediaCodec_dequeueInputBuffer", (void *)(uintptr_t)&android_media_codec_dequeue_input_buffer, 0},
    {"AMediaCodec_dequeueOutputBuffer", (void *)(uintptr_t)&android_media_codec_dequeue_output_buffer, 0},
    {"AMediaCodec_flush", (void *)(uintptr_t)&android_media_codec_flush, 0},
    {"AMediaCodec_getInputBuffer", (void *)(uintptr_t)&android_media_codec_get_input_buffer, 0},
    {"AMediaCodec_getOutputBuffer", (void *)(uintptr_t)&android_media_codec_get_output_buffer, 0},
    {"AMediaCodec_getOutputFormat", (void *)(uintptr_t)&android_media_codec_get_output_format, 0},
    {"AMediaCodec_queueInputBuffer", (void *)(uintptr_t)&android_media_codec_queue_input_buffer, 0},
    {"AMediaCodec_releaseOutputBuffer", (void *)(uintptr_t)&android_media_codec_release_output_buffer, 0},
    {"AMediaCodec_setOutputSurface", (void *)(uintptr_t)&android_media_codec_set_output_surface, 0},
    {"AMediaCodec_start", (void *)(uintptr_t)&android_media_codec_start, 0},
    {"AMediaCodec_stop", (void *)(uintptr_t)&android_media_codec_stop, 0},
    {"AMediaFormat_delete", (void *)(uintptr_t)&android_media_format_delete, 0},
    {"AMediaFormat_getFloat", (void *)(uintptr_t)&android_media_format_get_float, 0},
    {"AMediaFormat_getInt32", (void *)(uintptr_t)&android_media_format_get_int32, 0},
    {"AMediaFormat_getInt64", (void *)(uintptr_t)&android_media_format_get_int64, 0},
    {"AMediaFormat_getString", (void *)(uintptr_t)&android_media_format_get_string, 0},
    {"AMediaFormat_setInt32", (void *)(uintptr_t)&android_media_format_set_int32, 0},
    {"AMediaExtractor_new", (void *)(uintptr_t)&android_media_extractor_new, 0},
    {"AMediaExtractor_delete", (void *)(uintptr_t)&android_media_extractor_delete, 0},
    {"AMediaExtractor_setDataSourceFd", (void *)(uintptr_t)&android_media_extractor_set_data_source_fd, 0},
    {"AMediaExtractor_setDataSource", (void *)(uintptr_t)&android_media_extractor_set_data_source, 0},
    {"AMediaExtractor_setDataSourceCustom", (void *)(uintptr_t)&android_media_extractor_set_data_source_custom, 0},
    {"AMediaExtractor_getTrackCount", (void *)(uintptr_t)&android_media_extractor_get_track_count, 0},
    {"AMediaExtractor_getTrackFormat", (void *)(uintptr_t)&android_media_extractor_get_track_format, 0},
    {"AMediaExtractor_selectTrack", (void *)(uintptr_t)&android_media_extractor_select_track, 0},
    {"AMediaExtractor_readSampleData", (void *)(uintptr_t)&android_media_extractor_read_sample_data, 0},
    {"AMediaExtractor_getSampleTrackIndex", (void *)(uintptr_t)&android_media_extractor_get_sample_track_index, 0},
    {"AMediaExtractor_getSampleTime", (void *)(uintptr_t)&android_media_extractor_get_sample_time, 0},
    {"AMediaExtractor_advance", (void *)(uintptr_t)&android_media_extractor_advance, 0},
    {"AMediaExtractor_seekTo", (void *)(uintptr_t)&android_media_extractor_seek_to, 0},
    {"ANativeWindow_getWidth", (void *)(uintptr_t)&window_runtime_get_width, 0},
    {"ANativeWindow_getHeight", (void *)(uintptr_t)&window_runtime_get_height, 0},
    {"ANativeWindow_acquire", (void *)(uintptr_t)&window_runtime_acquire, 0},
    {"ANativeWindow_release", (void *)(uintptr_t)&window_runtime_release, 0},
    {"ANativeWindow_setBuffersGeometry", (void *)(uintptr_t)&window_runtime_set_buffers_geometry, 0},
    {"ANativeWindow_fromSurface", (void *)(uintptr_t)&android_native_window_from_surface, 0},
    {"ANativeWindow_toSurface", (void *)(uintptr_t)&android_native_window_to_surface, 0},
    {"ASensorManager_getInstance", (void *)(uintptr_t)&android_sensor_manager_get_instance, 0},
    {"ASensorManager_getInstanceForPackage", (void *)(uintptr_t)&android_sensor_manager_get_instance_for_package, 0},
    {"ASensorManager_getDefaultSensor", (void *)(uintptr_t)&android_sensor_manager_get_default_sensor, 0},
    {"ASensorManager_getSensorList", (void *)(uintptr_t)&android_sensor_manager_get_sensor_list, 0},
    {"ASensorManager_createEventQueue", (void *)(uintptr_t)&android_sensor_manager_create_event_queue, 0},
    {"ASensorManager_destroyEventQueue", (void *)(uintptr_t)&android_sensor_manager_destroy_event_queue, 0},
    {"ASensorEventQueue_enableSensor", (void *)(uintptr_t)&android_sensor_event_queue_enable_sensor, 0},
    {"ASensorEventQueue_disableSensor", (void *)(uintptr_t)&android_sensor_event_queue_disable_sensor, 0},
    {"ASensorEventQueue_setEventRate", (void *)(uintptr_t)&android_sensor_event_queue_set_event_rate, 0},
    {"ASensorEventQueue_hasEvents", (void *)(uintptr_t)&android_sensor_event_queue_has_events, 0},
    {"ASensorEventQueue_getEvents", (void *)(uintptr_t)&android_sensor_event_queue_get_events, 0},
    {"ASensor_getType", (void *)(uintptr_t)&android_sensor_get_type, 0},
    {"ASensor_getName", (void *)(uintptr_t)&android_sensor_get_name, 0},
    {"ASensor_getVendor", (void *)(uintptr_t)&android_sensor_get_vendor, 0},
    {"ASensor_getResolution", (void *)(uintptr_t)&android_sensor_get_resolution, 0},
    {"ASensor_getMinDelay", (void *)(uintptr_t)&android_sensor_get_min_delay, 0},
    {"ALooper_forThread", (void *)(uintptr_t)&android_looper_for_thread, 0},
    {"ALooper_prepare", (void *)(uintptr_t)&android_looper_prepare, 0},
    {"ALooper_acquire", (void *)(uintptr_t)&android_looper_acquire, 0},
    {"ALooper_release", (void *)(uintptr_t)&android_looper_release, 0},
    {"ALooper_wake", (void *)(uintptr_t)&android_looper_wake, 0},
    {"ALooper_pollOnce", (void *)(uintptr_t)&android_looper_poll_once, 0},
    {"ALooper_pollAll", (void *)(uintptr_t)&android_looper_poll_all, 0},
#define MEDIA_KEY(name, value) {#name, (void *)&shim_##name, 1},
#include "bionic_media_keys.inc"
#undef MEDIA_KEY
    /* ELF STT_OBJECT resolves the pointer variable, not the table itself. */
    {"_ctype_", (void *)&shim_bionic_ctype_pointer, 1},
    {"__errno", (void *)(uintptr_t)&shim_errno, 0},
    {"sysconf", (void *)(uintptr_t)&shim_android_sysconf, 0},
    {"SL_IID_ANDROIDCONFIGURATION", (void *)&shim_SL_IID_ANDROIDCONFIGURATION, 1},
    {"SL_IID_ANDROIDSIMPLEBUFFERQUEUE", (void *)&shim_SL_IID_ANDROIDSIMPLEBUFFERQUEUE, 1},
    {"SL_IID_ANDROIDACOUSTICECHOCANCELLATION", (void *)&shim_SL_IID_ANDROIDACOUSTICECHOCANCELLATION, 1},
    {"SL_IID_ANDROIDAUTOMATICGAINCONTROL", (void *)&shim_SL_IID_ANDROIDAUTOMATICGAINCONTROL, 1},
    {"SL_IID_ANDROIDNOISESUPPRESSION", (void *)&shim_SL_IID_ANDROIDNOISESUPPRESSION, 1},
    {"__open_2", (void *)(uintptr_t)&shim_open_2, 0},
    {"__read_chk", (void *)(uintptr_t)&shim_read_chk, 0},
    {"__memmove_chk", (void *)(uintptr_t)&shim_memmove_chk, 0},
    {"__strcpy_chk", (void *)(uintptr_t)&shim_strcpy_chk, 0},
    {"__strcat_chk", (void *)(uintptr_t)&shim_strcat_chk, 0},
    {"__strlen_chk", (void *)(uintptr_t)&shim_strlen_chk, 0},
    {"__vsnprintf_chk", (void *)(uintptr_t)&shim_vsnprintf_chk, 0},
    {"__vsprintf_chk", (void *)(uintptr_t)&shim_vsprintf_chk, 0},
    {"strtoll_l", (void *)(uintptr_t)&shim_strtoll_l, 0},
    {"strtoull_l", (void *)(uintptr_t)&shim_strtoull_l, 0},
    {"strtold_l", (void *)(uintptr_t)&shim_strtold_l, 0},
    {"__system_property_get", (void *)(uintptr_t)&shim_system_property_get, 0},
    {"__system_property_find", (void *)(uintptr_t)&android_system_property_find, 0},
    {"__system_property_read", (void *)(uintptr_t)&android_system_property_read, 0},
    {"android_set_abort_message",
     (void *)(uintptr_t)&shim_android_set_abort_message, 0},
    {"__ctype_get_mb_cur_max",
     (void *)(uintptr_t)&shim_ctype_get_mb_cur_max, 0},
    {"__android_log_print", (void *)(uintptr_t)&shim_android_log_print, 0},
    {"__android_log_write", (void *)(uintptr_t)&shim_android_log_write, 0},
    {"__android_log_vprint", (void *)(uintptr_t)&shim_android_log_vprint, 0},
    {"__sF", (void *)(uintptr_t)&android_stdio_sF[0][0], 1},
};

static void *shim_lookup(const char *name, int *is_object)
{
    size_t i;
    if (is_object) *is_object = 0;
    if (!name) return NULL;
    if(!strcmp(name,"dladdr"))return (void *)(uintptr_t)&shim_dladdr;
    if(!strcmp(name,"dl_iterate_phdr"))return (void *)(uintptr_t)&shim_dl_iterate_phdr;
    for (i = 0; i < sizeof(g_shims) / sizeof(g_shims[0]); ++i) {
        if (strcmp(g_shims[i].name, name) == 0) {
            if (is_object) *is_object = g_shims[i].object;
            return g_shims[i].addr;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* 文件映射与 ELF 头校验                                              */
/* ------------------------------------------------------------------ */

void *elf_loader_android_abi_symbol(const char *name)
{
    if(!name)return NULL;
    if(!strcmp(name,"dladdr"))return shim_lookup(name,NULL);
    if(!strcmp(name,"dl_iterate_phdr"))return shim_lookup(name,NULL);
    if(!strcmp(name,"sysconf"))return shim_lookup(name,NULL);
    if(!strcmp(name,"mkdir")||!strcmp(name,"opendir"))return shim_lookup(name,NULL);
    if(!strcmp(name,"open")||!strcmp(name,"stat")||!strcmp(name,"lstat")||!strcmp(name,"access"))return shim_lookup(name,NULL);
    if(!strcmp(name,"pthread_create")||!strcmp(name,"pthread_getspecific"))return shim_lookup(name,NULL);
    if(!strcmp(name,"pthread_cond_wait")||!strcmp(name,"pthread_cond_timedwait")||!strcmp(name,"pthread_cond_signal")||!strcmp(name,"pthread_cond_broadcast"))return shim_lookup(name,NULL);
    if(!strcmp(name,"__android_log_print")||!strcmp(name,"__android_log_write")||!strcmp(name,"__android_log_vprint")||!strcmp(name,"android_set_abort_message"))return shim_lookup(name,NULL);
    void *egl=egl_window_runtime_symbol(name);if(egl)return egl;
    void *asset=native_asset_bridge_symbol(name);if(asset)return asset;
    /* Every token-FILE* stdio entry point must resolve to the paired shim.
     * The old 'f'-prefix test missed setbuf/setvbuf (s-prefixed), so a shim
     * token from fopen reached musl setvbuf and crashed with SIGSEGV. */
    if(!strncmp(name,"sem_",4)||name[0]=='f'||
       !strcmp(name,"setbuf")||!strcmp(name,"setvbuf")||
       !strcmp(name,"strlen")||
       !strcmp(name,"vfprintf")||!strcmp(name,"vfscanf")||
       !strcmp(name,"sigemptyset")||!strcmp(name,"sigaddset")||
       !strcmp(name,"sigaction")||!strcmp(name,"pthread_sigmask"))
        return shim_lookup(name,NULL);
    return NULL;
}

static int map_file(const char *path, struct file_image *fi,
                    char *err, size_t errlen)
{
    struct stat st;
    void *map;
    int fd;
    if (!fi) return 0;
    memset(fi, 0, sizeof(*fi));
    fi->fd = -1;
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        set_text(err, errlen, "打开文件失败：%s（errno=%d）", path, errno);
        return 0;
    }
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) {
        set_text(err, errlen, "文件不是普通非空文件：%s", path);
        close(fd);
        return 0;
    }
    if ((uint64_t)st.st_size > (uint64_t)ELF_LOADER_MAX_FILE_SIZE) {
        set_text(err, errlen, "文件超过加载上限 %zu 字节：%s",
                 ELF_LOADER_MAX_FILE_SIZE, path);
        close(fd);
        return 0;
    }
    map = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED) {
        set_text(err, errlen, "mmap 文件失败：%s（errno=%d）", path, errno);
        close(fd);
        return 0;
    }
    fi->fd = fd;
    fi->map = map;
    fi->size = (size_t)st.st_size;
    return 1;
}

static void unmap_file(struct file_image *fi)
{
    if (!fi) return;
    if (fi->map && fi->map != MAP_FAILED) munmap(fi->map, fi->size);
    if (fi->fd >= 0) close(fi->fd);
    fi->map = NULL;
    fi->fd = -1;
    fi->size = 0;
}

static int validate_elf_header(struct elf_loader *l, const struct file_image *fi,
                               const Elf64_Ehdr **out)
{
    const Elf64_Ehdr *eh;
    uint64_t ph_end;
    if (!l || !fi || !out || fi->size < sizeof(Elf64_Ehdr)) {
        loader_fail(l, "validate", "文件小于 ELF64 头");
        return 0;
    }
    eh = (const Elf64_Ehdr *)fi->map;
    if (memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0) {
        loader_fail(l, "validate", "不是 ELF 文件");
        return 0;
    }
    if (eh->e_ident[EI_CLASS] != ELFCLASS64) {
        loader_fail(l, "validate", "不是 ELF64（class=%u）",
                    (unsigned)eh->e_ident[EI_CLASS]);
        return 0;
    }
    if (eh->e_ident[EI_DATA] != ELFDATA2LSB) {
        loader_fail(l, "validate", "不是小端 ELF（data=%u）",
                    (unsigned)eh->e_ident[EI_DATA]);
        return 0;
    }
    if (eh->e_ident[EI_VERSION] != EV_CURRENT ||
        eh->e_version != EV_CURRENT) {
        loader_fail(l, "validate", "ELF 版本不是 current");
        return 0;
    }
    if (eh->e_type != ET_DYN) {
        loader_fail(l, "validate", "e_type=%u，只接受 ET_DYN",
                    (unsigned)eh->e_type);
        return 0;
    }
    if (eh->e_machine != EM_AARCH64) {
        loader_fail(l, "validate", "e_machine=%u，只接受 AArch64",
                    (unsigned)eh->e_machine);
        return 0;
    }
    if (eh->e_ehsize < sizeof(Elf64_Ehdr) ||
        eh->e_phentsize != sizeof(Elf64_Phdr) ||
        eh->e_phnum == 0 || eh->e_phnum > 256) {
        loader_fail(l, "validate", "程序头尺寸/数量非法（entsize=%u num=%u）",
                    (unsigned)eh->e_phentsize, (unsigned)eh->e_phnum);
        return 0;
    }
    if (eh->e_phoff > fi->size ||
        (uint64_t)eh->e_phnum * sizeof(Elf64_Phdr) > fi->size - eh->e_phoff) {
        loader_fail(l, "validate", "程序头超出文件范围");
        return 0;
    }
    ph_end = (uint64_t)eh->e_phoff +
             (uint64_t)eh->e_phnum * sizeof(Elf64_Phdr);
    if (ph_end > fi->size) {
        loader_fail(l, "validate", "程序头结束地址溢出");
        return 0;
    }
    *out = eh;
    return 1;
}

static int map_segments(struct elf_loader *l, const struct file_image *fi,
                        const Elf64_Ehdr *eh)
{
    const Elf64_Phdr *ph;
    uint64_t page = (uint64_t)page_size();
    uint64_t min_vaddr = UINT64_MAX;
    uint64_t max_vaddr = 0;
    uint64_t map_size;
    uint64_t i;
    void *map;

    if (!l || !fi || !eh || page == 0) return 0;
    ph = (const Elf64_Phdr *)((const unsigned char *)fi->map + eh->e_phoff);

    for (i = 0; i < eh->e_phnum; ++i) {
        if (ph[i].p_type != PT_LOAD) continue;
        if (ph[i].p_memsz == 0) continue;
        if (ph[i].p_filesz > ph[i].p_memsz) {
            loader_fail(l, "map", "PT_LOAD[%llu] p_filesz > p_memsz",
                        (unsigned long long)i);
            return 0;
        }
        if (ph[i].p_offset > fi->size ||
            ph[i].p_filesz > fi->size - ph[i].p_offset) {
            loader_fail(l, "map", "PT_LOAD[%llu] 文件范围越界",
                        (unsigned long long)i);
            return 0;
        }
        if (ph[i].p_vaddr > UINT64_MAX - ph[i].p_memsz) {
            loader_fail(l, "map", "PT_LOAD[%llu] vaddr+memsz 溢出",
                        (unsigned long long)i);
            return 0;
        }
        if (ph[i].p_vaddr < min_vaddr) min_vaddr = ph[i].p_vaddr;
        if (ph[i].p_vaddr + ph[i].p_memsz > max_vaddr)
            max_vaddr = ph[i].p_vaddr + ph[i].p_memsz;
    }
    if (min_vaddr == UINT64_MAX || max_vaddr <= min_vaddr) {
        loader_fail(l, "map", "没有有效 PT_LOAD");
        return 0;
    }
    if (!align_down_u64(min_vaddr, page, &min_vaddr) ||
        !align_up_u64(max_vaddr, page, &max_vaddr)) {
        loader_fail(l, "map", "PT_LOAD 页对齐溢出");
        return 0;
    }
    if (max_vaddr <= min_vaddr || max_vaddr - min_vaddr > ELF_LOADER_MAX_MAP_SIZE) {
        loader_fail(l, "map", "PT_LOAD 映射跨度非法");
        return 0;
    }
    map_size = max_vaddr - min_vaddr;
    map = mmap(NULL, (size_t)map_size, PROT_NONE,
               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (map == MAP_FAILED) {
        loader_fail(l, "map", "保留 PROT_NONE 映射失败（size=%llu errno=%d）",
                    (unsigned long long)map_size, errno);
        return 0;
    }
    l->map_base = map;
    l->map_size = (size_t)map_size;
    l->load_bias = (uintptr_t)map - (uintptr_t)min_vaddr;
    l->segment_count = 0;

    /* OHOS rejects adding PROT_EXEC with mprotect on an anonymous reservation
     * (observed EINVAL at the first executable page).  Build each PT_LOAD as
     * a file-backed mapping with its final permissions instead; the file is
     * still the original .so, no bytes are patched.  BSS pages are anonymous
     * RW and are never executable. */
    for (i = 0; i < eh->e_phnum; ++i) {
        uint64_t seg_start_v, seg_end_v, seg_span;
        uint64_t file_off, file_end_v, file_span;
        uint64_t bss_begin, bss_end;
        uintptr_t seg_addr;
        int prot = 0;

        if (ph[i].p_type != PT_LOAD || ph[i].p_memsz == 0) continue;
        if (ph[i].p_flags & PF_R) prot |= PROT_READ;
        if (ph[i].p_flags & PF_W) prot |= PROT_WRITE;
        if (ph[i].p_flags & PF_X) prot |= PROT_EXEC;
        if ((prot & PROT_WRITE) && (prot & PROT_EXEC)) {
            loader_fail(l, "map", "PT_LOAD[%llu] 同时要求 W/X（W^X）",
                        (unsigned long long)i);
            return 0;
        }
        if (!align_down_u64(ph[i].p_vaddr, page, &seg_start_v) ||
            !align_up_u64(ph[i].p_vaddr + ph[i].p_memsz, page, &seg_end_v) ||
            seg_end_v <= seg_start_v) {
            loader_fail(l, "map", "PT_LOAD[%llu] 段页对齐失败",
                        (unsigned long long)i);
            return 0;
        }
        seg_span = seg_end_v - seg_start_v;
        seg_addr = (uintptr_t)map + (uintptr_t)(seg_start_v - min_vaddr);

        file_off = ph[i].p_offset & ~(page - 1);
        if (!align_up_u64(ph[i].p_offset + ph[i].p_filesz, page, &file_end_v) ||
            file_end_v < file_off) {
            loader_fail(l, "map", "PT_LOAD[%llu] 文件页对齐失败",
                        (unsigned long long)i);
            return 0;
        }
        file_span = file_end_v - file_off;
        if (file_span > seg_span) file_span = seg_span;
        if (file_span) {
            void *got = mmap((void *)seg_addr, (size_t)file_span, prot,
                             MAP_FIXED | MAP_PRIVATE, fi->fd, (off_t)file_off);
            if (got == MAP_FAILED) {
                loader_fail(l, "map", "文件段映射失败（seg=%llu off=%llu len=%llu prot=%d errno=%d）",
                            (unsigned long long)i, (unsigned long long)file_off,
                            (unsigned long long)file_span, prot, errno);
                return 0;
            }
        }

        /* BSS begins at the first page after the file-backed bytes.  A pure
         * BSS PT_LOAD starts at its (possibly unaligned) virtual address. */
        bss_begin = ph[i].p_filesz
                        ? ((ph[i].p_vaddr + ph[i].p_filesz + page - 1) & ~(page - 1))
                        : (ph[i].p_vaddr & ~(page - 1));
        if (!align_up_u64(ph[i].p_vaddr + ph[i].p_memsz, page, &bss_end)) {
            loader_fail(l, "map", "PT_LOAD[%llu] BSS 页对齐失败",
                        (unsigned long long)i);
            return 0;
        }
        if (bss_end > bss_begin) {
            void *got = mmap((void *)((uintptr_t)map + (uintptr_t)(bss_begin - min_vaddr)),
                             (size_t)(bss_end - bss_begin), PROT_READ | PROT_WRITE,
                             MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (got == MAP_FAILED) {
                loader_fail(l, "map", "BSS 映射失败（seg=%llu len=%llu errno=%d）",
                            (unsigned long long)i,
                            (unsigned long long)(bss_end - bss_begin), errno);
                return 0;
            }
        }

        /* Zero the tail of the last file page when it contains BSS bytes.  A
         * writable data segment can be cleared in place; a read-only/exec
         * page with a non-page-aligned BSS tail is refused rather than
         * executed with stale bytes. */
        if (ph[i].p_memsz > ph[i].p_filesz) {
            uint64_t tail = (ph[i].p_vaddr + ph[i].p_filesz) & (page - 1);
            if (tail) {
                uint64_t n = page - tail;
                if (n > ph[i].p_memsz - ph[i].p_filesz)
                    n = ph[i].p_memsz - ph[i].p_filesz;
                if (prot & PROT_WRITE) {
                    memset((unsigned char *)map +
                               (size_t)(ph[i].p_vaddr - min_vaddr + ph[i].p_filesz),
                           0, (size_t)n);
                } else {
                    loader_fail(l, "map", "PT_LOAD[%llu] 只读段含未对齐 BSS 尾部",
                                (unsigned long long)i);
                    return 0;
                }
            }
        }

        if (l->segment_count >= ELF_LOADER_MAX_SEGMENTS) {
            loader_fail(l, "map", "PT_LOAD 数量超过内部上限");
            return 0;
        }
        l->segments[l->segment_count].start = (uintptr_t)(l->load_bias + ph[i].p_vaddr);
        l->segments[l->segment_count].end =
            l->segments[l->segment_count].start + (uintptr_t)ph[i].p_memsz;
        l->segments[l->segment_count].prot = prot;
        ++l->segment_count;
    }

    for (i = 0; i < eh->e_phnum; ++i) {
        if (ph[i].p_type == PT_DYNAMIC) {
            l->dynamic_vaddr = (uintptr_t)ph[i].p_vaddr;
            l->dynamic_size = (size_t)ph[i].p_filesz;
            break;
        }
    }
    if (l->dynamic_vaddr == 0 || l->dynamic_size < sizeof(Elf64_Dyn)) {
        loader_fail(l, "map", "缺少有效 PT_DYNAMIC");
        return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* 动态段解析与符号表计数                                             */
/* ------------------------------------------------------------------ */

static const char *dynstr_name(const struct elf_loader *l, uint64_t off)
{
    const char *s;
    if (!l || !l->dynstr || off >= l->dynstr_size) return NULL;
    s = l->dynstr + (size_t)off;
    if (!memchr(s, '\0', l->dynstr_size - (size_t)off)) return NULL;
    return s;
}

static int parse_dynamic(struct elf_loader *l, struct dyn_info *di)
{
    const Elf64_Dyn *dyn;
    size_t count;
    size_t i;
    uintptr_t addr;
    if (!l || !di) return 0;
    memset(di, 0, sizeof(*di));
    di->relaent = sizeof(Elf64_Rela);
    di->syment = sizeof(Elf64_Sym);
    addr = l->load_bias + l->dynamic_vaddr;
    if (!loader_range_ok(l, addr, l->dynamic_size)) {
        loader_fail(l, "dynamic", "PT_DYNAMIC 不在映射范围内");
        return 0;
    }
    dyn = (const Elf64_Dyn *)addr;
    count = l->dynamic_size / sizeof(Elf64_Dyn);
    for (i = 0; i < count; ++i) {
        uint64_t tag = dyn[i].d_tag;
        uint64_t val = dyn[i].d_un.d_val;
        if (tag == DT_NULL) break;
        switch (tag) {
        case DT_NEEDED:
            if (di->needed_count >= ELF_LOADER_MAX_DEPS) {
                loader_fail(l, "dynamic", "DT_NEEDED 数量超过内部上限");
                return 0;
            }
            di->needed[di->needed_count++] = val;
            break;
        case DT_SONAME:
            di->has_soname = 1;
            di->soname_off = val;
            break;
        case DT_SYMTAB: di->symtab = (uintptr_t)val; break;
        case DT_STRTAB: di->strtab = (uintptr_t)val; break;
        case DT_HASH: di->hash = (uintptr_t)val; break;
        case DT_GNU_HASH: di->gnu_hash = (uintptr_t)val; break;
        case DT_RELA: di->rela = (uintptr_t)val; break;
        case DT_JMPREL: di->jmprel = (uintptr_t)val; break;
        case DT_INIT: di->init = (uintptr_t)val; break;
        case DT_INIT_ARRAY: di->init_array = (uintptr_t)val; break;
        case DT_STRSZ: di->strsz = (size_t)val; break;
        case DT_RELASZ: di->relasz = (size_t)val; break;
        case DT_RELAENT: di->relaent = (size_t)val; break;
        case DT_PLTRELSZ: di->pltrelsz = (size_t)val; break;
        case DT_PLTREL: di->pltrel = val; break;
        case DT_INIT_ARRAYSZ: di->init_arraysz = (size_t)val; break;
        case DT_SYMENT: di->syment = (size_t)val; break;
        case DT_FLAGS: di->flags = val; break;
        case DT_FLAGS_1: di->flags1 = val; break;
        case DT_TEXTREL:
            di->textrel = 1;
            break;
        default:
            break;
        }
    }
    if (di->syment != sizeof(Elf64_Sym)) {
        loader_fail(l, "dynamic", "DT_SYMENT=%zu，期望 %zu",
                    di->syment, sizeof(Elf64_Sym));
        return 0;
    }
    if (di->relaent != sizeof(Elf64_Rela)) {
        loader_fail(l, "dynamic", "DT_RELAENT=%zu，期望 %zu",
                    di->relaent, sizeof(Elf64_Rela));
        return 0;
    }
    if (!di->symtab || !di->strtab || di->strsz == 0) {
        loader_fail(l, "dynamic", "缺少 DT_SYMTAB/DT_STRTAB/DT_STRSZ");
        return 0;
    }
    l->dynsym = (const Elf64_Sym *)(l->load_bias + di->symtab);
    l->dynstr = (const char *)(l->load_bias + di->strtab);
    l->dynstr_size = di->strsz;
    if (!loader_range_ok(l, (uintptr_t)l->dynsym, sizeof(Elf64_Sym)) ||
        !loader_range_ok(l, (uintptr_t)l->dynstr, l->dynstr_size)) {
        loader_fail(l, "dynamic", "dynsym/dynstr 超出映射范围");
        return 0;
    }
    if (di->rela && di->relasz) {
        if (di->relasz % sizeof(Elf64_Rela) != 0) {
            loader_fail(l, "dynamic", "DT_RELASZ 不是 RELA 大小整数倍");
            return 0;
        }
        l->rela = (const Elf64_Rela *)(l->load_bias + di->rela);
        l->rela_count = di->relasz / sizeof(Elf64_Rela);
        if (!loader_range_ok(l, (uintptr_t)l->rela,
                             (size_t)l->rela_count * sizeof(Elf64_Rela))) {
            loader_fail(l, "dynamic", "DT_RELA 超出映射范围");
            return 0;
        }
    } else if (di->rela || di->relasz) {
        loader_fail(l, "dynamic", "DT_RELA/DT_RELASZ 不成对");
        return 0;
    }
    if (di->jmprel && di->pltrelsz) {
        if (di->pltrel != DT_RELA) {
            loader_fail(l, "dynamic", "DT_PLTREL=%llu，只接受 RELA",
                        (unsigned long long)di->pltrel);
            return 0;
        }
        if (di->pltrelsz % sizeof(Elf64_Rela) != 0) {
            loader_fail(l, "dynamic", "DT_PLTRELSZ 不是 RELA 大小整数倍");
            return 0;
        }
        l->jmprel = (const Elf64_Rela *)(l->load_bias + di->jmprel);
        l->jmprel_count = di->pltrelsz / sizeof(Elf64_Rela);
        if (!loader_range_ok(l, (uintptr_t)l->jmprel,
                             (size_t)l->jmprel_count * sizeof(Elf64_Rela))) {
            loader_fail(l, "dynamic", "DT_JMPREL 超出映射范围");
            return 0;
        }
    } else if (di->jmprel || di->pltrelsz) {
        loader_fail(l, "dynamic", "DT_JMPREL/DT_PLTRELSZ 不成对");
        return 0;
    }
    if (di->init_array && di->init_arraysz) {
        if (di->init_arraysz % sizeof(uintptr_t) != 0) {
            loader_fail(l, "dynamic", "DT_INIT_ARRAYSZ 不是指针大小整数倍");
            return 0;
        }
        l->init_array = l->load_bias + di->init_array;
        l->init_array_count = di->init_arraysz / sizeof(uintptr_t);
        if (!loader_range_ok(l, l->init_array,
                             (size_t)l->init_array_count * sizeof(uintptr_t))) {
            loader_fail(l, "dynamic", "DT_INIT_ARRAY 超出映射范围");
            return 0;
        }
    } else if (di->init_array || di->init_arraysz) {
        loader_fail(l, "dynamic", "DT_INIT_ARRAY/DT_INIT_ARRAYSZ 不成对");
        return 0;
    }
    if (di->init) l->init_fn = l->load_bias + di->init;
    l->flags = di->flags;
    l->flags1 = di->flags1;
    return 1;
}

static int derive_symbol_count(struct elf_loader *l, const struct dyn_info *di)
{
    uintptr_t symaddr;
    uintptr_t straddr;
    size_t upper = 0;
    size_t nsyms = 0;
    if (!l || !di) return 0;
    symaddr = (uintptr_t)l->dynsym;
    straddr = (uintptr_t)l->dynstr;
    if (straddr > symaddr)
        upper = (size_t)((straddr - symaddr) / sizeof(Elf64_Sym));

    if (di->hash) {
        const uint32_t *hash = (const uint32_t *)(l->load_bias + di->hash);
        if (!loader_range_ok(l, (uintptr_t)hash, 2 * sizeof(uint32_t))) {
            loader_fail(l, "symbols", "DT_HASH 头超出映射范围");
            return 0;
        }
        nsyms = (size_t)hash[1]; /* nchain 即 dynsym 数量 */
        if (nsyms == 0 || nsyms > 4000000u) {
            loader_fail(l, "symbols", "DT_HASH nchain 非法（%zu）", nsyms);
            return 0;
        }
        if (!loader_range_ok(l, symaddr, nsyms * sizeof(Elf64_Sym))) {
            loader_fail(l, "symbols", "DT_HASH 推导的 dynsym 超出映射范围");
            return 0;
        }
        l->nsyms = nsyms;
        return 1;
    }

    if (di->gnu_hash) {
        const unsigned char *base = (const unsigned char *)(l->load_bias + di->gnu_hash);
        const uint32_t *h;
        uint32_t nbuckets;
        uint32_t symoffset;
        uint32_t bloom_size;
        const uint32_t *buckets;
        const uint32_t *chains;
        uintptr_t map_end;
        size_t max_chain;
        size_t i;
        size_t max_sym = 0;
        if (!loader_range_ok(l, (uintptr_t)base, 4 * sizeof(uint32_t))) {
            loader_fail(l, "symbols", "DT_GNU_HASH 头超出映射范围");
            return 0;
        }
        h = (const uint32_t *)base;
        nbuckets = h[0];
        symoffset = h[1];
        bloom_size = h[2];
        if (nbuckets == 0 || nbuckets > (1u << 20) ||
            bloom_size > (1u << 20) || upper == 0) {
            loader_fail(l, "symbols", "DT_GNU_HASH 参数非法或无法推导符号数");
            return 0;
        }
        {
            uint64_t bucket_off = 16u + (uint64_t)bloom_size * 8u;
            uint64_t need = bucket_off + (uint64_t)nbuckets * 4u;
            if (!loader_range_ok(l, (uintptr_t)base, (size_t)need)) {
                loader_fail(l, "symbols", "DT_GNU_HASH buckets 超出映射范围");
                return 0;
            }
            buckets = (const uint32_t *)(base + bucket_off);
            chains = buckets + nbuckets;
        }
        map_end = (uintptr_t)l->map_base + l->map_size;
        max_chain = (map_end > (uintptr_t)chains)
                        ? (size_t)((map_end - (uintptr_t)chains) /
                                   sizeof(uint32_t))
                        : 0;
        if (max_chain == 0) {
            loader_fail(l, "symbols", "DT_GNU_HASH 链数组为空");
            return 0;
        }
        for (i = 0; i < nbuckets; ++i) {
            uint32_t b = buckets[i];
            size_t idx;
            size_t j;
            int terminated = 0;
            if (b == 0) continue;
            if (b < symoffset) {
                loader_fail(l, "symbols", "DT_GNU_HASH bucket=%u < symoffset=%u",
                            b, symoffset);
                return 0;
            }
            idx = (size_t)(b - symoffset);
            if (idx >= max_chain) {
                loader_fail(l, "symbols", "DT_GNU_HASH bucket 链索引越界");
                return 0;
            }
            for (j = 0; j < upper; ++j) {
                uint32_t v;
                if (idx + j >= max_chain) break;
                v = chains[idx + j];
                if (v & 1u) {
                    size_t last = (size_t)b + j + 1u;
                    if (last > max_sym) max_sym = last;
                    terminated = 1;
                    break;
                }
            }
            if (!terminated) {
                loader_fail(l, "symbols", "DT_GNU_HASH bucket 链未终止");
                return 0;
            }
        }
        if (max_sym == 0) {
            loader_fail(l, "symbols", "DT_GNU_HASH 无 bucket 链，无法得到符号数");
            return 0;
        }
        if (!loader_range_ok(l, symaddr, max_sym * sizeof(Elf64_Sym))) {
            loader_fail(l, "symbols", "DT_GNU_HASH 推导的 dynsym 超出映射范围");
            return 0;
        }
        l->nsyms = max_sym;
        return 1;
    }

    /* 没有 hash 时不能把 dynstr-dynsym 的差值当成符号数，否则会越界扫描。 */
    loader_fail(l, "symbols", "缺少 DT_HASH/DT_GNU_HASH，无法可靠确定 dynsym 数量");
    return 0;
}

/* ------------------------------------------------------------------ */
/* 符号查找                                                           */
/* ------------------------------------------------------------------ */

/*
 * 返回值：0 未找到；1 找到；2 找到同名但类型不受支持（TLS/IFUNC）。
 * 只匹配已定义符号；版本信息完全忽略。
 */
static int find_symbol_in_loader(const struct elf_loader *l, const char *name,
                                 uintptr_t *out)
{
    size_t i;
    if (!l || !name || !out || !l->dynsym || !l->dynstr) return 0;
    for (i = 0; i < l->nsyms; ++i) {
        const Elf64_Sym *sym = &l->dynsym[i];
        const char *s;
        unsigned type;
        uintptr_t addr;
        if (sym->st_name >= l->dynstr_size) continue;
        s = l->dynstr + sym->st_name;
        if (!memchr(s, '\0', l->dynstr_size - sym->st_name)) continue;
        if (strcmp(s, name) != 0) continue;
        if (sym->st_shndx == SHN_UNDEF ||
            sym->st_shndx == SHN_COMMON) continue;
        type = ELF64_ST_TYPE(sym->st_info);
        if (type == STT_SECTION || type == STT_FILE) continue;
        if (type == STT_TLS || type == STT_GNU_IFUNC) return 2;
        if (sym->st_shndx == SHN_ABS)
            addr = (uintptr_t)sym->st_value;
        else
            addr = l->load_bias + (uintptr_t)sym->st_value;
        *out = addr;
        return 1;
    }
    return 0;
}

typedef void (*cxa_throw_fn)(void *, void *, void (*)(void *));
struct il2cpp_throw_probe {
    struct elf_loader *owner;
    cxa_throw_fn original;
    void *wrapper_type;
    void *(*object_class)(void *);
    const char *(*class_name)(void *);
    const char *(*class_namespace)(void *);
    void *(*class_field)(void *, const char *);
    void (*field_value)(void *, void *, void *);
    int32_t (*string_length)(void *);
    const uint16_t *(*string_chars)(void *);
    void (*format_trace)(void *,char *,int);
    void (*walk_frames)(void (*)(const void *,void *),void *);
    const char *(*method_name)(void *);
    void *(*method_class)(void *);
    void *(*thread_current)(void);
};
static struct il2cpp_throw_probe throw_probe;
struct il2cpp_frame_log {FILE *file;unsigned count;};
static void observe_managed_frame(const void *frame,void *data)
{
    struct il2cpp_frame_log *log=data;unsigned index=log->count++;
    if(!frame||index>=64)return;
    /* Original APK frame builder 0x27da1b4 stores MethodInfo* at entry +0;
     * the exported walker passes that actual entry to the callback. */
    void *method=NULL;memcpy(&method,frame,sizeof method);if(!method)return;
    void *klass=throw_probe.method_class(method);if(!klass)return;
    const char *name=throw_probe.method_name(method),*type=throw_probe.class_name(klass),*space=throw_probe.class_namespace(klass);
    fprintf(log->file,"IL2CPP_CURRENT_FRAME index=%u class=%s%s%s method=%s\n",index,space?space:"",space&&*space?".":"",type?type:"?",name?name:"?");
}
struct module_visibility {uintptr_t base;unsigned count;bool found;};
static int observe_host_module(struct dl_phdr_info *info,size_t size,void *data)
{
    (void)size;struct module_visibility *check=data;++check->count;
    if(info->dlpi_addr==check->base)check->found=true;return 0;
}

static void observe_il2cpp_throw(void *object, void *type, void *caller)
{
    if (type != throw_probe.wrapper_type || !object) return;
    void *exception = NULL;
    memcpy(&exception, object, sizeof exception);
    int saved_errno = errno;
    FILE *file = fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt", "a");
    if (!file) { errno = saved_errno; return; }
    fprintf(file, "IL2CPP_THROW caller=%p base=%p object=%p thread=%llu\n",
            caller, (void *)throw_probe.owner->load_bias, exception,
            (unsigned long long)(uintptr_t)pthread_self());
    for (unsigned depth = 0; exception && depth < 4; ++depth) {
        void *klass = throw_probe.object_class(exception);
        if (!klass) break;
        const char *name = throw_probe.class_name(klass);
        const char *space = throw_probe.class_namespace(klass);
        void *field = throw_probe.class_field(klass, "_message");
        void *message = NULL;
        if (field) throw_probe.field_value(exception, field, &message);
        fprintf(file, "IL2CPP_EXCEPTION depth=%u class=%s.%s message_utf16=",
                depth, space ? space : "?", name ? name : "?");
        if (message) {
            int32_t length = throw_probe.string_length(message);
            const uint16_t *units = throw_probe.string_chars(message);
            int32_t count = length > 512 ? 512 : length;
            for (int32_t i = 0; units && i < count; ++i) fprintf(file, "%04x", units[i]);
            fprintf(file, " length=%d", length);
        }
        fputc('\n', file);
        if(depth==0){char trace[4096]={0};throw_probe.format_trace(exception,trace,sizeof trace-1);trace[sizeof trace-1]=0;
            fprintf(file,"IL2CPP_MANAGED_TRACE_BEGIN\n%s\nIL2CPP_MANAGED_TRACE_END\n",trace[0]?trace:"<unavailable>");
            struct module_visibility check={.base=throw_probe.owner->load_bias};dl_iterate_phdr(observe_host_module,&check);
            fprintf(file,"IL2CPP_HOST_MODULE_VISIBILITY found=%d host_modules=%u\n",check.found,check.count);
            struct module_visibility guest={.base=throw_probe.owner->load_bias};
            shim_dl_iterate_phdr(observe_host_module,&guest);Dl_info own={0};
            int located=shim_dladdr((void *)throw_probe.original,&own);
            fprintf(file,"IL2CPP_GUEST_MODULE_VISIBILITY found=%d modules=%u dladdr=%d base=%p path=%s\n",guest.found,guest.count,located,own.dli_fbase,own.dli_fname?own.dli_fname:"?");
            void *thread=throw_probe.thread_current();struct il2cpp_frame_log frames={.file=file};
            if(thread)throw_probe.walk_frames(observe_managed_frame,&frames);
            fprintf(file,"IL2CPP_CURRENT_FRAMES attached=%d count=%u truncated=%d\n",thread!=NULL,frames.count,frames.count>64);
        }
        field = throw_probe.class_field(klass, "_innerException");
        void *inner = NULL;
        if (field) throw_probe.field_value(exception, field, &inner);
        if (inner == exception) break;
        exception = inner;
    }
    fclose(file);
    errno = saved_errno;
}

static void bridge_il2cpp_throw(void *object, void *type, void (*destroy)(void *))
{
    observe_il2cpp_throw(object, type, __builtin_return_address(0));
    __attribute__((musttail)) return throw_probe.original(object, type, destroy);
}

static bool install_il2cpp_throw_probe(struct elf_loader *loader)
{
    if (throw_probe.owner) return throw_probe.owner == loader;
    struct il2cpp_throw_probe probe = {0};
    uintptr_t address = 0;
    if (find_symbol_in_loader(loader, "__cxa_throw", &address) != 1 ||
        address != loader->load_bias + 0x289eb94) return false;
    probe.original = (cxa_throw_fn)address;
    /* Verified in this APK's Exception::Raise: an 8-byte wrapper stores the
     * managed exception at +0 and uses RTTI at libil2cpp+0x607ed88. */
    probe.wrapper_type = (void *)(loader->load_bias + 0x607ed88);
#define LOAD_THROW_API(field, symbol) \
    do { \
        if (find_symbol_in_loader(loader, symbol, &address) != 1) return false; \
        memcpy(&probe.field, &address, sizeof(probe.field)); \
    } while (0)
    LOAD_THROW_API(object_class, "il2cpp_object_get_class");
    LOAD_THROW_API(class_name, "il2cpp_class_get_name");
    LOAD_THROW_API(class_namespace, "il2cpp_class_get_namespace");
    LOAD_THROW_API(class_field, "il2cpp_class_get_field_from_name");
    LOAD_THROW_API(field_value, "il2cpp_field_get_value");
    LOAD_THROW_API(string_length, "il2cpp_string_length");
    LOAD_THROW_API(string_chars, "il2cpp_string_chars");
    LOAD_THROW_API(format_trace, "il2cpp_format_stack_trace");
    LOAD_THROW_API(walk_frames, "il2cpp_current_thread_walk_frame_stack");
    LOAD_THROW_API(method_name, "il2cpp_method_get_name");
    LOAD_THROW_API(method_class, "il2cpp_method_get_class");
    LOAD_THROW_API(thread_current, "il2cpp_thread_current");
#undef LOAD_THROW_API
    probe.owner = loader;
    throw_probe = probe;
    return true;
}

static int dlsym_try(void *handle, const char *name, uintptr_t *out)
{
    const char *e;
    void *p;
    if (!name || !out) return 0; /* RTLD_DEFAULT is legitimately NULL. */
    dlerror();
    p = dlsym(handle, name);
    e = dlerror();
    if (e) return 0;
    *out = (uintptr_t)p;
    return 1;
}

/*
 * 按需求顺序查找：已加载自定义库 dynsym -> 系统句柄/RTLD_DEFAULT -> shim。
 * 若同名符号只以 TLS/IFUNC 形式存在，*unsupported 置 1，由重定位层报 no-go。
 */
static int find_scoped(struct elf_loader *,const char *,uintptr_t *,struct elf_loader **,unsigned *);
static int resolve_symbol_name(struct elf_loader *requester, const char *name,
                               uintptr_t *out, int *unsupported)
{
    unsigned i;
    if (!requester || !name || !*name || !out) return 0;
    *out = 0;
    if (unsupported) *unsupported = 0;
    /* These four imports form one handle ABI. Binding even one to libc while
     * returning custom handles from another would corrupt the system linker. */
    if (!strcmp(name,"dlopen")) { *out=(uintptr_t)&elf_android_dlopen; return 1; }
    if (!strcmp(name,"dlsym")) { *out=(uintptr_t)&elf_android_dlsym; return 1; }
    if (!strcmp(name,"dlclose")) { *out=(uintptr_t)&elf_android_dlclose; return 1; }
    if (!strcmp(name,"dlerror")) { *out=(uintptr_t)&elf_android_dlerror; return 1; }
    /* Pair the object with the forced stdio family; bypass legacy __sF. */
    if(!strcmp(name,"__sF")){
        *out=(uintptr_t)shim_lookup(name,NULL);
        requester->shim_used++;set_text(requester->last_shim,sizeof(requester->last_shim),"%s",name);return 1;
    }
    if(!strncmp(name,"AAsset",6)&&!native_asset_bridge_symbol(name)){if(unsupported)*unsupported=1;return 0;}
    void *abi=elf_loader_android_abi_symbol(name);
    if(abi){*out=(uintptr_t)abi;requester->shim_used++;set_text(requester->last_shim,sizeof(requester->last_shim),"%s",name);return 1;}

    /* A LOCAL library must not borrow an unrelated LOCAL object's exports.
     * All dependency maps are immutable and retained after publication. */
    struct elf_loader *seen[ELF_LOADER_MAX_LOADERS];unsigned seen_count=0;
    int scoped=find_scoped(requester,name,out,seen,&seen_count);
    if(scoped==1)return 1;
    if(scoped==2){if(unsupported)*unsupported=1;return 0;}

    for (i = 0; i < requester->sys_count; ++i) {
        if (requester->sys[i].ok &&
            dlsym_try(requester->sys[i].handle, name, out)) {
            return 1;
        }
    }
    if (dlsym_try(RTLD_DEFAULT, name, out)) return 1;

    {
        int object = 0;
        void *p = shim_lookup(name, &object);
        if (p) {
            *out = (uintptr_t)p;
            requester->shim_used++;
            set_text(requester->last_shim, sizeof(requester->last_shim), "%s",
                     name);
            if (strcmp(name, "__sF") == 0)
                requester->sf_placeholder_used = 1;
            (void)object;
            return 1;
        }
    }
    return 0;
}

static int resolve_reloc_symbol(struct elf_loader *l, uint32_t symidx,
                                uintptr_t *out, char *namebuf, size_t namecap)
{
    const Elf64_Sym *sym;
    const char *name;
    unsigned type;
    unsigned bind;
    int unsupported = 0;

    if (!l || !out) return 0;
    *out = 0;
    if (namebuf && namecap) namebuf[0] = '\0';
    if (symidx == 0) return 1;
    if (symidx >= l->nsyms) {
        loader_fail(l, "relocation", "符号索引 %u 超出 dynsym 数量 %zu",
                    symidx, l->nsyms);
        return 0;
    }
    sym = &l->dynsym[symidx];
    if (sym->st_name >= l->dynstr_size) {
        loader_fail(l, "relocation", "符号索引 %u 的 st_name 越界", symidx);
        return 0;
    }
    name = l->dynstr + sym->st_name;
    if (!memchr(name, '\0', l->dynstr_size - sym->st_name)) {
        loader_fail(l, "relocation", "符号索引 %u 的名字未终止", symidx);
        return 0;
    }
    if (namebuf && namecap) set_text(namebuf, namecap, "%s", name);
    type = ELF64_ST_TYPE(sym->st_info);
    bind = ELF64_ST_BIND(sym->st_info);
    if (type == STT_TLS) {
        loader_fail(l, "relocation", "不支持 TLS 符号：%s", name);
        return 0;
    }
    if (type == STT_GNU_IFUNC) {
        loader_fail(l, "relocation", "不支持 GNU IFUNC 符号：%s", name);
        return 0;
    }
    if (!strcmp(name, "__cxa_throw") && !strcmp(l->file_name, "libil2cpp.so") &&
        install_il2cpp_throw_probe(l)) {
        *out = (uintptr_t)&bridge_il2cpp_throw;
        return 1;
    }
    if (sym->st_shndx != SHN_UNDEF && sym->st_shndx != SHN_COMMON) {
        if (sym->st_shndx == SHN_ABS)
            *out = (uintptr_t)sym->st_value;
        else
            *out = l->load_bias + (uintptr_t)sym->st_value;
        return 1;
    }
    if (sym->st_shndx == SHN_COMMON) {
        loader_fail(l, "relocation", "不支持 SHN_COMMON 符号：%s", name);
        return 0;
    }
    if (resolve_symbol_name(l, name, out, &unsupported)) {
        if (unsupported) {
            loader_fail(l, "relocation", "符号 %s 仅以 TLS/IFUNC 形式存在", name);
            return 0;
        }
        return 1;
    }
    if (bind == STB_WEAK) {
        /* ELF ABI：未解析的弱未定义符号绑定为 0，并记录，不当作成功加载的假象。 */
        *out = 0;
        l->weak_unresolved++;
        set_text(l->weak_last, sizeof(l->weak_last), "%s", name);
        return 1;
    }
    loader_fail(l, "relocation", "未解析符号：%s（绑定=%u 类型=%u）",
                name, bind, type);
    return 0;
}

static int target_is_writable(const struct elf_loader *l, uintptr_t addr,
                              size_t len)
{
    unsigned i;
    if (!l) return 0;
    if (addr > UINTPTR_MAX - len) return 0;
    for (i = 0; i < l->segment_count; ++i) {
        uintptr_t a = l->segments[i].start;
        uintptr_t b = l->segments[i].end;
        if (addr >= a && addr + len <= b && (l->segments[i].prot & PROT_WRITE))
            return 1;
    }
    return 0;
}

static int apply_reloc_array(struct elf_loader *l, const Elf64_Rela *arr,
                             size_t count, const char *which)
{
    size_t i;
    if (!l) return 0;
    if (count == 0) return 1;
    if (!arr) {
        loader_fail(l, "relocation", "%s 指针为空但数量非零", which);
        return 0;
    }
    for (i = 0; i < count; ++i) {
        const Elf64_Rela *r = &arr[i];
        uint64_t type = ELF64_R_TYPE(r->r_info);
        uint32_t symidx = (uint32_t)ELF64_R_SYM(r->r_info);
        uintptr_t target;
        uintptr_t value = 0;
        char symname[ELF_LOADER_NAME_MAX];

        if (r->r_offset > UINTPTR_MAX - l->load_bias) {
            loader_fail(l, "relocation", "%s[%zu] r_offset 溢出", which, i);
            return 0;
        }
        target = l->load_bias + (uintptr_t)r->r_offset;
        if (!loader_range_ok(l, target, sizeof(uint64_t))) {
            loader_fail(l, "relocation", "%s[%zu] 目标不在映射范围", which, i);
            return 0;
        }
        if (!target_is_writable(l, target, sizeof(uint64_t))) {
            loader_fail(l, "relocation",
                        "%s[%zu] 目标不可写（可能为 textrel/RELRO）", which, i);
            return 0;
        }
        symname[0] = '\0';

        switch (type) {
        case R_AARCH64_RELATIVE:
            value = (uintptr_t)((uint64_t)l->load_bias +
                                (uint64_t)(int64_t)r->r_addend);
            l->rel_relative++;
            break;
        case R_AARCH64_ABS64:
            if (!resolve_reloc_symbol(l, symidx, &value, symname,
                                      sizeof(symname)))
                return 0;
            value = (uintptr_t)((uint64_t)value +
                                (uint64_t)(int64_t)r->r_addend);
            l->rel_abs64++;
            break;
        case R_AARCH64_GLOB_DAT:
            if (!resolve_reloc_symbol(l, symidx, &value, symname,
                                      sizeof(symname)))
                return 0;
            l->rel_glob_dat++;
            break;
        case R_AARCH64_JUMP_SLOT:
            if (!resolve_reloc_symbol(l, symidx, &value, symname,
                                      sizeof(symname)))
                return 0;
            l->rel_jump_slot++;
            break;
        default:
            loader_fail(l, "relocation",
                        "%s[%zu] 不支持重定位类型 %llu（符号=%s）",
                        which, i, (unsigned long long)type,
                        symname[0] ? symname : "-");
            return 0;
        }
        memcpy((void *)target, &value, sizeof(value));
    }
    return 1;
}

static int apply_relocations(struct elf_loader *l)
{
    if (!l) return 0;
    if (l->flags & DF_TEXTREL) {
        loader_fail(l, "relocation", "库声明 DF_TEXTREL，本文加载器不支持");
        return 0;
    }
    if (!apply_reloc_array(l, l->rela, l->rela_count, "DT_RELA"))
        return 0;
    if (!apply_reloc_array(l, l->jmprel, l->jmprel_count, "DT_JMPREL"))
        return 0;
    return 1;
}

/* ------------------------------------------------------------------ */
/* 依赖加载                                                           */
/* ------------------------------------------------------------------ */

static int is_forced_system_name(const char *name)
{
    static const char *const names[] = {
        "libc.so", "libm.so", "libdl.so", "liblog.so",
        "libpthread.so", "librt.so", NULL
    };
    size_t i;
    if (!name) return 0;
    for (i = 0; names[i]; ++i)
        if (strcmp(name, names[i]) == 0) return 1;
    return 0;
}

/*
 * 返回 1 表示应使用本加载器加载，并把候选路径写入 out。
 * 返回 0 表示交给系统 dlopen。
 */
static int dependency_path(const struct elf_loader *l, const char *name,
                           char *out, size_t outcap)
{
    char parent[ELF_LOADER_PATH_MAX];
    char candidate[PATH_MAX];
    if (!l || !name || !*name || !out || outcap == 0) return 0;
    if (is_forced_system_name(name)) return 0;
    if (strchr(name, '/')) {
        if (name[0] == '/') {
            if (!file_is_regular(name)) return 0;
            set_text(out, outcap, "%s", name);
            return 1;
        }
        path_parent(l->path, parent, sizeof(parent));
        if (snprintf(candidate, sizeof(candidate), "%s/%s", parent, name) >=
            (int)sizeof(candidate))
            return 0;
        if (!file_is_regular(candidate)) return 0;
        set_text(out, outcap, "%s", candidate);
        return 1;
    }
    path_parent(l->path, parent, sizeof(parent));
    if (snprintf(candidate, sizeof(candidate), "%s/%s", parent, name) >=
        (int)sizeof(candidate))
        return 0;
    if (!file_is_regular(candidate)) return 0;
    set_text(out, outcap, "%s", candidate);
    return 1;
}

static void load_system_dependency(struct elf_loader *l, const char *name)
{
    void *handle;
    char first_error[ELF_LOADER_SYS_ERROR_MAX];
    char second_error[ELF_LOADER_SYS_ERROR_MAX];
    struct system_dep *dep;
    if (!l || !name) return;
    if (l->sys_count >= ELF_LOADER_MAX_SYSTEM) {
        loader_fail(l, "dependency", "系统依赖数量超过内部上限：%s", name);
        return;
    }
    first_error[0] = '\0';
    second_error[0] = '\0';
    dep = &l->sys[l->sys_count++];
    memset(dep, 0, sizeof(*dep));
    set_text(dep->name, sizeof(dep->name), "%s", name);

    dlerror();
    handle = dlopen(name, RTLD_NOW | RTLD_GLOBAL);
    if (!handle) {
        const char *e = dlerror();
        set_text(first_error, sizeof(first_error), "%s", e ? e : "unknown");
        dlerror();
        handle = dlopen(name, RTLD_LAZY | RTLD_GLOBAL);
        if (!handle) {
            const char *e2 = dlerror();
            set_text(second_error, sizeof(second_error), "%s",
                     e2 ? e2 : "unknown");
        }
    }
    if (handle) {
        dep->handle = handle;
        dep->ok = 1;
        set_text(dep->error, sizeof(dep->error), "-");
        return;
    }
    dep->ok = 0;
    l->sys_missing++;
    set_text(dep->error, sizeof(dep->error), "NOW: %s；LAZY: %s",
             first_error[0] ? first_error : "unknown",
             second_error[0] ? second_error : "unknown");
}

static int load_dependencies(struct elf_loader *l, const struct dyn_info *di)
{
    unsigned i;
    if (!l || !di) return 0;
    for (i = 0; i < di->needed_count; ++i) {
        const char *name = dynstr_name(l, di->needed[i]);
        char dep_path[PATH_MAX];
        if (!name || !*name) {
            loader_fail(l, "dependency", "DT_NEEDED[%u] 字符串无效", i);
            return 0;
        }
        if (dependency_path(l, name, dep_path, sizeof(dep_path))) {
            char dep_err[ELF_LOADER_ERROR_MAX];
            elf_loader_t *dep;
            dep_err[0] = '\0';
            dep = elf_loader_open(dep_path, dep_err, sizeof(dep_err));
            if (!dep) {
                loader_fail(l, "dependency", "加载依赖 %s 失败：%s", name,
                            dep_err[0] ? dep_err : "unknown");
                return 0;
            }
            if (l->dep_count >= ELF_LOADER_MAX_DEPS) {
                loader_fail(l, "dependency", "自定义依赖数量超过内部上限");
                return 0;
            }
            l->deps[l->dep_count++] = dep;
        } else {
            load_system_dependency(l, name);
            if (l->error[0]) return 0;
        }
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* 构造器                                                             */
/* ------------------------------------------------------------------ */

static _Thread_local unsigned g_require_initializers;
static _Thread_local elf_loader_init_observer g_init_observer;
static _Thread_local void *g_init_observer_data;

static int init_target_executable(const struct elf_loader *l, uintptr_t p)
{
    for (unsigned i = 0; i < l->segment_count; ++i)
        if ((l->segments[i].prot & PROT_EXEC) && p >= l->segments[i].start &&
            p < l->segments[i].end && (p & 3u) == 0) return 1;
    return 0;
}
static int invoke_initializer(struct elf_loader *l, size_t index, uintptr_t p)
{
    if (g_init_observer && g_init_observer(l->path,index,p,1,g_init_observer_data)) {
        loader_fail(l,"initialization","initializer observer failed before call"); return 0;
    }
    ((void (*)(void))p)();
    l->init_executed = 1;
    if (g_init_observer && g_init_observer(l->path,index,p,0,g_init_observer_data)) {
        loader_fail(l,"initialization","initializer observer failed after call"); return 0;
    }
    return 1;
}
static int should_skip_init(void)
{
    if (g_require_initializers) return 0;
    const char *skip = getenv("ELF_LOADER_SKIP_INIT");
    const char *run = getenv("ELF_LOADER_RUN_INIT");
    if (skip && (strcmp(skip, "0") == 0 || strcmp(skip, "no") == 0))
        return 0;
    if (skip && (strcmp(skip, "1") == 0 || strcmp(skip, "yes") == 0))
        return 1;
    if (run && (strcmp(run, "1") == 0 || strcmp(run, "yes") == 0))
        return 0;
    /* 需求要求默认跳过；环境变量只能显式放开。 */
    return 1;
}

static int run_initializers(struct elf_loader *l)
{
    size_t i;
    if (!l) return 0;
    l->init_skipped = should_skip_init();
    {
        static _Atomic unsigned init_log;
        if (atomic_fetch_add(&init_log, 1) < 64) {
            FILE *f = fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt", "a");
            if (f) {
                fprintf(f, "ELF_INIT path=%s skipped=%d count=%zu\n",
                        l->path ? l->path : "(null)", l->init_skipped, l->init_array_count);
                fclose(f);
            }
        }
    }
    if (l->init_skipped) {
        l->init_executed = 0;
        return 1;
    }
    /* Validate every target before invoking any constructor in this object. */
    const uintptr_t *arr = (const uintptr_t *)l->init_array;
    if (l->init_fn && !init_target_executable(l,l->init_fn)) {
        loader_fail(l,"initialization","DT_INIT outside executable segment"); return 0;
    }
    for (i=0; i<l->init_array_count; ++i) {
        uintptr_t p; memcpy(&p,&arr[i],sizeof(p));
        if (p && p != UINTPTR_MAX && !init_target_executable(l,p)) {
            loader_fail(l,"initialization","DT_INIT_ARRAY[%zu] outside executable segment",i); return 0;
        }
    }
    if (l->init_fn && !invoke_initializer(l,SIZE_MAX,l->init_fn)) return 0;
    for (i=0; i<l->init_array_count; ++i) {
        uintptr_t p; memcpy(&p,&arr[i],sizeof(p));
        if (p && p != UINTPTR_MAX && !invoke_initializer(l,i,p)) return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* 状态文本                                                           */
/* ------------------------------------------------------------------ */

static const char *state_name(enum loader_state s)
{
    switch (s) {
    case LS_LOADING: return "loading";
    case LS_READY: return "ready";
    case LS_FAILED: return "failed";
    case LS_EMPTY:
    default: return "empty";
    }
}

static void build_status_locked(struct elf_loader *l)
{
    size_t used;
    unsigned i;
    if (!l) return;
    used = (size_t)snprintf(
        l->status, sizeof(l->status),
        "path=%s soname=%s state=%s map=%p map_size=%zu bias=0x%llx "
        "deps=%u sys=%u sys_missing=%u reloc_relative=%u reloc_abs64=%u "
        "reloc_glob_dat=%u reloc_jump_slot=%u weak_unresolved=%u init_skipped=%d "
        "init_executed=%d sF_placeholder=%s shim_used=%u last_shim=%s error=%s",
        l->path[0] ? l->path : "-",
        l->soname[0] ? l->soname : "-",
        state_name(l->state), l->map_base, l->map_size,
        (unsigned long long)l->load_bias, l->dep_count, l->sys_count,
        l->sys_missing, l->rel_relative, l->rel_abs64, l->rel_glob_dat,
        l->rel_jump_slot, l->weak_unresolved, l->init_skipped,
        l->init_executed, l->sf_placeholder_used ? "used" : "not-used",
        l->shim_used, l->last_shim[0] ? l->last_shim : "-",
        l->error[0] ? l->error : "-");
    if (used >= sizeof(l->status)) used = sizeof(l->status) - 1;
    for (i = 0; i < l->dep_count && used + 8 < sizeof(l->status); ++i) {
        int n = snprintf(l->status + used, sizeof(l->status) - used,
                         " dep%u=%s", i,
                         l->deps[i]->soname[0] ? l->deps[i]->soname :
                                                l->deps[i]->path);
        if (n < 0) break;
        if ((size_t)n >= sizeof(l->status) - used) {
            used = sizeof(l->status) - 1;
            break;
        }
        used += (size_t)n;
    }
    if (l->weak_last[0] && used + 16 < sizeof(l->status)) {
        (void)snprintf(l->status + used, sizeof(l->status) - used,
                       " weak_last=%s", l->weak_last);
    }
}

/* ------------------------------------------------------------------ */
/* 单个库加载主流程                                                   */
/* ------------------------------------------------------------------ */

static int claim_soname_locked(struct elf_loader *l, const char *soname)
{
    unsigned i;
    if (!l || !soname || !*soname) return 0;
    for (i = 0; i < ELF_LOADER_MAX_LOADERS; ++i) {
        struct elf_loader *o = &g_loaders[i];
        if (o == l || o->state == LS_EMPTY || o->state == LS_FAILED)
            continue;
        if (!o->soname[0]) continue;
        if (strcmp(o->soname, soname) == 0 && strcmp(o->path, l->path) != 0) {
            loader_fail(l, "duplicate",
                        "同名 SONAME=%s 已由另一路径加载：%s（本路径 %s）",
                        soname, o->path, l->path);
            return 0;
        }
    }
    set_text(l->soname, sizeof(l->soname), "%s", soname);
    return 1;
}

static int claim_soname(struct elf_loader *l, const struct dyn_info *di)
{
    const char *soname = NULL;
    int ok;
    if (!l || !di) return 0;
    if (di->has_soname) soname = dynstr_name(l, di->soname_off);
    if (!soname || !*soname) soname = l->file_name;
    if (!soname || !*soname) {
        loader_fail(l, "duplicate", "无法确定 SONAME/文件名");
        return 0;
    }
    pthread_mutex_lock(&g_mu);
    ok = claim_soname_locked(l, soname);
    pthread_mutex_unlock(&g_mu);
    return ok;
}

static int load_impl(struct elf_loader *l, char *err, size_t errlen)
{
    struct file_image fi;
    const Elf64_Ehdr *eh = NULL;
    struct dyn_info di;
    char open_error[ELF_LOADER_ERROR_MAX];
    int ok = 0;

    memset(&fi, 0, sizeof(fi));
    fi.fd = -1;
    open_error[0] = '\0';
    if (!map_file(l->path, &fi, open_error, sizeof(open_error))) {
        loader_fail(l, "open", "%s", open_error[0] ? open_error : "unknown");
        set_text(err, errlen, "%s", l->error);
        return 0;
    }
    if (!validate_elf_header(l, &fi, &eh)) goto out;
    if (!map_segments(l, &fi, eh)) goto out;
    l->phnum=eh->e_phnum;
    memcpy(l->phdrs,(const unsigned char *)fi.map+eh->e_phoff,
           (size_t)l->phnum*sizeof(l->phdrs[0]));
    unmap_file(&fi);

    if (!parse_dynamic(l, &di)) goto out;
    if (!derive_symbol_count(l, &di)) goto out;
    if (!claim_soname(l, &di)) goto out;
    if (!load_dependencies(l, &di)) goto out;
    if (!apply_relocations(l)) goto out;
    if (g_require_initializers && l->sf_placeholder_used) {
        loader_fail(l,"initialization","strict load refuses placeholder Bionic stdio before constructors");
        goto out;
    }
    if (!run_initializers(l)) goto out;
    ok = 1;

out:
    unmap_file(&fi);
    if (!ok) {
        /* A returned initializer may have published code/data pointers or
         * spawned threads. Do not unmap after any such side effects. */
        if (l->map_base && !l->init_executed) {
            munmap(l->map_base, l->map_size);
            l->map_base = NULL;
            l->map_size = 0;
        }
        if (!l->error[0])
            loader_fail(l, "load", "未知加载失败");
        set_text(err, errlen, "%s", l->error);
    }
    return ok;
}

/* ------------------------------------------------------------------ */
/* 公开 API                                                           */
/* ------------------------------------------------------------------ */

static int loader_is_member(const elf_loader_t *l)
{
    unsigned i;
    if (!l) return 0;
    for (i = 0; i < ELF_LOADER_MAX_LOADERS; ++i) {
        if (l == &g_loaders[i]) return 1;
    }
    return 0;
}

/* Called under g_mu; graphs are bounded and cycles are already rejected by
 * the loader. A diagnostic map must not be silently promoted to a real load. */
static int graph_is_initialized(const struct elf_loader *l,unsigned depth)
{
    if (!l || depth>ELF_LOADER_MAX_LOADERS || l->sf_placeholder_used) return 0;
    if (l->init_skipped && (l->init_fn || l->init_array_count)) return 0;
    for (unsigned i=0;i<l->dep_count;++i)
        if (!graph_is_initialized(l->deps[i],depth+1)) return 0;
    return 1;
}

elf_loader_t *elf_loader_open(const char *path, char *err, size_t errlen)
{
    char canonical[PATH_MAX];
    char canonical2[ELF_LOADER_PATH_MAX];
    struct elf_loader *slot = NULL;
    unsigned i;
    int duplicate_wait = 0;
    char load_error[ELF_LOADER_ERROR_MAX];

    if (err && errlen) err[0] = '\0';
    load_error[0] = '\0';
    if (!path || !*path) {
        set_text(err, errlen, "路径为空");
        return NULL;
    }
    if (strlen(path) >= sizeof(canonical)) {
        set_text(err, errlen, "路径过长");
        return NULL;
    }
    if (!realpath(path, canonical)) {
        set_text(err, errlen, "realpath(%s) 失败（errno=%d）", path, errno);
        return NULL;
    }
    if (strlen(canonical) >= sizeof(canonical2)) {
        set_text(err, errlen, "规范化路径过长：%s", canonical);
        return NULL;
    }
    set_text(canonical2, sizeof(canonical2), "%s", canonical);

    pthread_mutex_lock(&g_mu);
    for (;;) {
        struct elf_loader *existing = NULL;
        for (i = 0; i < ELF_LOADER_MAX_LOADERS; ++i) {
            if (g_loaders[i].state != LS_EMPTY &&
                strcmp(g_loaders[i].path, canonical2) == 0) {
                existing = &g_loaders[i];
                break;
            }
        }
        if (existing) {
            if (existing->state == LS_READY) {
                if (g_require_initializers && !graph_is_initialized(existing,0)) {
                    set_text(err,errlen,"cached diagnostic mapping lacks required initialization or uses placeholder stdio: %s",canonical2);
                    pthread_mutex_unlock(&g_mu);
                    return NULL;
                }
                existing->refcount++;
                build_status_locked(existing);
                pthread_mutex_unlock(&g_mu);
                return existing;
            }
            if (existing->state == LS_FAILED) {
                set_text(err, errlen, "%s",
                         existing->error[0] ? existing->error : "之前加载失败");
                pthread_mutex_unlock(&g_mu);
                return NULL;
            }
            if (existing->state == LS_LOADING) {
                if (pthread_equal(existing->loading_thread, pthread_self())) {
                    set_text(err, errlen,
                             "检测到循环依赖或重入：%s", canonical2);
                    pthread_mutex_unlock(&g_mu);
                    return NULL;
                }
                duplicate_wait = 1;
                pthread_cond_wait(&g_cv, &g_mu);
                continue;
            }
        }
        break;
    }
    for (i = 0; i < ELF_LOADER_MAX_LOADERS; ++i) {
        if (g_loaders[i].state == LS_EMPTY) {
            slot = &g_loaders[i];
            break;
        }
    }
    if (!slot) {
        pthread_mutex_unlock(&g_mu);
        set_text(err, errlen, "加载器槽位耗尽（上限 %d）",
                 ELF_LOADER_MAX_LOADERS);
        return NULL;
    }
    memset(slot, 0, sizeof(*slot));
    slot->state = LS_LOADING;
    slot->refcount = 1;
    slot->loading_thread = pthread_self();
    slot->init_skipped = 1;
    set_text(slot->path, sizeof(slot->path), "%s", canonical2);
    path_basename(canonical2, slot->file_name, sizeof(slot->file_name));
    set_text(slot->status, sizeof(slot->status),
             "path=%s soname=- state=loading", slot->path);
    pthread_mutex_unlock(&g_mu);

    if (!load_impl(slot, load_error, sizeof(load_error))) {
        pthread_mutex_lock(&g_mu);
        slot->state = LS_FAILED;
        if (!slot->error[0])
            set_text(slot->error, sizeof(slot->error), "%s", load_error);
        build_status_locked(slot);
        pthread_cond_broadcast(&g_cv);
        pthread_mutex_unlock(&g_mu);
        set_text(err, errlen, "%s", slot->error);
        return NULL;
    }

    pthread_mutex_lock(&g_mu);
    if (g_require_initializers && !graph_is_initialized(slot,0)) {
        slot->state=LS_FAILED;
        set_text(slot->error,sizeof slot->error,"strict load rejected placeholder stdio or skipped initialization");
        build_status_locked(slot);pthread_cond_broadcast(&g_cv);
        pthread_mutex_unlock(&g_mu);set_text(err,errlen,"%s",slot->error);return NULL;
    }
    slot->state = LS_READY;
    build_status_locked(slot);
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_mu);
    (void)duplicate_wait;
    return slot;
}

elf_loader_t *elf_loader_acquire_initialized_cached(const char *path, char *err, size_t errlen)
{
    char canonical[PATH_MAX];
    if (err && errlen) err[0]='\0';
    if (!path || !realpath(path,canonical)) {
        set_text(err,errlen,"cannot canonicalize cached path"); return NULL;
    }
    pthread_mutex_lock(&g_mu);
    struct elf_loader *result=NULL;
    for (unsigned i=0;i<ELF_LOADER_MAX_LOADERS;++i) {
        struct elf_loader *l=&g_loaders[i];
        if (l->state==LS_READY && strcmp(l->path,canonical)==0 && graph_is_initialized(l,0)) {
            ++l->refcount; result=l; break;
        }
    }
    pthread_mutex_unlock(&g_mu);
    if (!result) set_text(err,errlen,"initialized cached graph unavailable");
    return result;
}

elf_loader_t *elf_loader_open_initialized(const char *path,char *err,size_t errlen)
{
    ++g_require_initializers;
    elf_loader_t *result=elf_loader_open(path,err,errlen);
    --g_require_initializers;
    return result;
}

int elf_loader_with_init_observer(elf_loader_init_observer observer, void *userdata,
                                  elf_loader_observed_body body, void *bodydata)
{
    if(!body) return -1;
    elf_loader_init_observer old=g_init_observer; void *old_data=g_init_observer_data;
    g_init_observer=observer;g_init_observer_data=userdata;
    int result=body(bodydata);
    g_init_observer=old;g_init_observer_data=old_data;
    return result;
}

elf_loader_t *elf_loader_open_initialized_traced(const char *path, char *err,
    size_t errlen, elf_loader_init_observer observer, void *userdata)
{
    elf_loader_init_observer previous = g_init_observer;
    void *previous_data = g_init_observer_data;
    g_init_observer = observer;
    g_init_observer_data = userdata;
    elf_loader_t *result = elf_loader_open_initialized(path,err,errlen);
    g_init_observer = previous;
    g_init_observer_data = previous_data;
    return result;
}

static int find_scoped(struct elf_loader *l,const char *name,uintptr_t *out,
                       struct elf_loader **seen,unsigned *count)
{
    for(unsigned i=0;i<*count;++i)if(seen[i]==l)return 0;
    if(*count>=ELF_LOADER_MAX_LOADERS || !loader_is_member(l))return 0;
    if(l->state!=LS_READY && !(l->state==LS_LOADING&&pthread_equal(l->loading_thread,pthread_self())))return 0;
    seen[(*count)++]=l;
    int r=find_symbol_in_loader(l,name,out);
    if(r)return r;
    for(unsigned i=0;i<l->dep_count;++i){r=find_scoped(l->deps[i],name,out,seen,count);if(r)return r;}
    if(!strcmp(name,"__sF"))return 0; /* Import-only fallback below, no host placeholder. */
    for(unsigned i=0;i<l->sys_count;++i)
        if(l->sys[i].ok&&dlsym_try(l->sys[i].handle,name,out))return 1;
    return 0;
}

int elf_loader_import_target(elf_loader_t *l,const char *name,void **target)
{
    if(target)*target=NULL;
    if(!loader_is_member(l)||!name||!target)return -1;
    pthread_mutex_lock(&g_mu);
    int result=0;uintptr_t found=0;
    if(l->state!=LS_READY){pthread_mutex_unlock(&g_mu);return -1;}
    for(unsigned table=0;table<2;++table){
        const Elf64_Rela *rel=table?l->jmprel:l->rela;
        size_t count=table?l->jmprel_count:l->rela_count;
        for(size_t i=0;i<count;++i){
            unsigned type=ELF64_R_TYPE(rel[i].r_info);
            size_t index=ELF64_R_SYM(rel[i].r_info);
            if((type!=R_AARCH64_GLOB_DAT&&type!=R_AARCH64_JUMP_SLOT)||index>=l->nsyms)continue;
            const Elf64_Sym *sym=&l->dynsym[index];
            if(sym->st_shndx!=SHN_UNDEF||sym->st_name>=l->dynstr_size)continue;
            const char *s=l->dynstr+sym->st_name;
            if(!memchr(s,0,l->dynstr_size-sym->st_name)||strcmp(s,name))continue;
            if(rel[i].r_offset>UINTPTR_MAX-l->load_bias){result=-1;goto finish_import;}
            uintptr_t address=l->load_bias+rel[i].r_offset;int readable=0;
            for(unsigned j=0;j<l->segment_count;++j){
                struct load_segment *seg=&l->segments[j];
                if((seg->prot&PROT_READ)&&seg->end>=sizeof(uintptr_t)&&address>=seg->start&&address<=seg->end-sizeof(uintptr_t)){readable=1;break;}
            }
            if(!readable){result=-1;goto finish_import;}
            uintptr_t value;memcpy(&value,(const void*)address,sizeof(value));
            if(result==1&&value!=found){result=-1;goto finish_import;}
            found=value;result=1;
        }
    }
finish_import:
    if(result==1)*target=(void*)found;
    pthread_mutex_unlock(&g_mu);return result;
}

void *elf_loader_own_executable_symbol(elf_loader_t *l,const char *name)
{
    if (!loader_is_member(l) || !name || !*name) return NULL;
    pthread_mutex_lock(&g_mu);
    if (l->state!=LS_READY) { pthread_mutex_unlock(&g_mu); return NULL; }
    uintptr_t address=0;
    for (size_t i=0;i<l->nsyms;++i) {
        const Elf64_Sym *sym=&l->dynsym[i];
        if (sym->st_name>=l->dynstr_size) continue;
        const char *symbol=l->dynstr+sym->st_name;
        if (!memchr(symbol,'\0',l->dynstr_size-sym->st_name) || strcmp(symbol,name)) continue;
        unsigned binding=ELF64_ST_BIND(sym->st_info);
        unsigned visibility=ELF64_ST_VISIBILITY(sym->st_other);
        if ((binding!=STB_GLOBAL && binding!=STB_WEAK) ||
            (visibility!=STV_DEFAULT && visibility!=STV_PROTECTED) ||
            ELF64_ST_TYPE(sym->st_info)!=STT_FUNC || sym->st_shndx==SHN_UNDEF ||
            sym->st_shndx>=SHN_LORESERVE || sym->st_value>UINTPTR_MAX-l->load_bias) continue;
        uintptr_t candidate=l->load_bias+(uintptr_t)sym->st_value;
        if (init_target_executable(l,candidate)) { address=candidate; break; }
    }
    pthread_mutex_unlock(&g_mu);
    return (void *)address;
}

void *elf_loader_symbol_scoped(elf_loader_t *loader,const char *name)
{
    if(!loader_is_member(loader)||!name||!*name)return NULL;
    struct elf_loader *seen[ELF_LOADER_MAX_LOADERS];unsigned count=0;uintptr_t out=0;
    pthread_mutex_lock(&g_mu);
    int ready=loader->state==LS_READY;
    pthread_mutex_unlock(&g_mu);
    if(!ready)return NULL;
    int found=find_scoped(loader,name,&out,seen,&count);
    if(found==2)return NULL;
    if(found==1)return (void*)out;
    /* A shim may satisfy this scope's imported symbol; it must not make an
     * unrelated compatibility symbol appear exported by every LOCAL DSO. */
    for(unsigned j=0;j<count;++j){
        struct elf_loader *l=seen[j];
        for(size_t i=0;i<l->nsyms;++i){
            const Elf64_Sym *s=&l->dynsym[i];
            if(s->st_shndx!=SHN_UNDEF||s->st_name>=l->dynstr_size)continue;
            const char *n=l->dynstr+s->st_name;
            if(memchr(n,0,l->dynstr_size-s->st_name)&&!strcmp(n,name)){
                int object=0;return shim_lookup(name,&object);
            }
        }
    }
    return NULL;
}

void *elf_loader_symbol(elf_loader_t *loader, const char *name)
{
    /* In particular, querying one DSO's JNI_OnLoad must not return the main
     * loader's unrelated JNI_OnLoad from the old process-wide map scan. */
    return elf_loader_symbol_scoped(loader,name);
}

const char *elf_loader_status(elf_loader_t *loader)
{
    static const char invalid[] = "elf_loader: 无效句柄";
    if (!loader_is_member(loader)) return invalid;
    return loader->status;
}

int elf_loader_close(elf_loader_t *loader)
{
    if (!loader_is_member(loader)) return -1;
    pthread_mutex_lock(&g_mu);
    if (loader->state == LS_LOADING) {
        pthread_mutex_unlock(&g_mu);
        return -1;
    }
    if (loader->refcount > 0) loader->refcount--;
    /* 映射按安全策略保留到进程结束；close 只释放调用方引用。 */
    build_status_locked(loader);
    pthread_mutex_unlock(&g_mu);
    return 0;
}

size_t elf_loader_last_error(elf_loader_t *loader, char *buf, size_t len)
{
    size_t n;
    if (!loader_is_member(loader)) {
        if (buf && len) buf[0] = '\0';
        return 0;
    }
    pthread_mutex_lock(&g_mu);
    n = strlen(loader->error);
    if (buf && len) {
        size_t copy = n < len - 1 ? n : len - 1;
        memcpy(buf, loader->error, copy);
        buf[copy] = '\0';
    }
    pthread_mutex_unlock(&g_mu);
    return n;
}

int elf_loader_is_valid(elf_loader_t *loader)
{
    int valid;
    if (!loader_is_member(loader)) return 0;
    pthread_mutex_lock(&g_mu);
    valid = loader->state == LS_READY || loader->state == LS_LOADING;
    pthread_mutex_unlock(&g_mu);
    return valid;
}
