#ifndef NATIVE_JNI_NEXT_JNI_UTIL_H
#define NATIVE_JNI_NEXT_JNI_UTIL_H

/*
 * 与 DVM 解耦的 JNI 纯逻辑：
 *   - 64 位 jobject token 编解码；
 *   - JNI 签名解析；
 *   - Java_ 短名/长名 mangling。
 *
 * 本文件不访问 DVM、不分配 DVM 对象，便于 host selftest 直接覆盖。
 */

#include "jni_abi.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define JNI_TOKEN_INDEX_BITS 32u
#define JNI_TOKEN_GEN_BITS   24u
#define JNI_TOKEN_VM_BITS     8u
#define JNI_TOKEN_INDEX_MASK  ((uint64_t)0xffffffffu)
#define JNI_TOKEN_GEN_MASK    ((uint64_t)0x00ffffffu)
#define JNI_TOKEN_VM_MASK     ((uint64_t)0x000000ffu)

#define JNI_MAX_SIG_ARGS 64
#define JNI_MANGLE_MAX   4096

typedef enum jni_kind {
    JNI_KIND_INVALID = 0,
    JNI_KIND_VOID,
    JNI_KIND_BOOLEAN,
    JNI_KIND_BYTE,
    JNI_KIND_CHAR,
    JNI_KIND_SHORT,
    JNI_KIND_INT,
    JNI_KIND_LONG,
    JNI_KIND_FLOAT,
    JNI_KIND_DOUBLE,
    JNI_KIND_OBJECT,
    JNI_KIND_ARRAY
} jni_kind;

typedef struct jni_sig_arg {
    jni_kind kind;
    const char *desc;   /* 指向原签名内的参数描述，例如 "I" 或 "Ljava/lang/String;" */
    size_t desc_len;
} jni_sig_arg;

typedef struct jni_sig_info {
    int nargs;
    jni_sig_arg args[JNI_MAX_SIG_ARGS];
    jni_kind ret;
    const char *ret_desc;
    size_t ret_desc_len;
} jni_sig_info;

typedef struct jni_token_parts {
    uint32_t index;      /* 句柄表槽位下标，0 基 */
    uint32_t generation; /* 24 位 generation */
    uint8_t vm_tag;      /* 8 位 VM 标识 */
} jni_token_parts;

/* token 0 表示 null，绝不映射到 dvm_ref 0 以外的任何对象。 */
jobject jni_token_null(void);
bool jni_token_is_null(jobject token);
bool jni_token_encode(uint32_t index, uint32_t generation, uint8_t vm_tag,
                      jobject *out);
bool jni_token_decode(jobject token, jni_token_parts *out);
uint64_t jni_token_raw(jobject token);

/* 解析完整 JNI 方法签名，例如 "(ILjava/lang/String;[J)Z"。 */
bool jni_sig_parse(const char *sig, jni_sig_info *out);
const char *jni_kind_name(jni_kind kind);
size_t jni_kind_width(jni_kind kind); /* 数组/对象返回 sizeof(void*) 语义宽度，仅作边界用途 */

/* 标识符 mangling：'/' 或 '.' -> '_'，'_' -> '_1'，';' -> '_2'，
 * '[' -> '_3'，其他非字母数字 -> _0xxxx。 */
bool jni_mangle_ident(const char *in, char *out, size_t out_size);

/* 把签名中的参数编码为 Java_ 长名后缀，例如 "(ILjava/lang/String;)V"
 * -> "ILjava_lang_String_2"。 */
bool jni_mangle_sig_args(const char *sig, char *out, size_t out_size);

bool jni_java_symbol_short(const char *class_name, const char *method_name,
                           char *out, size_t out_size);
bool jni_java_symbol_long(const char *class_name, const char *method_name,
                          const char *sig, char *out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif /* NATIVE_JNI_NEXT_JNI_UTIL_H */
