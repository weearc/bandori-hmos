#ifndef NATIVE_JNI_NEXT_NATIVE_LOADER_H
#define NATIVE_JNI_NEXT_NATIVE_LOADER_H

/*
 * 原生库加载与 native 方法解析。
 *
 * 所有权：
 *   - 每个 jni_native_state 拥有自己 dlopen 的库句柄；
 *   - RegisterNatives 条目引用所属库（如果能在已加载库中找到符号），
 *     jni_native_state_destroy 先清注册表再 dlclose；
 *   - 加载失败保留 dlerror()/JNI_OnLoad 错误原文，不伪造成功；
 *   - 本模块不自动调用任何 Java_ 函数，只提供 register/find。
 */

#include "jni_abi.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct jni_bridge;
struct jni_native_state;

struct jni_native_state *jni_native_state_create(struct jni_bridge *bridge);
void jni_native_state_destroy(struct jni_native_state *state);

/* System.loadLibrary 语义：先尝试 lib<name>.so，再按原始 name。 */
bool jni_native_load_name(struct jni_native_state *state, const char *name);
/* 显式路径 dlopen；用于测试时加载自有探针库。 */
bool jni_native_load_path(struct jni_native_state *state, const char *path);

/* 注册表精确匹配优先，然后按 Java_ 长名、短名在已加载库中查找。
 * 找不到返回 NULL，不设置 Java 异常；调用方负责设置 UnsatisfiedLinkError。 */
void *jni_native_find(struct jni_native_state *state, const char *class_name,
                      const char *method, const char *sig);

/* 两阶段注册：全部校验通过才写入；失败不部分提交。 */
bool jni_native_register(struct jni_native_state *state, jclass cls,
                         const JNINativeMethod *methods, jint count);
bool jni_native_unregister(struct jni_native_state *state, jclass cls);

const char *jni_native_last_error(struct jni_native_state *state);
uint32_t jni_native_library_count(struct jni_native_state *state);
uint32_t jni_native_registration_count(struct jni_native_state *state);
/* Exact RegisterNatives-table query; never falls back to exported symbols. */
bool jni_native_is_registered(struct jni_native_state *state,
 const char *class_name, const char *method, const char *sig);

#ifdef __cplusplus
}
#endif

#endif /* NATIVE_JNI_NEXT_NATIVE_LOADER_H */
