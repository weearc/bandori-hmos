/*
 * R2 原生库加载诊断探针实现。
 *
 * 该文件按任务边界新建，可独立编译（C99/C11 均可用）；本仓库当前 CMake
 * 只编译 probe.cpp，且 CMakeLists.txt 不在本任务允许修改的范围内，因此
 * probe.cpp 以单一翻译单元的方式包含本文件。若后续把本文件加入
 * CMakeLists 的源列表，必须同时删除 probe.cpp 中的文本包含，避免重复定义。
 *
 * 安全边界：本文件只做文件读取、ELF 解析、dlopen/dlsym 和 hilog 输出。
 * 不注册信号处理器、不 fork/exec、不调用任何 Java_* 导出、不修改环境变量
 * 或链接器配置、不写系统设置。
 */

#include "native_probe.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <hilog/log.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define NP_LOG_DOMAIN 0x0000u
#define NP_LOG_TAG "NativeProbe"

#define NP_MAX_DIRS 4
#define NP_DIR_MAX 512
#define NP_NAME_MAX 256
#define NP_TEXT_MAX 256
#define NP_ERROR_MAX 512
#define NP_MAX_NEEDED 16
#define NP_MAX_VERNEED 16
#define NP_MAX_VERSIONS 16
#define NP_MAX_BIONIC 24
#define NP_MAX_SEGMENTS 32
#define NP_MAX_UNDEF_SCAN 200000
#define NP_FILE_LIMIT (64u * 1024u * 1024u)

/* ------------------------------------------------------------------------- */
/* 小型可增长字符串缓冲（JSON 输出）                                          */
/* ------------------------------------------------------------------------- */

typedef struct {
    char *data;
    size_t len;
    size_t cap;
    int failed;
} np_buf;

static int np_buf_reserve(np_buf *b, size_t extra) {
    size_t need;
    size_t cap;
    char *next;

    if (b->failed) return -1;
    if (extra > (size_t)-1 - b->len - 1) {
        b->failed = 1;
        return -1;
    }
    need = b->len + extra + 1;
    if (need <= b->cap) return 0;
    cap = b->cap ? b->cap : 1024;
    while (cap < need) {
        if (cap > ((size_t)1 << 20)) {
            cap = need;
            break;
        }
        cap *= 2;
    }
    next = (char *)realloc(b->data, cap);
    if (!next) {
        b->failed = 1;
        return -1;
    }
    b->data = next;
    b->cap = cap;
    return 0;
}

static void np_buf_putc(np_buf *b, char c) {
    if (np_buf_reserve(b, 1) != 0) return;
    b->data[b->len++] = c;
    b->data[b->len] = '\0';
}

