/*
 * JNI 纯逻辑实现。禁止在本文件调用 DVM 或任何全局可变状态，
 * 以便 selftest 可以在没有 DVM/设备的环境里直接验证。
 */

#include "jni_util.h"

#include <stdio.h>
#include <string.h>

static bool jni_prim_kind(char c, jni_kind *out)
{
    switch (c) {
    case 'Z': *out = JNI_KIND_BOOLEAN; return true;
    case 'B': *out = JNI_KIND_BYTE;    return true;
    case 'C': *out = JNI_KIND_CHAR;    return true;
    case 'S': *out = JNI_KIND_SHORT;   return true;
    case 'I': *out = JNI_KIND_INT;     return true;
    case 'J': *out = JNI_KIND_LONG;    return true;
    case 'F': *out = JNI_KIND_FLOAT;   return true;
    case 'D': *out = JNI_KIND_DOUBLE;  return true;
    default: return false;
    }
}

jobject jni_token_null(void)
{
    return (jobject)0;
}

bool jni_token_is_null(jobject token)
{
    return (uintptr_t)token == 0;
}

uint64_t jni_token_raw(jobject token)
{
    return (uint64_t)(uintptr_t)token;
}

bool jni_token_encode(uint32_t index, uint32_t generation, uint8_t vm_tag,
                      jobject *out)
{
    if (!out) return false;
    if (sizeof(uintptr_t) < 8 || sizeof(uint64_t) < 8) {
        *out = jni_token_null();
        return false;
    }
    if (index == UINT32_MAX) { /* index+1 不能溢出 */
        *out = jni_token_null();
        return false;
    }
    uint64_t raw = ((uint64_t)(index + 1u) & JNI_TOKEN_INDEX_MASK) |
                   (((uint64_t)generation & JNI_TOKEN_GEN_MASK) << JNI_TOKEN_INDEX_BITS) |
                   (((uint64_t)vm_tag & JNI_TOKEN_VM_MASK) <<
                    (JNI_TOKEN_INDEX_BITS + JNI_TOKEN_GEN_BITS));
    *out = (jobject)(uintptr_t)raw;
    return true;
}

bool jni_token_decode(jobject token, jni_token_parts *out)
{
    if (!out) return false;
    uint64_t raw = (uint64_t)(uintptr_t)token;
    if (raw == 0) {
        out->index = 0;
        out->generation = 0;
        out->vm_tag = 0;
        return false;
    }
    uint32_t low = (uint32_t)(raw & JNI_TOKEN_INDEX_MASK);
    if (low == 0) return false;
    out->index = low - 1u;
    out->generation = (uint32_t)((raw >> JNI_TOKEN_INDEX_BITS) & JNI_TOKEN_GEN_MASK);
    out->vm_tag = (uint8_t)((raw >> (JNI_TOKEN_INDEX_BITS + JNI_TOKEN_GEN_BITS)) &
                            JNI_TOKEN_VM_MASK);
    return true;
}

static bool parse_type(const char **cursor, jni_kind *kind,
                       const char **desc, size_t *desc_len)
{
    const char *p = *cursor;
    const char *start = p;
    if (!p || !*p) return false;

    if (*p == '[') {
        while (*p == '[') ++p;
        if (*p == 'L') {
            ++p;
            const char *name = p;
            while (*p && *p != ';') ++p;
            if (p == name || *p != ';') return false;
            ++p;
        } else if (*p && strchr("ZBCSIJFD", *p)) {
            ++p;
        } else {
            return false;
        }
        *kind = JNI_KIND_ARRAY;
        *desc = start;
        *desc_len = (size_t)(p - start);
        *cursor = p;
        return true;
    }

    if (*p == 'L') {
        ++p;
        const char *name = p;
        while (*p && *p != ';') ++p;
        if (p == name || *p != ';') return false;
        ++p;
        *kind = JNI_KIND_OBJECT;
        *desc = start;
        *desc_len = (size_t)(p - start);
        *cursor = p;
        return true;
    }

    if (*p == 'V') {
        *kind = JNI_KIND_VOID;
        *desc = start;
        *desc_len = 1;
        *cursor = p + 1;
        return true;
    }

    if (jni_prim_kind(*p, kind)) {
        *desc = start;
        *desc_len = 1;
        *cursor = p + 1;
        return true;
    }
    return false;
}

bool jni_sig_parse(const char *sig, jni_sig_info *out)
{
    if (!sig || !out || sig[0] != '(') return false;
    memset(out, 0, sizeof(*out));
    out->ret = JNI_KIND_INVALID;

    const char *p = sig + 1;
    while (*p && *p != ')') {
        if (out->nargs >= JNI_MAX_SIG_ARGS) return false;
        jni_kind kind;
        const char *desc;
        size_t len;
        if (!parse_type(&p, &kind, &desc, &len)) return false;
        if (kind == JNI_KIND_VOID) return false;
        out->args[out->nargs].kind = kind;
        out->args[out->nargs].desc = desc;
        out->args[out->nargs].desc_len = len;
        ++out->nargs;
    }
    if (*p != ')') return false;
    ++p;
    if (!*p) return false;

    jni_kind ret_kind;
    const char *ret_desc;
    size_t ret_len;
    if (!parse_type(&p, &ret_kind, &ret_desc, &ret_len)) return false;
    if (*p != '\0') return false;
    out->ret = ret_kind;
    out->ret_desc = ret_desc;
    out->ret_desc_len = ret_len;
    return true;
}

