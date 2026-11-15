#ifndef SDK_NATIVE_PROBE_H
#define SDK_NATIVE_PROBE_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * R2 原生库“只加载、不调用”诊断探针。
 *
 * 目标：
 *   1. 用纯 C（不依赖 libelf）解析 ELF64 动态段，输出 SONAME、DT_NEEDED、
 *      DT_RPATH/DT_RUNPATH、VERNEED 版本依赖以及未定义动态符号中命中已知
 *      Bionic 符号的数量与名字。
 *   2. 按“先预加载 libcrypto_1_1.so / libssl_1_1.so，再加载
 *      libcustom_rules.so”的顺序，对每个候选目录使用绝对路径 dlopen，
 *      记录每个尝试路径、handle 是否为空以及真实 dlerror 原文。
 *   3. 仅用 dlsym 查询 4 个 Java_com_base_crules_CustomRules_* 导出是否存在，
 *      绝不调用它们，因此本探针不产生 JNI 可用性结论。
 *
 * 约束：
 *   - 不注册信号处理器、不 fork、不修改 LD 配置、不写系统设置。
 *   - 不依赖 LD_LIBRARY_PATH；所有 dlopen 都使用绝对路径。
 *   - 加载成功的 handle 故意不 dlclose，避免诊断期间对象被卸载。
 *   - dlopen 会按 ELF 语义执行库自身的 init_array/DT_INIT 构造函数；这是
 *     “加载”的一部分，本探针不额外调用任何导出（尤其不调用 Java_* 函数）。
 *
 * 目录尝试顺序（固定）：
 *   1. /data/storage/el1/bundle/libs/arm64（HAP 原生库在设备上的标准解压位置）
 *   2. NAPI 调用方传入的可选目录（sdk_native_probe_run_with_dir）
 *   3. 环境变量 NATIVE_PROBE_LIB_DIR
 *
 * 返回：0 表示诊断已执行（即使所有 dlopen 都失败）；负值表示参数/内存等
 * 本机错误。无论返回值如何，sdk_native_probe_json() 都返回最近一次诊断的
 * JSON 文本，供 NAPI 层原样交给 ArkTS。
 */
int sdk_native_probe_run(void);

/* 与 sdk_native_probe_run 相同，但把 NAPI 传入的目录插在标准目录之后。 */
int sdk_native_probe_run_with_dir(const char *preferred_dir);

/* 返回最近一次诊断的 JSON 字符串（进程内静态缓冲区，下一次运行会覆盖）。 */
const char *sdk_native_probe_json(void);

#ifdef __cplusplus
}
#endif

#endif /* SDK_NATIVE_PROBE_H */
