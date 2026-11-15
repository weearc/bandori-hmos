#ifndef NATIVE_JNI_NEXT_JNI_BRIDGE_H
#define NATIVE_JNI_NEXT_JNI_BRIDGE_H

/*
 * 自有 JNI 1.6 桥的公共接口。
 *
 * 设计边界：
 *   - 本桥只通过公开 dvm_* API 和少量 dvm_internal.h 中的结构/辅助函数工作；
 *     不修改 dvm.c，也不依赖原 libcustom_rules.so。
 *   - jobject/jclass 是桥内 64 位 token，不是 dvm_ref；原生代码无法用伪造
 *     数值直接访问 DVM 堆。
 *   - call_native hook 负责参数/返回值转换、local frame、GIL 释放与异常交接。
 *   - 所有需要在 DVM 上执行的槽都会确保当前线程持有 GIL；已经持有时不重复
 *     释放。
 *
 * 接线要求（由主代理完成）：
 *   1. dvm_create() 返回后、同一线程仍持 GIL 时调用
 *      jni_bridge_init(vm) 和 jni_bridge_install_hooks(vm)；
 *   2. dvm_destroy() 之前调用 jni_bridge_shutdown(vm)；
 *   3. 桥的 shutdown 会恢复安装前的 hooks，并释放全部 pin/句柄。
 */

#include "jni_abi.h"
#include "jni_handles.h"
#include "jni_util.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dvm/dvm.h"

#ifdef __cplusplus
extern "C" {
#endif

struct jni_bridge;

/* --- 生命周期 ---------------------------------------------------------- */

bool jni_bridge_init(struct dvm *vm);
bool jni_bridge_install_hooks(struct dvm *vm);
void jni_bridge_shutdown(struct dvm *vm);
struct jni_bridge *jni_bridge_for_vm(struct dvm *vm);

/* --- 线程/JNIEnv/JavaVM ------------------------------------------------ */

/* 当前线程的 JNIEnv；必要时 attach。返回的指针指向 per-thread 结构，
 * 其首字段是 JNINativeInterface_ 函数表。 */
JNIEnv *jni_bridge_env(struct jni_bridge *b);
JavaVM *jni_bridge_java_vm(struct jni_bridge *b);
struct dvm *jni_bridge_vm(struct jni_bridge *b);

bool jni_bridge_attach_current(struct jni_bridge *b, bool daemon, JNIEnv **out);
bool jni_bridge_detach_current(struct jni_bridge *b);
bool jni_bridge_thread_is_attached(struct jni_bridge *b);

/* --- GIL 辅助（供 native loader 的 JNI_OnLoad 使用） -------------------- */

/* 若当前线程持有 GIL，则 unlock_all 并返回原深度；否则返回 0。 */
unsigned jni_bridge_gil_drop(struct jni_bridge *b);
/* 仅当 depth != 0 时 relock。 */
void jni_bridge_gil_restore(struct jni_bridge *b, unsigned depth);

/* --- 引用/类辅助（供 native loader 与槽实现共用） ---------------------- */

bool jni_bridge_resolve_object(struct jni_bridge *b, jobject token,
                               jni_ref_kind accept, dvm_ref *out_ref);
bool jni_bridge_resolve_class(struct jni_bridge *b, jclass cls,
                              struct dvm_class **out_cls);

/* --- 受控 env 入口（JNI 原生函数参数校验） -----------------------------
 *
 * jni_bridge_for_env() 只比较地址：env 必须等于本线程 TLS 中已 attach 的
 * JNIEnv，绝不先解引用传入指针。其他线程的 env、未 attach 线程、任意指针
 * 都返回 NULL。用于 ANativeWindow_fromSurface/toSurface 等公开 ABI 入口。 */
struct jni_bridge *jni_bridge_for_env(JNIEnv *env);
bool jni_bridge_resolve_object_for_env(JNIEnv *env, jobject token,
                                       jni_ref_kind accept, dvm_ref *out_ref,
                                       struct jni_bridge **out_bridge);
/* 在当前线程已 attach 的前提下，用本桥的 local frame 创建 guest 对象的
 * local 引用；不 attach、不 push frame，失败返回 jni_token_null()。 */
jobject jni_bridge_new_local_object_for_env(JNIEnv *env, dvm_ref ref);
bool jni_bridge_class_name(struct jni_bridge *b, jclass cls, char *out,
                           size_t out_size);
bool jni_bridge_validate_native(struct jni_bridge *b, jclass cls,
                                const char *name, const char *sig);

/* 当前线程是否有 pending 异常。 */
bool jni_bridge_pending(struct jni_bridge *b);
/* 在桥内设置 pending 异常；class_name 使用 JNI 斜杠形式。成功返回 true。 */
bool jni_bridge_throw_new(struct jni_bridge *b, const char *class_name,
                          const char *message);

/* --- 诊断 -------------------------------------------------------------- */

void jni_bridge_note_limited(struct jni_bridge *b, int slot, const char *name);
uint32_t jni_bridge_limited_count(struct jni_bridge *b);
void jni_bridge_set_last_error(struct jni_bridge *b, const char *fmt, ...);
const char *jni_bridge_last_error(struct jni_bridge *b);

/* --- hooks（安装到 struct dvm_hooks） ---------------------------------- */

bool jni_bridge_call_native(void *user, struct dvm *vm,
                            const char *class_name, const char *method,
                            const char *sig, bool is_static, dvm_ref self,
                            const union dvm_value *args, int nargs,
                            union dvm_value *out);
dvm_ref jni_bridge_take_pending_exception(void *user, struct dvm *vm);
bool jni_bridge_load_library(void *user, struct dvm *vm, const char *name);
/* Thread-scoped observer of actual native dispatch, around native_call_words. */
typedef int (*jni_native_observer)(const char *class_name,const char *method,const char *sig,
 dvm_ref self,const union dvm_value *args,int nargs,int entering,uintptr_t function_address,void *userdata);
typedef int (*jni_native_observed_body)(void *userdata);
int jni_bridge_with_native_observer(jni_native_observer observer,void *userdata,
 jni_native_observed_body body,void *bodydata);
/* Diagnostic queries expose no raw function pointers or VM-owned storage. */
bool jni_bridge_native_registered(struct dvm *vm, const char *class_name,
 const char *method, const char *sig);
uint32_t jni_bridge_native_registration_count(struct dvm *vm);

#ifdef __cplusplus
}
#endif

#endif /* NATIVE_JNI_NEXT_JNI_BRIDGE_H */
