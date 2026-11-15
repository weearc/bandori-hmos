/*
 * 最小自定义 ELF64 加载器（仅 AArch64 / OHOS）。
 *
 * 目的：绕过 OHOS 系统加载器的符号版本限制，真实 mmap 并重定位原 Android
 * 库。它不是伪造的成功接口：所有失败都会保留首个失败阶段/符号名。
 *
 * 本头文件只暴露不透明句柄与最小 API；内部结构、固定容量和锁都在
 * elf_loader.c 中。所有注释使用简体中文。
 */
#ifndef ELF_LOADER_H
#define ELF_LOADER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 不透明加载器句柄。同一路径重复 open 返回同一句柄并增加引用计数。 */
typedef struct elf_loader elf_loader_t;

/*
 * 打开并加载一个 ELF64 AArch64 ET_DYN 库。
 *
 * 成功返回非 NULL 句柄；失败返回 NULL，并把首个失败阶段/符号写入 err。
 * 该函数会递归加载 HAP 库目录内的 DT_NEEDED；系统库交给系统 dlopen。
 * 调用方不得在未持有有效路径时调用。
 */
elf_loader_t *elf_loader_open(const char *path, char *err, size_t errlen);
/* Strict nested dlopen: run constructors for new loads, reject cached graphs
 * whose required constructors were skipped or which use placeholder stdio. */
elf_loader_t *elf_loader_open_initialized(const char *path, char *err, size_t errlen);
/* index SIZE_MAX denotes DT_INIT; otherwise it is the DT_INIT_ARRAY index.
 * Observer returns zero on success. A nonzero result aborts the load; after
 * an entered constructor this cannot undo its side effects. The observer is
 * scoped to this thread and inherited by recursive dependency loads. */
typedef int (*elf_loader_init_observer)(const char *path, size_t index,
                                        uintptr_t address, int entering,
                                        void *userdata);
typedef int (*elf_loader_observed_body)(void *userdata);
int elf_loader_with_init_observer(elf_loader_init_observer observer, void *userdata,
                                  elf_loader_observed_body body, void *bodydata);

elf_loader_t *elf_loader_open_initialized_traced(const char *path, char *err,
    size_t errlen, elf_loader_init_observer observer, void *userdata);

/* Search only this object, its dependencies and compatible system/shim scope. */
void *elf_loader_symbol_scoped(elf_loader_t *loader, const char *name);
/* Own dynamic export only, and only an ordinary executable function. */
void *elf_loader_own_executable_symbol(elf_loader_t *loader, const char *name);
/* Acquire an already initialized cached graph without mapping or executing. */
elf_loader_t *elf_loader_acquire_initialized_cached(const char *path, char *err, size_t errlen);
/* Read actual relocated GLOB_DAT/JUMP_SLOT cells, not a fresh symbol lookup.
 * 1=consistent target found (possibly NULL for weak), 0=absent, -1=invalid. */
int elf_loader_import_target(elf_loader_t *loader, const char *name, void **target);
/* Overrides whose guest object layouts cannot be passed to the host libc. */
void *elf_loader_android_abi_symbol(const char *name);


/*
 * 在已加载的自定义库中按名字查找符号（忽略符号版本）。
 *
 * 查找范围：该库及其 DT_NEEDED 闭包、依赖的系统句柄、内置 Bionic shim。
 * 不搜索无关的自定义 LOCAL 库。未找到返回 NULL。
 */
void *elf_loader_symbol(elf_loader_t *loader, const char *name);

/*
 * 返回该加载器的诊断状态字符串，永不返回 NULL。
 * 内容包括路径、SONAME、load_bias、依赖、重定位统计、init_skipped、
 * __sF 占位使用情况和系统依赖缺失计数。
 */
const char *elf_loader_status(elf_loader_t *loader);

/*
 * 释放调用方引用。
 *
 * 出于安全考虑，映射默认保留到进程结束：原库可能已经执行构造器、注册过
 * 线程回调或 JNI 函数指针，立即 munmap 会让这些指针悬空。返回 0 表示
 * 引用计数成功递减；-1 表示句柄非法。
 */
int elf_loader_close(elf_loader_t *loader);

/* 读取最近一次失败文本；返回写入长度（不含结尾 NUL）。 */
size_t elf_loader_last_error(elf_loader_t *loader, char *buf, size_t len);

/* 供诊断代码识别空/非法句柄。 */
int elf_loader_is_valid(elf_loader_t *loader);

#ifdef __cplusplus
}
#endif

#endif /* ELF_LOADER_H */