const char *jni_kind_name(jni_kind kind)
{
    switch (kind) {
    case JNI_KIND_VOID:    return "void";
    case JNI_KIND_BOOLEAN: return "boolean";
    case JNI_KIND_BYTE:    return "byte";
    case JNI_KIND_CHAR:    return "char";
    case JNI_KIND_SHORT:   return "short";
    case JNI_KIND_INT:     return "int";
    case JNI_KIND_LONG:    return "long";
    case JNI_KIND_FLOAT:   return "float";
    case JNI_KIND_DOUBLE:  return "double";
    case JNI_KIND_OBJECT:  return "object";
    case JNI_KIND_ARRAY:   return "array";
    default:               return "invalid";
    }
}

size_t jni_kind_width(jni_kind kind)
{
    switch (kind) {
    case JNI_KIND_BOOLEAN:
    case JNI_KIND_BYTE:    return 1;
    case JNI_KIND_CHAR:
    case JNI_KIND_SHORT:   return 2;
    case JNI_KIND_INT:
    case JNI_KIND_FLOAT:   return 4;
    case JNI_KIND_LONG:
    case JNI_KIND_DOUBLE:  return 8;
    case JNI_KIND_OBJECT:
    case JNI_KIND_ARRAY:   return sizeof(void *);
    default:               return 0;
    }
}

static bool append_range(char *out, size_t out_size, size_t *pos,
                         const char *text, size_t len)
{
    if (!out || out_size == 0 || !pos) return false;
    if (*pos > out_size - 1) return false;
    if (len > out_size - 1 - *pos) return false;
    if (len) memcpy(out + *pos, text, len);
    *pos += len;
    out[*pos] = '\0';
    return true;
}

static bool append_cstr(char *out, size_t out_size, size_t *pos, const char *text)
{
    return append_range(out, out_size, pos, text, strlen(text));
}

static bool mangle_range(const char *in, size_t len, char *out,
                         size_t out_size, size_t *pos)
{
    if (!in && len) return false;
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)in[i];
        char tmp[8];
        size_t tmp_len;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9')) {
            tmp[0] = (char)c;
            tmp_len = 1;
        } else if (c == '/' || c == '.') {
            tmp[0] = '_';
            tmp_len = 1;
        } else if (c == '_') {
            tmp[0] = '_'; tmp[1] = '1'; tmp_len = 2;
        } else if (c == ';') {
            tmp[0] = '_'; tmp[1] = '2'; tmp_len = 2;
        } else if (c == '[') {
            tmp[0] = '_'; tmp[1] = '3'; tmp_len = 2;
        } else {
            int n = snprintf(tmp, sizeof(tmp), "_0%04x", c);
            if (n < 0 || (size_t)n >= sizeof(tmp)) return false;
            tmp_len = (size_t)n;
        }
        if (!append_range(out, out_size, pos, tmp, tmp_len)) return false;
    }
    return true;
}

bool jni_mangle_ident(const char *in, char *out, size_t out_size)
{
    if (!out || out_size == 0) return false;
    out[0] = '\0';
    if (!in) return false;
    size_t len = 0;
    while (in[len]) {
        if (len > 65535u) { /* 防御无 NUL 的调用方输入 */
            return false;
        }
        ++len;
    }
    size_t pos = 0;
    if (!mangle_range(in, len, out, out_size, &pos)) {
        out[0] = '\0';
        return false;
    }
    return true;
}

bool jni_mangle_sig_args(const char *sig, char *out, size_t out_size)
{
    if (!out || out_size == 0) return false;
    out[0] = '\0';
    jni_sig_info info;
    if (!jni_sig_parse(sig, &info)) return false;
    size_t pos = 0;
    for (int i = 0; i < info.nargs; ++i) {
        const jni_sig_arg *a = &info.args[i];
        if (!mangle_range(a->desc, a->desc_len, out, out_size, &pos)) {
            out[0] = '\0';
            return false;
        }
    }
    return true;
}

bool jni_java_symbol_short(const char *class_name, const char *method_name,
                           char *out, size_t out_size)
{
    if (!class_name || !method_name || !out || out_size == 0) return false;
    out[0] = '\0';
    size_t pos = 0;
    if (!append_cstr(out, out_size, &pos, "Java_")) goto fail;
    if (!jni_mangle_ident(class_name, out + pos, out_size - pos)) goto fail;
    pos = strlen(out);
    if (!append_range(out, out_size, &pos, "_", 1)) goto fail;
    {
        char method[JNI_MANGLE_MAX];
        if (!jni_mangle_ident(method_name, method, sizeof(method))) goto fail;
        if (!append_cstr(out, out_size, &pos, method)) goto fail;
    }
    return true;
fail:
    out[0] = '\0';
    return false;
}

bool jni_java_symbol_long(const char *class_name, const char *method_name,
                          const char *sig, char *out, size_t out_size)
{
    if (!sig || !out || out_size == 0) return false;
    if (!jni_java_symbol_short(class_name, method_name, out, out_size)) return false;
    size_t pos = strlen(out);
    if (!append_range(out, out_size, &pos, "__", 2)) goto fail;
    {
        char args[JNI_MANGLE_MAX];
        if (!jni_mangle_sig_args(sig, args, sizeof(args))) goto fail;
        if (!append_cstr(out, out_size, &pos, args)) goto fail;
    }
    return true;
fail:
    out[0] = '\0';
    return false;
}