static void np_buf_append(np_buf *b, const char *s, size_t n) {
    if (!s || n == 0) return;
    if (np_buf_reserve(b, n) != 0) return;
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

static void np_buf_puts(np_buf *b, const char *s) {
    if (!s) s = "";
    np_buf_append(b, s, strlen(s));
}

static void np_buf_printf(np_buf *b, const char *fmt, ...) {
    char stack[1024];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(stack, sizeof(stack), fmt, ap);
    va_end(ap);
    if (n < 0) {
        b->failed = 1;
        return;
    }
    if ((size_t)n < sizeof(stack)) {
        np_buf_append(b, stack, (size_t)n);
        return;
    }
    {
        char *heap = (char *)malloc((size_t)n + 1);
        if (!heap) {
            b->failed = 1;
            return;
        }
        va_start(ap, fmt);
        vsnprintf(heap, (size_t)n + 1, fmt, ap);
        va_end(ap);
        np_buf_append(b, heap, (size_t)n);
        free(heap);
    }
}

static void np_json_str(np_buf *b, const char *s) {
    static const char hex[] = "0123456789abcdef";
    np_buf_putc(b, '"');
    if (s) {
        const unsigned char *p = (const unsigned char *)s;
        for (; *p; ++p) {
            unsigned char c = *p;
            switch (c) {
            case '"': np_buf_puts(b, "\\\""); break;
            case '\\': np_buf_puts(b, "\\\\"); break;
            case '\n': np_buf_puts(b, "\\n"); break;
            case '\r': np_buf_puts(b, "\\r"); break;
            case '\t': np_buf_puts(b, "\\t"); break;
            default:
                if (c < 0x20) {
                    char esc[7];
                    esc[0] = '\\';
                    esc[1] = 'u';
                    esc[2] = '0';
                    esc[3] = '0';
                    esc[4] = hex[(c >> 4) & 0x0f];
                    esc[5] = hex[c & 0x0f];
                    esc[6] = '\0';
                    np_buf_puts(b, esc);
                } else {
                    np_buf_putc(b, (char)c);
                }
                break;
            }
        }
    }
    np_buf_putc(b, '"');
}

/* ------------------------------------------------------------------------- */
/* hilog：每条日志先格式化到本地缓冲，去掉换行，再用固定格式串输出。          */
/* ------------------------------------------------------------------------- */

static void np_log(const char *fmt, ...) {
    char line[1200];
    va_list ap;
    char *p;

    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    for (p = line; *p; ++p) {
        if (*p == '\n' || *p == '\r') *p = ' ';
    }
    OH_LOG_Print(LOG_APP, LOG_INFO, NP_LOG_DOMAIN, NP_LOG_TAG, "%{public}s", line);
}

/* ------------------------------------------------------------------------- */
/* ELF64 基础读取（纯字节偏移，不依赖 libelf/elf.h）                          */
/* ------------------------------------------------------------------------- */

#define NP_EI_CLASS 4
#define NP_EI_DATA 5
#define NP_ELFCLASS64 2
#define NP_ELFDATA2LSB 1
#define NP_EM_AARCH64 183u

#define NP_PT_LOAD 1u
#define NP_PT_DYNAMIC 2u

#define NP_SHT_DYNSYM 11u

#define NP_DT_NULL 0u
#define NP_DT_NEEDED 1u
#define NP_DT_HASH 4u
#define NP_DT_STRTAB 5u
#define NP_DT_SYMTAB 6u
#define NP_DT_STRSZ 10u
#define NP_DT_SYMENT 11u
#define NP_DT_SONAME 14u
#define NP_DT_RPATH 15u
#define NP_DT_RUNPATH 29u
#define NP_DT_GNU_HASH 0x6ffffef5u
#define NP_DT_VERNEED 0x6ffffffeull
#define NP_DT_VERNEEDNUM 0x6fffffffull

typedef struct {
    uint64_t vaddr;
    uint64_t filesz;
    uint64_t offset;
} np_segment;

typedef struct {
    char library[NP_TEXT_MAX];
    char versions[NP_MAX_VERSIONS][NP_TEXT_MAX];
    int version_count;
} np_verneed_item;

typedef struct {
    int ok;
    char error[NP_TEXT_MAX];
    char machine[64];
    char soname[NP_TEXT_MAX];
    char needed[NP_MAX_NEEDED][NP_TEXT_MAX];
    int needed_count;
    char rpath[NP_TEXT_MAX];
    char runpath[NP_TEXT_MAX];
    np_verneed_item verneed[NP_MAX_VERNEED];
    int verneed_count;
    int undefined_symbols;
    char bionic[NP_MAX_BIONIC][NP_TEXT_MAX];
    int bionic_count;
    int symbol_table_entries;
} np_elf_summary;

static int np_range_ok(size_t file_size, uint64_t off, uint64_t size) {
    return off <= (uint64_t)file_size && size <= (uint64_t)file_size - off;
}

static uint16_t np_u16(const unsigned char *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t np_u32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint64_t np_u64(const unsigned char *p) {
    return (uint64_t)np_u32(p) | ((uint64_t)np_u32(p + 4) << 32);
}

static int np_read_file(const char *path, unsigned char **out, size_t *out_size,
                        char *err, size_t err_cap) {
    int fd;
    struct stat st;
    unsigned char *buf;
    size_t done = 0;

    *out = NULL;
    *out_size = 0;
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        snprintf(err, err_cap, "open failed: errno=%d", errno);
        return -1;
    }
    if (fstat(fd, &st) != 0) {
        snprintf(err, err_cap, "fstat failed: errno=%d", errno);
        close(fd);
        return -1;
    }
    if (st.st_size <= 0) {
        snprintf(err, err_cap, "file is empty");
        close(fd);
        return -1;
    }
    if ((uint64_t)st.st_size > (uint64_t)NP_FILE_LIMIT) {
        snprintf(err, err_cap, "file too large: %llu", (unsigned long long)st.st_size);
        close(fd);
        return -1;
    }
    buf = (unsigned char *)malloc((size_t)st.st_size);
    if (!buf) {
        snprintf(err, err_cap, "out of memory");
        close(fd);
        return -1;
    }
    while (done < (size_t)st.st_size) {
        ssize_t got = read(fd, buf + done, (size_t)st.st_size - done);
        if (got < 0) {
            if (errno == EINTR) continue;
            snprintf(err, err_cap, "read failed: errno=%d", errno);
            free(buf);
            close(fd);
            return -1;
        }
        if (got == 0) break;
        done += (size_t)got;
    }
    close(fd);
    if (done != (size_t)st.st_size) {
        snprintf(err, err_cap, "short read: %zu/%lld", done, (long long)st.st_size);
        free(buf);
        return -1;
    }
    *out = buf;
    *out_size = done;
    return 0;
}

static int np_vaddr_to_off(const np_segment *segs, int nsegs, uint64_t vaddr,
                           uint64_t size, size_t file_size, uint64_t *out) {
    int i;
    for (i = 0; i < nsegs; ++i) {
        uint64_t start = segs[i].vaddr;
        uint64_t end = start + segs[i].filesz;
        if (vaddr < start || vaddr > end) continue;
        if (size > end - vaddr) continue;
        {
            uint64_t delta = vaddr - start;
            uint64_t off = segs[i].offset + delta;
            if (!np_range_ok(file_size, off, size)) continue;
            *out = off;
            return 0;
        }
    }
    return -1;
}

static int np_dynstr(const unsigned char *file, size_t file_size, uint64_t str_off,
                     uint64_t str_size, uint32_t name_off, char *out, size_t out_cap) {
    uint64_t start;
    uint64_t i;
    uint64_t max_len;

    if (!out || out_cap == 0) return -1;
    out[0] = '\0';
    if (name_off >= str_size) return -1;
    start = str_off + name_off;
    if (!np_range_ok(file_size, start, 1)) return -1;
    max_len = str_size - name_off;
    for (i = 0; i < max_len; ++i) {
        unsigned char c;
        if (!np_range_ok(file_size, start + i, 1)) return -1;
        c = file[start + i];
        if (c == 0) break;
        if (i + 1 >= out_cap) return -1;
        out[i] = (char)c;
    }
    if (i >= max_len) return -1;
    out[i] = '\0';
    return 0;
}

static int np_gnu_hash_symcount(const unsigned char *file, size_t file_size,
                                uint64_t off, int *out_count) {
    uint32_t nbuckets;
    uint32_t symoffset;
    uint32_t bloom_size;
    uint64_t buckets_off;
    uint64_t chain_off;
    uint64_t max_index;
    uint32_t i;

    *out_count = 0;
    if (!np_range_ok(file_size, off, 16)) return -1;
    nbuckets = np_u32(file + off);
    symoffset = np_u32(file + off + 4);
    bloom_size = np_u32(file + off + 8);
    if (nbuckets == 0 || nbuckets > (1u << 20) || bloom_size > (1u << 20)) return -1;
    buckets_off = off + 16 + (uint64_t)bloom_size * 8;
    if (!np_range_ok(file_size, buckets_off, (uint64_t)nbuckets * 4)) return -1;
    chain_off = buckets_off + (uint64_t)nbuckets * 4;
    max_index = symoffset;
    for (i = 0; i < nbuckets; ++i) {
        uint32_t idx = np_u32(file + buckets_off + (uint64_t)i * 4);
        uint32_t guard = 0;
        if (idx < symoffset) continue;
        while (guard++ < NP_MAX_UNDEF_SCAN) {
            uint64_t chain_pos = chain_off + (uint64_t)(idx - symoffset) * 4;
            uint32_t val;
            if (!np_range_ok(file_size, chain_pos, 4)) break;
            val = np_u32(file + chain_pos);
            if (val & 1u) {
                if ((uint64_t)idx + 1 > max_index) max_index = (uint64_t)idx + 1;
                break;
            }
            ++idx;
        }
    }
    *out_count = (int)max_index;
    return 0;
}

static int np_is_bionic_symbol(const char *name, char *why, size_t why_cap) {
    /* 已知 Bionic/Android 专有符号集合：
     *   - 前 7 个是本任务点名的风险符号；
     *   - 其后 6 个是 Android liblog/系统 API，HarmonyOS 的 musl 基础 libc
     *     与 liblog 不承诺提供；
     *   - 最后 6 个是原库实际引用的 fortified/locale 变体，用于与静态分析
     *     的扩展风险命中交叉核对。
     * 该表只用于诊断提示，不代表其中每个符号在 HarmonyOS 上都必然缺失。 */
    static const char *const known[] = {
        "__sF",
        "__errno",
        "android_set_abort_message",
        "__open_2",
        "strtoll_l",
        "strtoull_l",
        "__system_property_get",
        "__android_log_print",
        "__android_log_write",
        "__android_log_assert",
        "__android_log_vprint",
        "android_get_device_api_level",
        "android_get_application_target_sdk_version",
        "__memmove_chk",
        "__read_chk",
        "__strcat_chk",
        "__strcpy_chk",
        "__vsnprintf_chk",
        "strtold_l"
    };
    size_t i;
    for (i = 0; i < sizeof(known) / sizeof(known[0]); ++i) {
        if (strcmp(name, known[i]) == 0) {
            if (why && why_cap) snprintf(why, why_cap, "%s", known[i]);
            return 1;
        }
    }
    return 0;
}

static void np_elf_parse(const char *path, np_elf_summary *s) {
    unsigned char *file = NULL;
    size_t file_size = 0;
    char err[NP_ERROR_MAX];
    np_segment segs[NP_MAX_SEGMENTS];
    int nsegs = 0;
    uint64_t phoff;
    uint16_t phentsize;
    uint16_t phnum;
    uint64_t shoff;
    uint16_t shentsize;
    uint16_t shnum;
    uint64_t dynamic_off = 0;
    uint64_t dynamic_size = 0;
    int have_dynamic = 0;
    uint64_t strtab = 0;
    uint64_t strsz = 0;
    uint64_t symtab = 0;
    uint64_t syment = 24;
    uint64_t hash_off = 0;
    uint64_t gnu_hash_off = 0;
    uint64_t verneed_off = 0;
    uint64_t verneed_num = 0;
    uint32_t soname_off = 0;
    uint32_t needed_off[NP_MAX_NEEDED];
    int needed_n = 0;
    uint32_t rpath_off = 0;
    uint32_t runpath_off = 0;
    int have_soname = 0;
    int have_rpath = 0;
    int have_runpath = 0;
    uint64_t pos;
    int i;

    memset(s, 0, sizeof(*s));
    snprintf(s->machine, sizeof(s->machine), "unknown");

    if (np_read_file(path, &file, &file_size, err, sizeof(err)) != 0) {
        snprintf(s->error, sizeof(s->error), "%s", err);
        return;
    }
    if (file_size < 64 || memcmp(file, "\177ELF", 4) != 0) {
        snprintf(s->error, sizeof(s->error), "not an ELF file");
        free(file);
        return;
    }
    if (file[NP_EI_CLASS] != NP_ELFCLASS64 || file[NP_EI_DATA] != NP_ELFDATA2LSB) {
        snprintf(s->error, sizeof(s->error), "unsupported ELF class/encoding");
        free(file);
        return;
    }
    {
        uint16_t machine = np_u16(file + 18);
        if (machine == NP_EM_AARCH64) {
            snprintf(s->machine, sizeof(s->machine), "aarch64");
        } else {
            snprintf(s->machine, sizeof(s->machine), "machine=%u", (unsigned)machine);
        }
    }

    phoff = np_u64(file + 32);
    phentsize = np_u16(file + 54);
    phnum = np_u16(file + 56);
    shoff = np_u64(file + 40);
    shentsize = np_u16(file + 58);
    shnum = np_u16(file + 60);

    if (phnum == 0 || phentsize < 56 || !np_range_ok(file_size, phoff, (uint64_t)phentsize * phnum)) {
        snprintf(s->error, sizeof(s->error), "invalid program header table");
        free(file);
        return;
    }

    for (i = 0; i < (int)phnum; ++i) {
        uint64_t base = phoff + (uint64_t)i * phentsize;
        uint32_t type = np_u32(file + base);
        if (type == NP_PT_LOAD && nsegs < NP_MAX_SEGMENTS) {
            segs[nsegs].offset = np_u64(file + base + 8);
            segs[nsegs].vaddr = np_u64(file + base + 16);
            segs[nsegs].filesz = np_u64(file + base + 32);
            ++nsegs;
        } else if (type == NP_PT_DYNAMIC) {
            dynamic_off = np_u64(file + base + 8);
            dynamic_size = np_u64(file + base + 32);
            have_dynamic = 1;
        }
    }

    if (!have_dynamic || dynamic_size < 16 ||
        !np_range_ok(file_size, dynamic_off, dynamic_size)) {
        snprintf(s->error, sizeof(s->error), "no usable PT_DYNAMIC");
        free(file);
        return;
    }

    for (pos = dynamic_off; pos + 16 <= dynamic_off + dynamic_size; pos += 16) {
        uint64_t tag = np_u64(file + pos);
        uint64_t val = np_u64(file + pos + 8);
        if (tag == NP_DT_NULL) break;
        switch (tag) {
        case NP_DT_NEEDED:
            if (needed_n < NP_MAX_NEEDED) needed_off[needed_n++] = (uint32_t)val;
            break;
        case NP_DT_SONAME:
            soname_off = (uint32_t)val;
            have_soname = 1;
            break;
        case NP_DT_RPATH:
            rpath_off = (uint32_t)val;
            have_rpath = 1;
            break;
        case NP_DT_RUNPATH:
            runpath_off = (uint32_t)val;
            have_runpath = 1;
            break;
        case NP_DT_STRTAB:
            strtab = val;
            break;
        case NP_DT_STRSZ:
            strsz = val;
            break;
        case NP_DT_SYMTAB:
            symtab = val;
            break;
        case NP_DT_SYMENT:
            if (val >= 24 && val <= 256) syment = val;
            break;
        case NP_DT_HASH:
            hash_off = val;
            break;
        case NP_DT_GNU_HASH:
            gnu_hash_off = val;
            break;
        case NP_DT_VERNEED:
            verneed_off = val;
            break;
        case NP_DT_VERNEEDNUM:
            verneed_num = val;
            break;
        default:
            break;
        }
    }

    if (strtab == 0 || strsz == 0) {
        snprintf(s->error, sizeof(s->error), "missing DT_STRTAB/DT_STRSZ");
        free(file);
        return;
    }
    {
        uint64_t strtab_off = 0;
        if (np_vaddr_to_off(segs, nsegs, strtab, strsz, file_size, &strtab_off) != 0) {
            snprintf(s->error, sizeof(s->error), "DT_STRTAB is not file-backed");
            free(file);
            return;
        }
        strtab = strtab_off;
    }

    if (have_soname) {
        np_dynstr(file, file_size, strtab, strsz, soname_off, s->soname, sizeof(s->soname));
    }
    for (i = 0; i < needed_n; ++i) {
        if (np_dynstr(file, file_size, strtab, strsz, needed_off[i],
                      s->needed[s->needed_count], sizeof(s->needed[0])) == 0) {
            ++s->needed_count;
        }
    }
    if (have_rpath) {
        np_dynstr(file, file_size, strtab, strsz, rpath_off, s->rpath, sizeof(s->rpath));
    }
    if (have_runpath) {
        np_dynstr(file, file_size, strtab, strsz, runpath_off, s->runpath, sizeof(s->runpath));
    }

    /* VERNEED：每个依赖库一行，versions[] 为该库要求的版本节点名字。 */
    if (verneed_off != 0 && verneed_num > 0 &&
        np_vaddr_to_off(segs, nsegs, verneed_off, 16, file_size, &pos) == 0) {
        uint64_t guard = 0;
        while (guard++ < verneed_num && s->verneed_count < NP_MAX_VERNEED) {
            uint16_t vn_cnt;
            uint32_t vn_file;
            uint32_t vn_aux;
            uint32_t vn_next;
            np_verneed_item *item;
            uint64_t aux_pos;
            int j;

            if (!np_range_ok(file_size, pos, 16)) break;
            vn_cnt = np_u16(file + pos + 2);
            vn_file = np_u32(file + pos + 4);
            vn_aux = np_u32(file + pos + 8);
            vn_next = np_u32(file + pos + 12);
            item = &s->verneed[s->verneed_count];
            memset(item, 0, sizeof(*item));
            np_dynstr(file, file_size, strtab, strsz, vn_file,
                      item->library, sizeof(item->library));
            aux_pos = pos + vn_aux;
            for (j = 0; j < (int)vn_cnt && item->version_count < NP_MAX_VERSIONS; ++j) {
                uint32_t vna_name;
                uint32_t vna_next;
                if (!np_range_ok(file_size, aux_pos, 16)) break;
                vna_name = np_u32(file + aux_pos + 8);
                vna_next = np_u32(file + aux_pos + 12);
                if (np_dynstr(file, file_size, strtab, strsz, vna_name,
                              item->versions[item->version_count],
                              sizeof(item->versions[0])) == 0) {
                    ++item->version_count;
                }
                if (vna_next == 0) break;
                aux_pos += vna_next;
            }
            ++s->verneed_count;
            if (vn_next == 0) break;
            pos += vn_next;
        }
    }

    /* 动态符号表大小：优先 .dynsym 节，其次 DT_HASH，再其次 DT_GNU_HASH。 */
    if (symtab != 0) {
        uint64_t symtab_off = 0;
        int nsyms = 0;

        if (shnum > 0 && shentsize >= 64 &&
            np_range_ok(file_size, shoff, (uint64_t)shentsize * shnum)) {
            for (i = 0; i < (int)shnum; ++i) {
                uint64_t base = shoff + (uint64_t)i * shentsize;
                uint32_t sh_type = np_u32(file + base + 4);
                if (sh_type == NP_SHT_DYNSYM) {
                    uint64_t sh_size = np_u64(file + base + 32);
                    uint64_t sh_entsize = np_u64(file + base + 56);
                    if (sh_entsize == 0) sh_entsize = syment;
                    if (sh_entsize > 0) nsyms = (int)(sh_size / sh_entsize);
                    break;
                }
            }
        }
        if (nsyms == 0 && hash_off != 0) {
            uint64_t hoff = 0;
            if (np_vaddr_to_off(segs, nsegs, hash_off, 8, file_size, &hoff) == 0) {
                nsyms = (int)np_u32(file + hoff + 4);
            }
        }
        if (nsyms == 0 && gnu_hash_off != 0) {
            uint64_t goff = 0;
            if (np_vaddr_to_off(segs, nsegs, gnu_hash_off, 16, file_size, &goff) == 0) {
                np_gnu_hash_symcount(file, file_size, goff, &nsyms);
            }
        }
        if (nsyms > NP_MAX_UNDEF_SCAN) nsyms = NP_MAX_UNDEF_SCAN;
        s->symbol_table_entries = nsyms;

        if (np_vaddr_to_off(segs, nsegs, symtab, (uint64_t)nsyms * syment,
                            file_size, &symtab_off) == 0) {
            int idx;
            for (idx = 1; idx < nsyms; ++idx) {
                uint64_t base = symtab_off + (uint64_t)idx * syment;
                uint32_t st_name;
                uint16_t st_shndx;
                char name[NP_TEXT_MAX];
                if (!np_range_ok(file_size, base, syment)) break;
                st_name = np_u32(file + base);
                st_shndx = np_u16(file + base + 6);
                if (st_name == 0 || st_shndx != 0) continue;
                if (np_dynstr(file, file_size, strtab, strsz, st_name,
                              name, sizeof(name)) != 0) {
                    continue;
                }
                ++s->undefined_symbols;
                if (np_is_bionic_symbol(name, NULL, 0)) {
                    int seen = 0;
                    int k;
                    for (k = 0; k < s->bionic_count; ++k) {
                        if (strcmp(s->bionic[k], name) == 0) {
                            seen = 1;
                            break;
                        }
                    }
                    if (!seen && s->bionic_count < NP_MAX_BIONIC) {
                        snprintf(s->bionic[s->bionic_count], sizeof(s->bionic[0]),
                                 "%s", name);
                        ++s->bionic_count;
                    }
                }
            }
        }
    }

    if (s->error[0] == '\0') s->ok = 1;
    free(file);
}

/* ------------------------------------------------------------------------- */
/* 目录与 dlopen                                                              */
/* ------------------------------------------------------------------------- */

typedef struct {
    char dir[NP_DIR_MAX];
} np_dir;

static int np_add_dir(np_dir *dirs, int count, int max, const char *dir) {
    int i;
    size_t len;

    if (!dir || !*dir) return count;
    len = strlen(dir);
    if (len >= NP_DIR_MAX) return count;
    for (i = 0; i < count; ++i) {
        if (strcmp(dirs[i].dir, dir) == 0) return count;
    }
    if (count >= max) return count;
    snprintf(dirs[count].dir, sizeof(dirs[count].dir), "%s", dir);
    /* 去掉末尾斜杠，避免拼出 "//name"。 */
    len = strlen(dirs[count].dir);
    while (len > 1 && dirs[count].dir[len - 1] == '/') {
        dirs[count].dir[--len] = '\0';
    }
    return count + 1;
}

static int np_build_dirs(const char *preferred, np_dir *dirs, int max) {
    int count = 0;
    const char *env;

    count = np_add_dir(dirs, count, max, "/data/storage/el1/bundle/libs/arm64");
    count = np_add_dir(dirs, count, max, preferred);
    env = getenv("NATIVE_PROBE_LIB_DIR");
    count = np_add_dir(dirs, count, max, env);
    return count;
}

typedef struct {
    const char *name;
    int role_preload;
    void *handle;
    char path[NP_DIR_MAX + NP_NAME_MAX + 2];
    char error[NP_ERROR_MAX];
    int attempts;
} np_lib_result;

static const char *np_dlerror_text(void) {
    const char *err = dlerror();
    return err ? err : "unknown";
}

/*
 * 依次尝试每个目录的绝对路径。attempts_json 追加形如
 *   [{"path":"...","result":"ok|fail","error":"..."}]
 * 的数组；out->handle 为最后一次成功（或失败时为空）。
 */
/* Preload the Bionic compatibility definitions into the global namespace.
 * Without this, libcustom_rules.so fails on versioned LIBC symbols such as
 * __errno before any JNI symbol can be inspected. */
static void np_preload_compat(const np_dir *dirs, int ndirs) {
    int i;
    for (i = 0; i < ndirs; ++i) {
        char path[NP_DIR_MAX + NP_NAME_MAX + 2];
        void *handle;
        const char *error;
        snprintf(path, sizeof(path), "%s/%s", dirs[i].dir, "libandroid_compat.so");
        dlerror();
        handle = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
        error = np_dlerror_text();
        if (handle) {
            np_log("NATIVE_PROBE stage=preload lib=libandroid_compat.so path=%s result=ok", path);
            return;
        }
        np_log("NATIVE_PROBE stage=preload lib=libandroid_compat.so path=%s result=fail error=%s",
               path, error ? error : "unknown");
    }
}

static void np_load_lib(const np_dir *dirs, int ndirs, const char *name, int flags,
                        np_lib_result *out, np_buf *attempts_json) {
    int i;

    memset(out, 0, sizeof(*out));
    out->name = name;
    np_buf_puts(attempts_json, "[");
    for (i = 0; i < ndirs; ++i) {
        char path[NP_DIR_MAX + NP_NAME_MAX + 2];
        void *handle;
        const char *error;

        snprintf(path, sizeof(path), "%s/%s", dirs[i].dir, name);
        if (i) np_buf_puts(attempts_json, ",");
        np_buf_puts(attempts_json, "{\"path\":");
        np_json_str(attempts_json, path);
        np_buf_puts(attempts_json, ",\"result\":");

        dlerror();
        handle = dlopen(path, flags);
        error = np_dlerror_text();
        ++out->attempts;
        if (handle) {
            np_buf_puts(attempts_json, "\"ok\",\"error\":null}");
            out->handle = handle;
            snprintf(out->path, sizeof(out->path), "%s", path);
            out->error[0] = '\0';
            np_log("NATIVE_PROBE stage=dlopen lib=%s path=%s result=ok error=", name, path);
            break;
        }
        np_buf_puts(attempts_json, "\"fail\"");
        if (error) {
            np_buf_puts(attempts_json, ",\"error\":");
            np_json_str(attempts_json, error);
        }
        np_buf_puts(attempts_json, "}");
        snprintf(out->error, sizeof(out->error), "%s", error);
        np_log("NATIVE_PROBE stage=dlopen lib=%s path=%s result=fail error=%s",
               name, path, error);
    }
    np_buf_puts(attempts_json, "]");
}

static void np_pick_elf_path(const np_dir *dirs, int ndirs, const char *name,
                             char *out, size_t out_cap) {
    int i;
    for (i = 0; i < ndirs; ++i) {
        char path[NP_DIR_MAX + NP_NAME_MAX + 2];
        snprintf(path, sizeof(path), "%s/%s", dirs[i].dir, name);
        if (access(path, R_OK) == 0) {
            snprintf(out, out_cap, "%s", path);
            return;
        }
    }
    if (ndirs > 0) {
        snprintf(out, out_cap, "%s/%s", dirs[0].dir, name);
    } else {
        snprintf(out, out_cap, "%s", name);
    }
}

static int np_dlsym_exists(void *handle, const char *name, char *err, size_t err_cap) {
    void *address;
    const char *error;

    if (err && err_cap) err[0] = '\0';
    if (!handle) {
        if (err && err_cap) snprintf(err, err_cap, "library handle is NULL");
        return 0;
    }
    dlerror();
    address = dlsym(handle, name);
    error = np_dlerror_text();
    if (error) {
        if (err && err_cap) snprintf(err, err_cap, "%s", error);
        return 0;
    }
    if (!address) {
        if (err && err_cap) snprintf(err, err_cap, "dlsym returned NULL with no dlerror");
        return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------------- */
/* JSON 输出                                                                 */
/* ------------------------------------------------------------------------- */

static np_buf g_report;
static pthread_mutex_t g_report_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_running = 0;

static void np_json_elf(np_buf *b, const np_elf_summary *e) {
    int i;
    int j;

    np_buf_puts(b, "{\"ok\":");
    np_buf_puts(b, e->ok ? "true" : "false");
    np_buf_puts(b, ",\"error\":");
    np_json_str(b, e->error);
    np_buf_puts(b, ",\"machine\":");
    np_json_str(b, e->machine);
    np_buf_puts(b, ",\"soname\":");
    np_json_str(b, e->soname);
    np_buf_puts(b, ",\"needed\":[");
    for (i = 0; i < e->needed_count; ++i) {
        if (i) np_buf_puts(b, ",");
        np_json_str(b, e->needed[i]);
    }
    np_buf_puts(b, "],\"rpath\":");
    np_json_str(b, e->rpath);
    np_buf_puts(b, ",\"runpath\":");
    np_json_str(b, e->runpath);
    np_buf_puts(b, ",\"verneed\":[");
    for (i = 0; i < e->verneed_count; ++i) {
        if (i) np_buf_puts(b, ",");
        np_buf_puts(b, "{\"library\":");
        np_json_str(b, e->verneed[i].library);
        np_buf_puts(b, ",\"versions\":[");
        for (j = 0; j < e->verneed[i].version_count; ++j) {
            if (j) np_buf_puts(b, ",");
            np_json_str(b, e->verneed[i].versions[j]);
        }
        np_buf_puts(b, "]}");
    }
    np_buf_puts(b, "],\"undefined_symbols\":");
    np_buf_printf(b, "%d", e->undefined_symbols);
    np_buf_puts(b, ",\"symbol_table_entries\":");
    np_buf_printf(b, "%d", e->symbol_table_entries);
    np_buf_puts(b, ",\"bionic_count\":");
    np_buf_printf(b, "%d", e->bionic_count);
    np_buf_puts(b, ",\"bionic_symbols\":[");
    for (i = 0; i < e->bionic_count; ++i) {
        if (i) np_buf_puts(b, ",");
        np_json_str(b, e->bionic[i]);
    }
    np_buf_puts(b, "]}");
}

static void np_emit_soname_log(const char *name, const np_elf_summary *e) {
    char needed[512];
    char verneed[768];
    char bionic[768];
    size_t nlen = 0;
    size_t vlen = 0;
    size_t blen = 0;
    int i;
    int j;

    needed[0] = '\0';
    verneed[0] = '\0';
    bionic[0] = '\0';
    for (i = 0; i < e->needed_count && nlen < sizeof(needed); ++i) {
        int written = snprintf(needed + nlen, sizeof(needed) - nlen, "%s%s",
                               i ? "," : "", e->needed[i]);
        if (written < 0) break;
        nlen += (size_t)written;
    }
    for (i = 0; i < e->verneed_count && vlen < sizeof(verneed); ++i) {
        int written = snprintf(verneed + vlen, sizeof(verneed) - vlen, "%s%s:",
                               i ? ";" : "", e->verneed[i].library);
        if (written < 0) break;
        vlen += (size_t)written;
        for (j = 0; j < e->verneed[i].version_count && vlen < sizeof(verneed); ++j) {
            written = snprintf(verneed + vlen, sizeof(verneed) - vlen, "%s%s",
                               j ? "|" : "", e->verneed[i].versions[j]);
            if (written < 0) break;
            vlen += (size_t)written;
        }
    }
    for (i = 0; i < e->bionic_count && blen < sizeof(bionic); ++i) {
        int written = snprintf(bionic + blen, sizeof(bionic) - blen, "%s%s",
                               i ? "," : "", e->bionic[i]);
        if (written < 0) break;
        blen += (size_t)written;
    }
    np_log("NATIVE_PROBE soname=%s needed=%s verneed=%s bionic_symbols=%d:%s",
           name, needed, verneed, e->bionic_count, bionic);
}

/* ------------------------------------------------------------------------- */
/* 主流程                                                                     */
/* ------------------------------------------------------------------------- */

static const char *const NP_EXPORTS[] = {
    "Java_com_base_crules_CustomRules_getVersion",
    "Java_com_base_crules_CustomRules_decodeStr",
    "Java_com_base_crules_CustomRules_encodeStr",
    "Java_com_base_crules_CustomRules_getTokens"
};

static int np_run_internal(const char *preferred_dir) {
    np_dir dirs[NP_MAX_DIRS];
    int ndirs;
    np_lib_result crypto;
    np_lib_result ssl;
    np_lib_result custom;
    np_elf_summary crypto_elf;
    np_elf_summary ssl_elf;
    np_elf_summary custom_elf;
    char crypto_elf_path[NP_DIR_MAX + NP_NAME_MAX + 2];
    char ssl_elf_path[NP_DIR_MAX + NP_NAME_MAX + 2];
    char custom_elf_path[NP_DIR_MAX + NP_NAME_MAX + 2];
    np_buf attempts_crypto;
    np_buf attempts_ssl;
    np_buf attempts_custom;
    int export_found[sizeof(NP_EXPORTS) / sizeof(NP_EXPORTS[0])];
    char export_error[sizeof(NP_EXPORTS) / sizeof(NP_EXPORTS[0])][NP_ERROR_MAX];
    int loaded = 0;
    int i;

    pthread_mutex_lock(&g_report_lock);
    if (g_running) {
        pthread_mutex_unlock(&g_report_lock);
        return -2;
    }
    g_running = 1;
    g_report.len = 0;
    if (g_report.data && g_report.cap) g_report.data[0] = '\0';
    g_report.failed = 0;

    memset(&attempts_crypto, 0, sizeof(attempts_crypto));
    memset(&attempts_ssl, 0, sizeof(attempts_ssl));
    memset(&attempts_custom, 0, sizeof(attempts_custom));
    memset(export_found, 0, sizeof(export_found));
    memset(export_error, 0, sizeof(export_error));

    ndirs = np_build_dirs(preferred_dir, dirs, NP_MAX_DIRS);
    np_log("NATIVE_PROBE stage=start abi=arm64-v8a dirs=%d", ndirs);
    for (i = 0; i < ndirs; ++i) {
        np_log("NATIVE_PROBE stage=dir index=%d path=%s", i, dirs[i].dir);
    }

    np_preload_compat(dirs, ndirs);
    /* 1) 预加载 libcrypto_1_1.so、libssl_1_1.so（RTLD_NOW|RTLD_GLOBAL）。 */
    np_load_lib(dirs, ndirs, "libcrypto_1_1.so", RTLD_NOW | RTLD_GLOBAL, &crypto,
                &attempts_crypto);
    crypto.role_preload = 1;
    if (crypto.handle) ++loaded;
    np_load_lib(dirs, ndirs, "libssl_1_1.so", RTLD_NOW | RTLD_GLOBAL, &ssl,
                &attempts_ssl);
    ssl.role_preload = 1;
    if (ssl.handle) ++loaded;

    /* 2) 即使预加载失败也继续直接加载 libcustom_rules.so（RTLD_NOW|RTLD_LOCAL），
     *    记录真实 dlerror，不中止诊断。 */
    np_load_lib(dirs, ndirs, "libcustom_rules.so", RTLD_NOW | RTLD_GLOBAL, &custom,
                &attempts_custom);
    if (custom.handle) ++loaded;

    /* 3) 静态 ELF 摘要：优先解析成功加载的路径；否则解析候选目录中存在的文件。 */
    if (crypto.path[0]) {
        snprintf(crypto_elf_path, sizeof(crypto_elf_path), "%s", crypto.path);
    } else {
        np_pick_elf_path(dirs, ndirs, "libcrypto_1_1.so", crypto_elf_path,
                         sizeof(crypto_elf_path));
    }
    if (ssl.path[0]) {
        snprintf(ssl_elf_path, sizeof(ssl_elf_path), "%s", ssl.path);
    } else {
        np_pick_elf_path(dirs, ndirs, "libssl_1_1.so", ssl_elf_path,
                         sizeof(ssl_elf_path));
    }
    if (custom.path[0]) {
        snprintf(custom_elf_path, sizeof(custom_elf_path), "%s", custom.path);
    } else {
        np_pick_elf_path(dirs, ndirs, "libcustom_rules.so", custom_elf_path,
                         sizeof(custom_elf_path));
    }
    np_elf_parse(crypto_elf_path, &crypto_elf);
    np_elf_parse(ssl_elf_path, &ssl_elf);
    np_elf_parse(custom_elf_path, &custom_elf);
    np_emit_soname_log("libcrypto_1_1.so", &crypto_elf);
    np_emit_soname_log("libssl_1_1.so", &ssl_elf);
    np_emit_soname_log("libcustom_rules.so", &custom_elf);

    /* 4) 仅查询 4 个 Java_ 导出是否存在，绝不调用。 */
    for (i = 0; i < (int)(sizeof(NP_EXPORTS) / sizeof(NP_EXPORTS[0])); ++i) {
        export_found[i] = np_dlsym_exists(custom.handle, NP_EXPORTS[i],
                                          export_error[i], sizeof(export_error[i]));
        np_log("NATIVE_PROBE export name=%s found=%d", NP_EXPORTS[i], export_found[i]);
    }

    /* 5) 生成 JSON。 */
    np_buf_puts(&g_report, "{\"probe\":\"sdk-native-probe\",\"schema\":1,\"started_unix\":");
    np_buf_printf(&g_report, "%lld", (long long)time(NULL));
    np_buf_puts(&g_report, ",\"abi\":\"arm64-v8a\",\"policy\":\"dlopen/dlsym only; Java_* exports are never called\",\"directories\":[");
    for (i = 0; i < ndirs; ++i) {
        if (i) np_buf_puts(&g_report, ",");
        np_json_str(&g_report, dirs[i].dir);
    }
    np_buf_puts(&g_report, "],\"preferred_directory\":");
    np_json_str(&g_report, preferred_dir ? preferred_dir : "");
    np_buf_puts(&g_report, ",\"libraries\":[");

    np_buf_puts(&g_report, "{\"name\":\"libcrypto_1_1.so\",\"role\":\"preload-global\",\"handle_ok\":");
    np_buf_puts(&g_report, crypto.handle ? "true" : "false");
    np_buf_puts(&g_report, ",\"path\":");
    np_json_str(&g_report, crypto.path);
    np_buf_puts(&g_report, ",\"error\":");
    np_json_str(&g_report, crypto.error);
    np_buf_puts(&g_report, ",\"attempts\":");
    np_buf_append(&g_report, attempts_crypto.data ? attempts_crypto.data : "[]",
                  attempts_crypto.data ? attempts_crypto.len : 2);
    np_buf_puts(&g_report, ",\"elf_path\":");
    np_json_str(&g_report, crypto_elf_path);
    np_buf_puts(&g_report, ",\"elf\":");
    np_json_elf(&g_report, &crypto_elf);
    np_buf_puts(&g_report, "},");

    np_buf_puts(&g_report, "{\"name\":\"libssl_1_1.so\",\"role\":\"preload-global\",\"handle_ok\":");
    np_buf_puts(&g_report, ssl.handle ? "true" : "false");
    np_buf_puts(&g_report, ",\"path\":");
    np_json_str(&g_report, ssl.path);
    np_buf_puts(&g_report, ",\"error\":");
    np_json_str(&g_report, ssl.error);
    np_buf_puts(&g_report, ",\"attempts\":");
    np_buf_append(&g_report, attempts_ssl.data ? attempts_ssl.data : "[]",
                  attempts_ssl.data ? attempts_ssl.len : 2);
    np_buf_puts(&g_report, ",\"elf_path\":");
    np_json_str(&g_report, ssl_elf_path);
    np_buf_puts(&g_report, ",\"elf\":");
    np_json_elf(&g_report, &ssl_elf);
    np_buf_puts(&g_report, "},");

    np_buf_puts(&g_report, "{\"name\":\"libcustom_rules.so\",\"role\":\"target-local\",\"handle_ok\":");
    np_buf_puts(&g_report, custom.handle ? "true" : "false");
    np_buf_puts(&g_report, ",\"path\":");
    np_json_str(&g_report, custom.path);
    np_buf_puts(&g_report, ",\"error\":");
    np_json_str(&g_report, custom.error);
    np_buf_puts(&g_report, ",\"attempts\":");
    np_buf_append(&g_report, attempts_custom.data ? attempts_custom.data : "[]",
                  attempts_custom.data ? attempts_custom.len : 2);
    np_buf_puts(&g_report, ",\"elf_path\":");
    np_json_str(&g_report, custom_elf_path);
    np_buf_puts(&g_report, ",\"elf\":");
    np_json_elf(&g_report, &custom_elf);
    np_buf_puts(&g_report, "}],\"exports\":[");
    for (i = 0; i < (int)(sizeof(NP_EXPORTS) / sizeof(NP_EXPORTS[0])); ++i) {
        if (i) np_buf_puts(&g_report, ",");
        np_buf_puts(&g_report, "{\"name\":");
        np_json_str(&g_report, NP_EXPORTS[i]);
        np_buf_puts(&g_report, ",\"found\":");
        np_buf_puts(&g_report, export_found[i] ? "true" : "false");
        np_buf_puts(&g_report, ",\"error\":");
        np_json_str(&g_report, export_error[i]);
        np_buf_puts(&g_report, ",\"called\":false}");
    }
    np_buf_puts(&g_report, "],\"loaded_count\":");
    np_buf_printf(&g_report, "%d", loaded);
    np_buf_puts(&g_report, ",\"custom_rules_loaded\":");
    np_buf_puts(&g_report, custom.handle ? "true" : "false");
    np_buf_puts(&g_report, ",\"jni_callable\":\"unknown-not-invoked\",\"notes\":[");
    np_buf_puts(&g_report, "\"dlopen 会按 ELF 语义执行库自身构造函数；本探针不额外调用任何导出。\",");
    np_buf_puts(&g_report, "\"dlopen 成功不代表 JNI 可用；本探针未调用任何 Java_ 导出。\",");
    np_buf_puts(&g_report, "\"加载成功的 handle 故意不 dlclose。\",");
    np_buf_puts(&g_report, "\"RTLD_NOW 会解析全部未定义符号，首个缺失依赖/符号见 dlerror 原文。\"");
    np_buf_puts(&g_report, "]}");

    np_log("NATIVE_PROBE stage=done loaded=%d/3 custom_rules=%d", loaded,
           custom.handle ? 1 : 0);

    free(attempts_crypto.data);
    free(attempts_ssl.data);
    free(attempts_custom.data);
    g_running = 0;
    pthread_mutex_unlock(&g_report_lock);
    return 0;
}

int sdk_native_probe_run(void) {
    return np_run_internal(NULL);
}

int sdk_native_probe_run_with_dir(const char *preferred_dir) {
    return np_run_internal(preferred_dir);
}

const char *sdk_native_probe_json(void) {
    const char *result;
    pthread_mutex_lock(&g_report_lock);
    result = (g_report.data && g_report.data[0]) ? g_report.data : "{}";
    pthread_mutex_unlock(&g_report_lock);
    return result;
}
