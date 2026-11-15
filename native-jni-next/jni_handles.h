#ifndef NATIVE_JNI_NEXT_JNI_HANDLES_H
#define NATIVE_JNI_NEXT_JNI_HANDLES_H

/*
 * jobject token 句柄表。
 *
 * 设计要点：
 *   - 原生代码拿到的 jobject 不是 dvm_ref，而是 64 位 token：
 *       bits 0..31  槽位下标 + 1
 *       bits 32..55 24 位 generation
 *       bits 56..63 8 位 vm_tag
 *   - 通过 generation/vm_tag/kind 校验拒绝伪造、已释放、跨 VM 的 token。
 *   - local/global 引用在表内对应一次 pin；weak 不 pin；class token 只保存
 *     类指针（DVM 类在 VM 生命周期内稳定）。
 *   - local 引用按 frame 管理；native 调用边界必须 pop 本层 frame。
 *
 * 本模块通过 pin/unpin 回调与 DVM 解耦，host selftest 可用 fake 计数验证。
 */

#include "jni_abi.h"
#include "jni_util.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define JNI_HANDLE_DEFAULT_CAPACITY 4096u
#define JNI_HANDLE_MAX_FRAMES       64u

typedef enum jni_ref_kind {
    JNI_REF_NONE = 0,
    JNI_REF_LOCAL,
    JNI_REF_GLOBAL,
    JNI_REF_WEAK,
    JNI_REF_CLASS
} jni_ref_kind;

typedef bool (*jni_handle_pin_fn)(void *ctx, uint32_t object_ref);
typedef void (*jni_handle_unpin_fn)(void *ctx, uint32_t object_ref);
/* Optional live guest identity query. Zero means dead/invalid. */
typedef uint64_t (*jni_handle_identity_fn)(void *ctx,uint32_t object_ref);

typedef struct jni_handle_callbacks {
    void *ctx;
    jni_handle_pin_fn pin;
    jni_handle_unpin_fn unpin;
    jni_handle_identity_fn identity;
} jni_handle_callbacks;

typedef struct jni_handle_slot {
    uint32_t object_ref;      /* dvm_ref；class token 为 0 */
    uint64_t object_identity; /* distinct from the JNI slot generation */
    void *cls;                /* class token 的 struct dvm_class*，其他为 NULL */
    uint32_t generation;      /* 24 位有效 */
    uint32_t frame_id;        /* local 所属 frame；global/weak/class 为 0 */
    uint8_t vm_tag;
    uint8_t kind;             /* jni_ref_kind */
    bool live;
    bool pinned;
} jni_handle_slot;

typedef struct jni_handle_table {
    jni_handle_slot *slots;
    uint32_t capacity;
    uint32_t next_hint;
    uint32_t live_count;
    uint32_t pinned_count;
    uint8_t vm_tag;
    jni_handle_callbacks cb;
    uint32_t frame_stack[JNI_HANDLE_MAX_FRAMES];
    uint32_t frame_depth;
    uint32_t frame_counter;
} jni_handle_table;

bool jni_handles_init(jni_handle_table *table, uint32_t capacity,
                      uint8_t vm_tag, const jni_handle_callbacks *cb);
void jni_handles_shutdown(jni_handle_table *table);

jobject jni_handle_new_local(jni_handle_table *table, uint32_t object_ref);
jobject jni_handle_new_global(jni_handle_table *table, uint32_t object_ref);
jobject jni_handle_new_weak(jni_handle_table *table, uint32_t object_ref);
jobject jni_handle_new_class(jni_handle_table *table, void *cls);

/* 显式 frame 版本：桥在每个 attached 线程内维护自己的 frame 栈，
 * 因此不依赖句柄表内那份仅供单线程 selftest 使用的栈。 */
jobject jni_handle_new_local_in_frame(jni_handle_table *table,
                                      uint32_t object_ref, uint32_t frame_id);
jobject jni_handle_new_class_in_frame(jni_handle_table *table, void *cls,
                                      uint32_t frame_id);
bool jni_handle_delete_local_in_frame(jni_handle_table *table, jobject token,
                                      uint32_t current_frame);
/* 释放该 frame 的全部 local；调用方负责保证嵌套 frame 已先释放。 */
void jni_handle_pop_frame_slots(jni_handle_table *table, uint32_t frame_id);
jobject jni_handle_promote_to_frame(jni_handle_table *table, jobject token,
                                    uint32_t target_frame);

/* 解析对象引用；accept 可以是 JNI_REF_LOCAL/GLOBAL/WEAK，actual 可为 NULL。 */
bool jni_handle_resolve_object(jni_handle_table *table, jobject token,
                               jni_ref_kind accept, uint32_t *object_ref,
                               jni_ref_kind *actual);
bool jni_handle_resolve_class(jni_handle_table *table, jobject token,
                              void **cls);
bool jni_handle_resolve_any(jni_handle_table *table, jobject token,
                            uint32_t *object_ref, void **cls,
                            jni_ref_kind *actual);

bool jni_handle_delete_local(jni_handle_table *table, jobject token);
/* JNI DeleteLocalRef 允许删除任意仍存活的 local（包括 native 参数所在
 * 外层 frame），因此桥使用这个不限制当前 frame 的版本。 */
bool jni_handle_delete_local_any(jni_handle_table *table, jobject token);
bool jni_handle_delete_global(jni_handle_table *table, jobject token);
bool jni_handle_delete_weak(jni_handle_table *table, jobject token);
bool jni_handle_delete(jni_handle_table *table, jobject token);

jobject jni_handle_new_local_ref(jni_handle_table *table, jobject token);
jobject jni_handle_new_global_ref(jni_handle_table *table, jobject token);
jboolean jni_handle_is_same(jni_handle_table *table, jobject a, jobject b);
jobjectRefType jni_handle_ref_type(jni_handle_table *table, jobject token);

/* local frame：push 返回 frame_id，0 表示失败；pop 必须 LIFO。 */
uint32_t jni_handle_frame_push(jni_handle_table *table);
bool jni_handle_frame_pop(jni_handle_table *table, uint32_t frame_id);
/* PopLocalFrame 语义：result 若属于被 pop 的 frame，则在父 frame 生成一个
 * 新 local 引用；*out 可为 NULL。 */
bool jni_handle_frame_pop_with_result(jni_handle_table *table, uint32_t frame_id,
                                      jobject result, jobject *out);

uint32_t jni_handle_live_count(const jni_handle_table *table);
uint32_t jni_handle_pinned_count(const jni_handle_table *table);
uint32_t jni_handle_current_frame(const jni_handle_table *table);

#ifdef __cplusplus
}
#endif

#endif /* NATIVE_JNI_NEXT_JNI_HANDLES_H */
