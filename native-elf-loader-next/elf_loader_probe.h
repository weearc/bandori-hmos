/*
 * 自定义 ELF 加载器的运行探针（简体中文注释）。
 *
 * 探针只做真实加载与符号查找，不调用任何 Java_ 函数、不伪造成功。
 */
#ifndef ELF_LOADER_PROBE_H
#define ELF_LOADER_PROBE_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 依次真实加载 libcrypto_1_1.so、libssl_1_1.so、libcustom_rules.so、
 * libblkv.so，并只查找 libcustom_rules 的四个 Java_ 导出。
 *
 * dir 为 HAP 原生库目录；传 NULL 或空串时使用
 * /data/storage/el1/bundle/libs/arm64。
 *
 * 返回：
 *   0  —— 四个库均加载成功，且四个 Java_ 导出均存在；
 *   1  —— 至少一个库加载失败或至少一个导出缺失；
 *  -1  —— 重入、目录非法或内部忙。
 */
int elf_loader_probe_run(const char *dir);

/*
 * 返回最近一次探针的 JSON 文本（静态缓冲区，调用方不得 free）。
 * 返回指针在同一次运行内有效；下一次 probe_run 可能覆盖它。
 */
const char *elf_loader_probe_json(void);

#ifdef __cplusplus
}
#endif

#endif /* ELF_LOADER_PROBE_H */
