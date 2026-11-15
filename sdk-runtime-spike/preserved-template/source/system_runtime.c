#include "system_runtime.h"
#include "dvm/dvm_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int sdk_trace_fprintf(FILE *, const char *, ...);

/* android.util.Base64 flags.  NO_CLOSE only affects the streaming API, which
 * this runtime does not expose; it is still an accepted flag value. */
#define B64_DEFAULT 0u
#define B64_NO_PADDING 1u
#define B64_NO_WRAP 2u
#define B64_CRLF 4u
#define B64_URL_SAFE 8u
#define B64_NO_CLOSE 16u
#define B64_VALID_FLAGS (B64_NO_PADDING | B64_NO_WRAP | B64_CRLF | B64_URL_SAFE | B64_NO_CLOSE)

/* Decoder character classes.  Data values are 0..63; the negatives mirror the
 * Android Decoder table, with a distinct class for bytes that Android would
 * silently skip but that this runtime rejects. */
#define B64_SKIP (-1)
#define B64_EQUALS (-2)
#define B64_ERROR (-3)

#define B64_LINE_GROUPS 19u /* 19 output tuples = 76 characters per line */

static const char b64_std_alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static const char b64_url_alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

static struct dvm *active_vm;

static bool is_call(const char *method, const char *signature,
                    const char *wanted_method, const char *wanted_signature)
{
    return method && signature && !strcmp(method, wanted_method) &&
           !strcmp(signature, wanted_signature);
}

/* ------------------------------------------------------------------ */
/* java.lang.System.arraycopy */

/* Java semantics, matching ART's System_arraycopy order:
 *   null source/destination -> NullPointerException
 *   non-array -> ArrayStoreException
 *   negative position/length or out of range -> IndexOutOfBoundsException
 *   primitive component mismatch -> ArrayStoreException
 *   reference elements are checked as they are written -> ArrayStoreException
 * The DVM has no collector and no write barrier: a reference array element is
 * a dvm_ref word and holding it in the array keeps the object alive. */
static bool system_arraycopy(struct dvm *vm, const union dvm_value *args)
{
    dvm_ref src_ref = args[0].l;
    int32_t src_pos = args[1].i;
    dvm_ref dst_ref = args[2].l;
    int32_t dst_pos = args[3].i;
    int32_t count = args[4].i;

    if (!src_ref) {
        dvm__throw(vm, "java/lang/NullPointerException", "src == null");
        return false;
    }
    if (!dst_ref) {
        dvm__throw(vm, "java/lang/NullPointerException", "dst == null");
        return false;
    }

    struct dvm_object *src = dvm__obj(vm, src_ref);
    struct dvm_object *dst = dvm__obj(vm, dst_ref);
    if (!src || src->kind != DVM_OBJ_ARRAY) {
        dvm__throw(vm, "java/lang/ArrayStoreException",
                   "source of type %s is not an array",
                   src && src->cls && src->cls->name ? src->cls->name : "null");
        return false;
    }
    if (!dst || dst->kind != DVM_OBJ_ARRAY) {
        dvm__throw(vm, "java/lang/ArrayStoreException",
                   "destination of type %s is not an array",
                   dst && dst->cls && dst->cls->name ? dst->cls->name : "null");
        return false;
    }

    int64_t src_length = (int64_t)src->length;
    int64_t dst_length = (int64_t)dst->length;
    if (src_pos < 0 || dst_pos < 0 || count < 0 ||
        (int64_t)src_pos + (int64_t)count > src_length ||
        (int64_t)dst_pos + (int64_t)count > dst_length) {
        dvm__throw(vm, "java/lang/IndexOutOfBoundsException",
                   "src.length=%u srcPos=%d dst.length=%u dstPos=%d length=%d",
                   src->length, (int)src_pos, dst->length, (int)dst_pos,
                   (int)count);
        return false;
    }

    bool src_is_ref = src->elem_kind == 'L';
    bool dst_is_ref = dst->elem_kind == 'L';
    if (src_is_ref != dst_is_ref) {
        dvm__throw(vm, "java/lang/ArrayStoreException",
                   "incompatible array types: src=%s dst=%s",
                   src->cls && src->cls->name ? src->cls->name : "?",
                   dst->cls && dst->cls->name ? dst->cls->name : "?");
        return false;
    }
    if (!src_is_ref && src->elem_kind != dst->elem_kind) {
        dvm__throw(vm, "java/lang/ArrayStoreException",
                   "incompatible array types: src=%s dst=%s",
                   src->cls && src->cls->name ? src->cls->name : "?",
                   dst->cls && dst->cls->name ? dst->cls->name : "?");
        return false;
    }

    if (count == 0) return true;

    if (!src_is_ref) {
        int width = dvm__elem_width(src->elem_kind);
        memmove((unsigned char *)dst->data + (size_t)dst_pos * (size_t)width,
                (const unsigned char *)src->data + (size_t)src_pos * (size_t)width,
                (size_t)count * (size_t)width);
        return true;
    }

    uint32_t *src_data = (uint32_t *)src->data;
    uint32_t *dst_data = (uint32_t *)dst->data;
    struct dvm_class *dst_component = dst->cls ? dst->cls->elem : NULL;

    /* When the source array class is assignable to the destination array
     * class every element is already known to fit, so one memmove is enough.
     * This also covers self-copy, where memmove supplies Java's overlap
     * semantics. */
    if (src->cls && dst->cls &&
        dvm__class_assignable(vm, src->cls, dst->cls)) {
        memmove(dst_data + dst_pos, src_data + src_pos,
                (size_t)count * sizeof(uint32_t));
        return true;
    }

    if (!dst_component) {
        dvm__throw(vm, "java/lang/ArrayStoreException",
                   "destination component type is unavailable");
        return false;
    }

    /* Element-by-element store check.  On a mismatch the already written
     * prefix stays in the destination, as with ART's checking memcpy. */
    for (int32_t i = 0; i < count; ++i) {
        dvm_ref value = src_data[src_pos + i];
        if (value) {
            struct dvm_object *object = dvm__obj(vm, value);
            if (!object ||
                !dvm__class_assignable(vm, object->cls, dst_component)) {
                dvm__throw(vm, "java/lang/ArrayStoreException",
                           "incompatible element %s at index %d",
                           object && object->cls && object->cls->name
                               ? object->cls->name
                               : "unknown",
                           (int)i);
                return false;
            }
        }
        dst_data[dst_pos + i] = value;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* android.util.Base64 */

static bool b64_flags_valid(struct dvm *vm, int32_t flags)
{
    if (((uint32_t)flags & ~(uint32_t)B64_VALID_FLAGS) != 0) {
        dvm__throw(vm, "java/lang/IllegalArgumentException",
                   "bad Base64 flags: 0x%08x", (unsigned)flags);
        return false;
    }
    return true;
}

/* Android computes the encoded size from the input length.  Wrapping adds one
 * separator for every 57 input bytes (19 tuples), including a final separator
 * for the last, possibly partial, line. */
static bool b64_encoded_size(uint32_t length, int32_t flags, size_t *size)
{
    size_t chars = (size_t)(length / 3u) * 4u;
    uint32_t remainder = length % 3u;
    if ((flags & B64_NO_PADDING) == 0) {
        if (remainder) chars += 4u;
    } else if (remainder == 1) {
        chars += 2u;
    } else if (remainder == 2) {
        chars += 3u;
    }
    if ((flags & B64_NO_WRAP) == 0 && length > 0) {
        size_t lines = ((size_t)(length - 1u) / 57u) + 1u;
        chars += lines * ((flags & B64_CRLF) ? 2u : 1u);
    }
    if (chars > UINT32_MAX) return false;
    *size = chars;
    return true;
}

static void b64_encode_into(const unsigned char *input, uint32_t length,
                            int32_t flags, unsigned char *output)
{
    const char *alphabet =
        (flags & B64_URL_SAFE) ? b64_url_alphabet : b64_std_alphabet;
    bool padding = (flags & B64_NO_PADDING) == 0;
    bool newline = (flags & B64_NO_WRAP) == 0;
    bool cr = (flags & B64_CRLF) != 0;
    size_t op = 0;
    uint32_t p = 0;
    unsigned count = B64_LINE_GROUPS;

    while (p + 3u <= length) {
        uint32_t v = ((uint32_t)input[p] << 16) |
                     ((uint32_t)input[p + 1] << 8) |
                     (uint32_t)input[p + 2];
        output[op++] = (unsigned char)alphabet[(v >> 18) & 0x3fu];
        output[op++] = (unsigned char)alphabet[(v >> 12) & 0x3fu];
        output[op++] = (unsigned char)alphabet[(v >> 6) & 0x3fu];
        output[op++] = (unsigned char)alphabet[v & 0x3fu];
        p += 3u;
        if (newline && --count == 0) {
            if (cr) output[op++] = '\r';
            output[op++] = '\n';
            count = B64_LINE_GROUPS;
        }
    }

    uint32_t remainder = length - p;
    if (remainder == 1u) {
        uint32_t v = (uint32_t)input[p] << 4;
        output[op++] = (unsigned char)alphabet[(v >> 6) & 0x3fu];
        output[op++] = (unsigned char)alphabet[v & 0x3fu];
        if (padding) {
            output[op++] = '=';
            output[op++] = '=';
        }
        if (newline) {
            if (cr) output[op++] = '\r';
            output[op++] = '\n';
        }
    } else if (remainder == 2u) {
        uint32_t v = ((uint32_t)input[p] << 10) |
                     ((uint32_t)input[p + 1] << 2);
        output[op++] = (unsigned char)alphabet[(v >> 12) & 0x3fu];
        output[op++] = (unsigned char)alphabet[(v >> 6) & 0x3fu];
        output[op++] = (unsigned char)alphabet[v & 0x3fu];
        if (padding) output[op++] = '=';
        if (newline) {
            if (cr) output[op++] = '\r';
            output[op++] = '\n';
        }
    } else if (newline && length > 0 && count != B64_LINE_GROUPS) {
        if (cr) output[op++] = '\r';
        output[op++] = '\n';
    }
}

static int b64_decode_char(unsigned char c, bool url_safe)
{
    if (c >= 'A' && c <= 'Z') return (int)(c - 'A');
    if (c >= 'a' && c <= 'z') return 26 + (int)(c - 'a');
    if (c >= '0' && c <= '9') return 52 + (int)(c - '0');
    if (url_safe) {
        if (c == '-') return 62;
        if (c == '_') return 63;
    } else {
        if (c == '+') return 62;
        if (c == '/') return 63;
    }
    if (c == '=') return B64_EQUALS;
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
        c == '\v')
        return B64_SKIP;
    return B64_ERROR;
}

/* The state machine follows Android's Decoder exactly (states 0..5, with
 * padding optional).  Unlike Android's permissive byte table, anything that
 * is neither base64 data, '=' nor ASCII whitespace is an error, and any
 * malformed tail or padding is an error. */
static bool b64_decode_into(struct dvm *vm, const unsigned char *input,
                            size_t length, int32_t flags,
                            unsigned char **decoded, uint32_t *decoded_length)
{
    size_t capacity = (length / 4u) * 3u + 3u;
    unsigned char *out = (unsigned char *)malloc(capacity ? capacity : 1u);
    if (!out) {
        dvm__throw(vm, "java/lang/OutOfMemoryError", "Base64 decode buffer");
        return false;
    }

    bool url_safe = (flags & B64_URL_SAFE) != 0;
    int state = 0;
    uint32_t value = 0;
    size_t op = 0;

    for (size_t i = 0; i < length; ++i) {
        int d = b64_decode_char(input[i], url_safe);
        switch (state) {
        case 0:
            if (d >= 0) {
                value = (uint32_t)d;
                state = 1;
            } else if (d != B64_SKIP) {
                goto bad;
            }
            break;
        case 1:
            if (d >= 0) {
                value = (value << 6) | (uint32_t)d;
                state = 2;
            } else if (d != B64_SKIP) {
                goto bad;
            }
            break;
        case 2:
            if (d >= 0) {
                value = (value << 6) | (uint32_t)d;
                state = 3;
            } else if (d == B64_EQUALS) {
                out[op++] = (unsigned char)(value >> 4);
                state = 4;
            } else if (d != B64_SKIP) {
                goto bad;
            }
            break;
        case 3:
            if (d >= 0) {
                value = (value << 6) | (uint32_t)d;
                out[op] = (unsigned char)(value >> 16);
                out[op + 1] = (unsigned char)(value >> 8);
                out[op + 2] = (unsigned char)value;
                op += 3;
                state = 0;
            } else if (d == B64_EQUALS) {
                out[op] = (unsigned char)(value >> 10);
                out[op + 1] = (unsigned char)(value >> 2);
                op += 2;
                state = 5;
            } else if (d != B64_SKIP) {
                goto bad;
            }
            break;
        case 4:
            if (d == B64_EQUALS) {
                state = 5;
            } else if (d != B64_SKIP) {
                goto bad;
            }
            break;
        case 5:
            if (d != B64_SKIP) goto bad;
            break;
        default:
            goto bad;
        }
    }

    switch (state) {
    case 0:
        break;
    case 1:
        goto bad;
    case 2:
        out[op++] = (unsigned char)(value >> 4);
        break;
    case 3:
        out[op++] = (unsigned char)(value >> 10);
        out[op++] = (unsigned char)(value >> 2);
        break;
    case 4:
        goto bad;
    case 5:
        break;
    default:
        goto bad;
    }

    *decoded = out;
    *decoded_length = (uint32_t)op;
    return true;

bad:
    free(out);
    dvm__throw(vm, "java/lang/IllegalArgumentException", "bad base-64");
    return false;
}

static bool b64_encode_common(struct dvm *vm, dvm_ref input_ref,
                              int32_t flags, unsigned char **encoded,
                              size_t *encoded_length)
{
    if (!input_ref) {
        dvm__throw(vm, "java/lang/NullPointerException", "input == null");
        return false;
    }
    if (!b64_flags_valid(vm, flags)) return false;
    struct dvm_object *input = dvm__obj(vm, input_ref);
    if (!input || input->kind != DVM_OBJ_ARRAY || input->elem_kind != 'B') {
        dvm__throw(vm, "java/lang/IllegalArgumentException",
                   "input is not a byte[]");
        return false;
    }
    size_t size;
    if (!b64_encoded_size(input->length, flags, &size)) {
        dvm__throw(vm, "java/lang/OutOfMemoryError", "Base64 encode buffer");
        return false;
    }
    unsigned char *buffer = (unsigned char *)malloc(size ? size : 1u);
    if (!buffer) {
        dvm__throw(vm, "java/lang/OutOfMemoryError", "Base64 encode buffer");
        return false;
    }
    b64_encode_into((const unsigned char *)input->data, input->length, flags,
                    buffer);
    *encoded = buffer;
    *encoded_length = size;
    return true;
}

static bool b64_return_bytes(struct dvm *vm, unsigned char *data,
                             uint32_t length, union dvm_value *out)
{
    dvm_ref array = dvm_new_array(vm, 'B', "B", length);
    if (!array) {
        free(data);
        dvm__throw(vm, "java/lang/OutOfMemoryError", "Base64 result array");
        return false;
    }
    if (length) memcpy(dvm_array_data(vm, array), data, length);
    free(data);
    out->l = array;
    return true;
}

static bool base64_encode_to_string_call(struct dvm *vm,
                                         const union dvm_value *args,
                                         union dvm_value *out)
{
    unsigned char *encoded = NULL;
    size_t encoded_length = 0;
    if (!b64_encode_common(vm, args[0].l, args[1].i, &encoded,
                           &encoded_length))
        return false;
    dvm_ref text = dvm_new_string_n(vm, (const char *)encoded, encoded_length);
    free(encoded);
    if (!text) {
        dvm__throw(vm, "java/lang/OutOfMemoryError", "Base64 result string");
        return false;
    }
    out->l = text;
    return true;
}

static bool base64_encode_call(struct dvm *vm, const union dvm_value *args,
                               union dvm_value *out)
{
    unsigned char *encoded = NULL;
    size_t encoded_length = 0;
    if (!b64_encode_common(vm, args[0].l, args[1].i, &encoded,
                           &encoded_length))
        return false;
    bool ok = b64_return_bytes(vm, encoded, (uint32_t)encoded_length, out);
    return ok;
}

static bool base64_decode_string_call(struct dvm *vm,
                                      const union dvm_value *args,
                                      union dvm_value *out)
{
    if (!args[0].l) {
        dvm__throw(vm, "java/lang/NullPointerException", "str == null");
        return false;
    }
    if (!b64_flags_valid(vm, args[1].i)) return false;
    const char *text = dvm_string_utf8(vm, args[0].l);
    if (!text) {
        dvm__throw(vm, "java/lang/IllegalArgumentException",
                   "input is not a String");
        return false;
    }
    uint32_t text_length = dvm_string_utf8_length(vm, args[0].l);
    unsigned char *decoded = NULL;
    uint32_t decoded_length = 0;
    if (!b64_decode_into(vm, (const unsigned char *)text, text_length,
                         args[1].i, &decoded, &decoded_length))
        return false;
    return b64_return_bytes(vm, decoded, decoded_length, out);
}

static bool base64_decode_bytes_call(struct dvm *vm,
                                     const union dvm_value *args,
                                     union dvm_value *out)
{
    if (!args[0].l) {
        dvm__throw(vm, "java/lang/NullPointerException", "input == null");
        return false;
    }
    if (!b64_flags_valid(vm, args[1].i)) return false;
    struct dvm_object *input = dvm__obj(vm, args[0].l);
    if (!input || input->kind != DVM_OBJ_ARRAY || input->elem_kind != 'B') {
        dvm__throw(vm, "java/lang/IllegalArgumentException",
                   "input is not a byte[]");
        return false;
    }
    unsigned char *decoded = NULL;
    uint32_t decoded_length = 0;
    if (!b64_decode_into(vm, (const unsigned char *)input->data,
                         input->length, args[1].i, &decoded, &decoded_length))
        return false;
    return b64_return_bytes(vm, decoded, decoded_length, out);
}

/* ------------------------------------------------------------------ */
/* Runtime entry points */

void system_runtime_vm_start(struct dvm *vm)
{
    if (vm) active_vm = vm;
}

void system_runtime_vm_end(struct dvm *vm)
{
    if (vm && vm == active_vm) active_vm = NULL;
}

bool system_runtime_call(struct dvm *vm, const char *class_name,
                         const char *method, const char *sig, dvm_ref self,
                         const union dvm_value *args, int nargs,
                         union dvm_value *out)
{
    (void)self;
    if (!vm || vm != active_vm || !class_name || !method || !sig || !out)
        return false;

    if (!strcmp(class_name, "java/lang/System")) {
        if (is_call(method, sig, "arraycopy",
                    "(Ljava/lang/Object;ILjava/lang/Object;II)V") &&
            nargs == 5 && args)
            return system_arraycopy(vm, args);
        return false;
    }

    if (!strcmp(class_name, "android/util/Base64")) {
        if (is_call(method, sig, "encodeToString",
                    "([BI)Ljava/lang/String;") &&
            nargs == 2 && args)
            return base64_encode_to_string_call(vm, args, out);
        /* This overload is not in the primary task list, but the guest DEX
         * calls it (for example Lcom/base/cachelib/utils/ConvertUtils;), so it
         * shares the same encoder. */
        if (is_call(method, sig, "encode", "([BI)[B") && nargs == 2 && args)
            return base64_encode_call(vm, args, out);
        if (is_call(method, sig, "decode", "(Ljava/lang/String;I)[B") &&
            nargs == 2 && args)
            return base64_decode_string_call(vm, args, out);
        /* Likewise called by Lcom/cloudplay/messagesdk/a/c; and
         * Lcom/tencent/open/utils/a; in the guest DEX. */
        if (is_call(method, sig, "decode", "([BI)[B") && nargs == 2 && args)
            return base64_decode_bytes_call(vm, args, out);
        return false;
    }

    return false;
}

/* ------------------------------------------------------------------ */
/* Selftest helpers */

#define SYSTEM_CHECK(expr)                                                    \
    do {                                                                      \
        if (!(expr)) {                                                        \
            sdk_trace_fprintf(                                                \
                stderr, "SYSTEM_RUNTIME_SELFTEST_FAIL line=%d expression=%s\n", \
                __LINE__, #expr);                                             \
            goto done;                                                        \
        }                                                                     \
    } while (0)

static const char *selftest_exception_name(struct dvm *vm)
{
    dvm_ref exception = dvm_exception(vm);
    struct dvm_object *object = exception ? dvm__obj(vm, exception) : NULL;
    return object && object->cls && object->cls->name ? object->cls->name
                                                      : "none";
}

static dvm_ref selftest_byte_array(struct dvm *vm,
                                   const unsigned char *data,
                                   uint32_t length)
{
    dvm_ref ref = dvm_new_array(vm, 'B', "B", length);
    if (ref && length && data) memcpy(dvm_array_data(vm, ref), data, length);
    return ref;
}

static dvm_ref selftest_int_array(struct dvm *vm, const int32_t *values,
                                  uint32_t length)
{
    dvm_ref ref = dvm_new_array(vm, 'I', "I", length);
    if (ref && length && values)
        memcpy(dvm_array_data(vm, ref), values, (size_t)length * 4u);
    return ref;
}

static dvm_ref selftest_long_array(struct dvm *vm, const int64_t *values,
                                   uint32_t length)
{
    dvm_ref ref = dvm_new_array(vm, 'J', "J", length);
    if (ref && length && values)
        memcpy(dvm_array_data(vm, ref), values, (size_t)length * 8u);
    return ref;
}

static dvm_ref selftest_ref_array(struct dvm *vm, const char *elem_desc,
                                  const dvm_ref *values, uint32_t length)
{
    dvm_ref ref = dvm_new_array(vm, 'L', elem_desc, length);
    if (ref && length && values)
        memcpy(dvm_array_data(vm, ref), values,
               (size_t)length * sizeof(dvm_ref));
    return ref;
}

static bool selftest_bytes_equal(struct dvm *vm, dvm_ref ref,
                                 const unsigned char *expected,
                                 uint32_t length)
{
    struct dvm_object *object = dvm__obj(vm, ref);
    return object && object->kind == DVM_OBJ_ARRAY &&
           object->elem_kind == 'B' && object->length == length &&
           (!length || !memcmp(object->data, expected, length));
}

static bool selftest_ints_equal(struct dvm *vm, dvm_ref ref,
                                const int32_t *expected, uint32_t length)
{
    struct dvm_object *object = dvm__obj(vm, ref);
    return object && object->kind == DVM_OBJ_ARRAY &&
           object->elem_kind == 'I' && object->length == length &&
           (!length ||
            !memcmp(object->data, expected, (size_t)length * 4u));
}

static bool selftest_longs_equal(struct dvm *vm, dvm_ref ref,
                                 const int64_t *expected, uint32_t length)
{
    struct dvm_object *object = dvm__obj(vm, ref);
    return object && object->kind == DVM_OBJ_ARRAY &&
           object->elem_kind == 'J' && object->length == length &&
           (!length ||
            !memcmp(object->data, expected, (size_t)length * 8u));
}

static bool selftest_refs_equal(struct dvm *vm, dvm_ref ref,
                                const dvm_ref *expected, uint32_t length)
{
    struct dvm_object *object = dvm__obj(vm, ref);
    return object && object->kind == DVM_OBJ_ARRAY &&
           object->elem_kind == 'L' && object->length == length &&
           (!length || !memcmp(object->data, expected,
                               (size_t)length * sizeof(dvm_ref)));
}

static bool selftest_call_arraycopy(struct dvm *vm, dvm_ref src,
                                    int32_t src_pos, dvm_ref dst,
                                    int32_t dst_pos, int32_t length)
{
    union dvm_value args[5];
    union dvm_value out = {0};
    args[0].l = src;
    args[1].i = src_pos;
    args[2].l = dst;
    args[3].i = dst_pos;
    args[4].i = length;
    return system_runtime_call(
        vm, "java/lang/System", "arraycopy",
        "(Ljava/lang/Object;ILjava/lang/Object;II)V", 0, args, 5, &out);
}

static bool selftest_arraycopy_error(struct dvm *vm, const char *expected,
                                     dvm_ref src, int32_t src_pos,
                                     dvm_ref dst, int32_t dst_pos,
                                     int32_t length)
{
    bool called = selftest_call_arraycopy(vm, src, src_pos, dst, dst_pos,
                                          length);
    bool ok = !called && !strcmp(selftest_exception_name(vm), expected);
    if (!ok) {
        sdk_trace_fprintf(
            stderr,
            "SYSTEM_RUNTIME_SELFTEST_FAIL stage=arraycopy expected=%s actual=%s called=%d\n",
            expected, selftest_exception_name(vm), called ? 1 : 0);
    }
    if (dvm_exception(vm)) dvm_clear_exception(vm);
    return ok;
}

static bool selftest_b64_encode_string(struct dvm *vm,
                                       const unsigned char *data,
                                       uint32_t length, int32_t flags,
                                       const char *expected,
                                       uint32_t expected_length)
{
    dvm_ref input = selftest_byte_array(vm, data, length);
    union dvm_value args[2];
    union dvm_value out = {0};
    bool ok = false;
    if (!input) {
        sdk_trace_fprintf(stderr,
                          "SYSTEM_RUNTIME_SELFTEST_FAIL stage=b64_encode_alloc\n");
        return false;
    }
    args[0].l = input;
    args[1].i = flags;
    bool called = system_runtime_call(
        vm, "android/util/Base64", "encodeToString",
        "([BI)Ljava/lang/String;", 0, args, 2, &out);
    const char *actual = NULL;
    uint32_t actual_length = 0;
    if (called && !dvm_exception(vm) && out.l) {
        actual = dvm_string_utf8(vm, out.l);
        actual_length = dvm_string_utf8_length(vm, out.l);
    }
    ok = actual && actual_length == expected_length &&
         (!expected_length || !memcmp(actual, expected, expected_length));
    if (!ok) {
        sdk_trace_fprintf(
            stderr,
            "SYSTEM_RUNTIME_SELFTEST_FAIL stage=b64_encode flags=%d called=%d expected_len=%u actual_len=%u actual=%.120s\n",
            (int)flags, called ? 1 : 0, expected_length, actual_length,
            actual ? actual : "(none)");
    }
    if (dvm_exception(vm)) dvm_clear_exception(vm);
    return ok;
}

static bool selftest_b64_encode_error(struct dvm *vm, dvm_ref input,
                                      int32_t flags, const char *expected)
{
    union dvm_value args[2];
    union dvm_value out = {0};
    args[0].l = input;
    args[1].i = flags;
    bool called = system_runtime_call(
        vm, "android/util/Base64", "encodeToString",
        "([BI)Ljava/lang/String;", 0, args, 2, &out);
    bool ok = !called && !strcmp(selftest_exception_name(vm), expected);
    if (!ok) {
        sdk_trace_fprintf(
            stderr,
            "SYSTEM_RUNTIME_SELFTEST_FAIL stage=b64_encode_error expected=%s actual=%s called=%d\n",
            expected, selftest_exception_name(vm), called ? 1 : 0);
    }
    if (dvm_exception(vm)) dvm_clear_exception(vm);
    return ok;
}

static bool selftest_b64_decode_string(struct dvm *vm, const char *encoded,
                                       int32_t flags,
                                       const unsigned char *expected,
                                       uint32_t expected_length)
{
    dvm_ref input = dvm_new_string(vm, encoded);
    union dvm_value args[2];
    union dvm_value out = {0};
    bool ok = false;
    if (!input) {
        sdk_trace_fprintf(stderr,
                          "SYSTEM_RUNTIME_SELFTEST_FAIL stage=b64_decode_alloc\n");
        return false;
    }
    args[0].l = input;
    args[1].i = flags;
    bool called = system_runtime_call(
        vm, "android/util/Base64", "decode", "(Ljava/lang/String;I)[B", 0,
        args, 2, &out);
    ok = called && !dvm_exception(vm) &&
         selftest_bytes_equal(vm, out.l, expected, expected_length);
    if (!ok) {
        sdk_trace_fprintf(
            stderr,
            "SYSTEM_RUNTIME_SELFTEST_FAIL stage=b64_decode_string flags=%d called=%d expected_len=%u actual_len=%u exception=%s\n",
            (int)flags, called ? 1 : 0, expected_length,
            out.l ? dvm_array_length(vm, out.l) : 0u,
            selftest_exception_name(vm));
    }
    if (dvm_exception(vm)) dvm_clear_exception(vm);
    return ok;
}

static bool selftest_b64_decode_bytes(struct dvm *vm,
                                      const unsigned char *data,
                                      uint32_t length, int32_t flags,
                                      const unsigned char *expected,
                                      uint32_t expected_length)
{
    dvm_ref input = selftest_byte_array(vm, data, length);
    union dvm_value args[2];
    union dvm_value out = {0};
    bool ok = false;
    if (!input) {
        sdk_trace_fprintf(stderr,
                          "SYSTEM_RUNTIME_SELFTEST_FAIL stage=b64_decode_bytes_alloc\n");
        return false;
    }
    args[0].l = input;
    args[1].i = flags;
    bool called = system_runtime_call(
        vm, "android/util/Base64", "decode", "([BI)[B", 0, args, 2, &out);
    ok = called && !dvm_exception(vm) &&
         selftest_bytes_equal(vm, out.l, expected, expected_length);
    if (!ok) {
        sdk_trace_fprintf(
            stderr,
            "SYSTEM_RUNTIME_SELFTEST_FAIL stage=b64_decode_bytes flags=%d called=%d expected_len=%u actual_len=%u exception=%s\n",
            (int)flags, called ? 1 : 0, expected_length,
            out.l ? dvm_array_length(vm, out.l) : 0u,
            selftest_exception_name(vm));
    }
    if (dvm_exception(vm)) dvm_clear_exception(vm);
    return ok;
}

static bool selftest_b64_decode_string_error(struct dvm *vm,
                                             const char *encoded,
                                             int32_t flags,
                                             const char *expected)
{
    dvm_ref input = encoded ? dvm_new_string(vm, encoded) : 0;
    union dvm_value args[2];
    union dvm_value out = {0};
    bool ok = false;
    if (encoded && !input) {
        sdk_trace_fprintf(stderr,
                          "SYSTEM_RUNTIME_SELFTEST_FAIL stage=b64_decode_error_alloc\n");
        return false;
    }
    args[0].l = input;
    args[1].i = flags;
    bool called = system_runtime_call(
        vm, "android/util/Base64", "decode", "(Ljava/lang/String;I)[B", 0,
        args, 2, &out);
    ok = !called && !strcmp(selftest_exception_name(vm), expected);
    if (!ok) {
        sdk_trace_fprintf(
            stderr,
            "SYSTEM_RUNTIME_SELFTEST_FAIL stage=b64_decode_string_error encoded=%.40s expected=%s actual=%s called=%d\n",
            encoded ? encoded : "(null)", expected, selftest_exception_name(vm),
            called ? 1 : 0);
    }
    if (dvm_exception(vm)) dvm_clear_exception(vm);
    return ok;
}

static bool selftest_b64_decode_bytes_error(struct dvm *vm,
                                            const unsigned char *data,
                                            uint32_t length, int32_t flags,
                                            const char *expected)
{
    dvm_ref input = selftest_byte_array(vm, data, length);
    union dvm_value args[2];
    union dvm_value out = {0};
    bool ok = false;
    if (!input) {
        sdk_trace_fprintf(stderr,
                          "SYSTEM_RUNTIME_SELFTEST_FAIL stage=b64_decode_bytes_error_alloc\n");
        return false;
    }
    args[0].l = input;
    args[1].i = flags;
    bool called = system_runtime_call(
        vm, "android/util/Base64", "decode", "([BI)[B", 0, args, 2, &out);
    ok = !called && !strcmp(selftest_exception_name(vm), expected);
    if (!ok) {
        sdk_trace_fprintf(
            stderr,
            "SYSTEM_RUNTIME_SELFTEST_FAIL stage=b64_decode_bytes_error expected=%s actual=%s called=%d\n",
            expected, selftest_exception_name(vm), called ? 1 : 0);
    }
    if (dvm_exception(vm)) dvm_clear_exception(vm);
    return ok;
}

/* ------------------------------------------------------------------ */
/* Selftest vectors */

static bool selftest_arraycopy_vectors(struct dvm *vm)
{
    static const int32_t source_values[5] = {10, 20, 30, 40, 50};
    static const int32_t minus_one[5] = {-1, -1, -1, -1, -1};
    static const int32_t copied[5] = {20, 30, 40, -1, -1};
    static const int32_t overlap_expected[5] = {1, 1, 2, 3, 4};
    static const int32_t backward_expected[5] = {2, 3, 4, 5, 5};
    static const int32_t reset_values[5] = {1, 2, 3, 4, 5};
    static const int64_t long_values[3] = {0x1122334455667788LL,
                                           0x2233445566778899LL,
                                           0x33445566778899aaLL};
    static const unsigned char byte_values[5] = {1, 2, 3, 4, 5};
    static const unsigned char byte_overlap[5] = {1, 1, 2, 3, 4};
    bool ok = false;
    dvm_ref src = 0, dst = 0, a = 0, bs = 0, bd = 0, ls = 0, ld = 0;
    dvm_ref not_array = 0, obj_arr = 0, byte_arr = 0, long_arr = 0;
    dvm_ref text_a = 0, text_b = 0, text_c = 0;
    dvm_ref string_src = 0, object_dst = 0;
    dvm_ref mixed_src = 0, mixed_dst = 0;
    dvm_ref null_mixed_src = 0, null_mixed_dst = 0;
    dvm_ref only_strings_src = 0, only_strings_dst = 0;
    struct dvm_class *object_class = NULL;
    dvm_ref plain_object = 0;

    src = selftest_int_array(vm, source_values, 5);
    dst = selftest_int_array(vm, minus_one, 5);
    SYSTEM_CHECK(src && dst);
    SYSTEM_CHECK(selftest_call_arraycopy(vm, src, 1, dst, 0, 3));
    SYSTEM_CHECK(!dvm_exception(vm));
    SYSTEM_CHECK(selftest_ints_equal(vm, dst, copied, 5));

    /* Zero length, including the one-past-the-end position. */
    SYSTEM_CHECK(selftest_call_arraycopy(vm, src, 5, dst, 5, 0));
    SYSTEM_CHECK(!dvm_exception(vm));
    SYSTEM_CHECK(selftest_arraycopy_error(
        vm, "java/lang/IndexOutOfBoundsException", src, 6, dst, 0, 0));

    /* Negative fields and ranges past the end. */
    SYSTEM_CHECK(selftest_arraycopy_error(
        vm, "java/lang/IndexOutOfBoundsException", src, -1, dst, 0, 1));
    SYSTEM_CHECK(selftest_arraycopy_error(
        vm, "java/lang/IndexOutOfBoundsException", src, 0, dst, -1, 1));
    SYSTEM_CHECK(selftest_arraycopy_error(
        vm, "java/lang/IndexOutOfBoundsException", src, 0, dst, 0, -1));
    SYSTEM_CHECK(selftest_arraycopy_error(
        vm, "java/lang/IndexOutOfBoundsException", src, 4, dst, 0, 2));
    SYSTEM_CHECK(selftest_arraycopy_error(
        vm, "java/lang/IndexOutOfBoundsException", src, 0, dst, 4, 2));

    /* Null source and destination. */
    SYSTEM_CHECK(selftest_arraycopy_error(
        vm, "java/lang/NullPointerException", 0, 0, dst, 0, 1));
    SYSTEM_CHECK(selftest_arraycopy_error(
        vm, "java/lang/NullPointerException", src, 0, 0, 0, 1));

    /* Non-arrays. */
    not_array = dvm_new_string(vm, "not-an-array");
    SYSTEM_CHECK(not_array);
    SYSTEM_CHECK(selftest_arraycopy_error(
        vm, "java/lang/ArrayStoreException", not_array, 0, dst, 0, 1));
    SYSTEM_CHECK(selftest_arraycopy_error(
        vm, "java/lang/ArrayStoreException", src, 0, not_array, 0, 1));

    /* Primitive component types must match exactly. */
    byte_arr = selftest_byte_array(vm, byte_values, 5);
    long_arr = dvm_new_array(vm, 'J', "J", 5);
    SYSTEM_CHECK(byte_arr && long_arr);
    SYSTEM_CHECK(selftest_arraycopy_error(
        vm, "java/lang/ArrayStoreException", byte_arr, 0, dst, 0, 3));
    SYSTEM_CHECK(selftest_arraycopy_error(
        vm, "java/lang/ArrayStoreException", src, 0, byte_arr, 0, 3));
    SYSTEM_CHECK(selftest_arraycopy_error(
        vm, "java/lang/ArrayStoreException", src, 0, long_arr, 0, 3));

    /* Reference and primitive arrays are never interchangeable. */
    obj_arr = dvm_new_array(vm, 'L', "Ljava/lang/Object;", 5);
    SYSTEM_CHECK(obj_arr);
    SYSTEM_CHECK(selftest_arraycopy_error(
        vm, "java/lang/ArrayStoreException", src, 0, obj_arr, 0, 3));
    SYSTEM_CHECK(selftest_arraycopy_error(
        vm, "java/lang/ArrayStoreException", obj_arr, 0, dst, 0, 3));

    /* Primitive copies preserve element width and overlap semantics. */
    bs = selftest_byte_array(vm, byte_values, 5);
    bd = selftest_byte_array(vm, byte_values, 5);
    SYSTEM_CHECK(bs && bd);
    SYSTEM_CHECK(selftest_call_arraycopy(vm, bs, 0, bs, 1, 4));
    SYSTEM_CHECK(!dvm_exception(vm));
    SYSTEM_CHECK(selftest_bytes_equal(vm, bs, byte_overlap, 5));

    ls = selftest_long_array(vm, long_values, 3);
    ld = dvm_new_array(vm, 'J', "J", 3);
    SYSTEM_CHECK(ls && ld);
    SYSTEM_CHECK(selftest_call_arraycopy(vm, ls, 0, ld, 0, 3));
    SYSTEM_CHECK(!dvm_exception(vm));
    SYSTEM_CHECK(selftest_longs_equal(vm, ld, long_values, 3));

    /* Two-byte elements are copied with width 2, and boolean[] is not
     * interchangeable with byte[] even though both are one byte wide. */
    {
        static const int16_t short_values[4] = {0x1122, 0x3344, 0x5566,
                                                0x7788};
        dvm_ref ss = dvm_new_array(vm, 'S', "S", 4);
        dvm_ref sd = dvm_new_array(vm, 'S', "S", 4);
        dvm_ref bool_arr = dvm_new_array(vm, 'Z', "Z", 3);
        SYSTEM_CHECK(ss && sd && bool_arr);
        memcpy(dvm_array_data(vm, ss), short_values, sizeof short_values);
        SYSTEM_CHECK(selftest_call_arraycopy(vm, ss, 0, sd, 0, 4));
        SYSTEM_CHECK(!dvm_exception(vm));
        SYSTEM_CHECK(!memcmp(dvm_array_data(vm, sd), short_values,
                             sizeof short_values));
        SYSTEM_CHECK(selftest_arraycopy_error(
            vm, "java/lang/ArrayStoreException", bool_arr, 0, byte_arr, 0,
            3));
    }

    /* OVERLAP: forward self-copy (memmove-like) and backward self-copy. */
    a = selftest_int_array(vm, reset_values, 5);
    SYSTEM_CHECK(a);
    SYSTEM_CHECK(selftest_call_arraycopy(vm, a, 0, a, 1, 4));
    SYSTEM_CHECK(!dvm_exception(vm));
    SYSTEM_CHECK(selftest_ints_equal(vm, a, overlap_expected, 5));
    {
        static const int32_t second[5] = {1, 2, 3, 4, 5};
        dvm_ref b = selftest_int_array(vm, second, 5);
        SYSTEM_CHECK(b);
        SYSTEM_CHECK(selftest_call_arraycopy(vm, b, 1, b, 0, 4));
        SYSTEM_CHECK(!dvm_exception(vm));
        SYSTEM_CHECK(selftest_ints_equal(vm, b, backward_expected, 5));
    }

    /* Reference arrays: covariant fast path String[] -> Object[]. */
    text_a = dvm_new_string(vm, "alpha");
    text_b = dvm_new_string(vm, "beta");
    text_c = dvm_new_string(vm, "gamma");
    SYSTEM_CHECK(text_a && text_b && text_c);
    {
        dvm_ref values[2] = {text_a, text_b};
        dvm_ref expected[2] = {text_a, text_b};
        string_src = selftest_ref_array(vm, "Ljava/lang/String;", values, 2);
        object_dst = dvm_new_array(vm, 'L', "Ljava/lang/Object;", 2);
        SYSTEM_CHECK(string_src && object_dst);
        SYSTEM_CHECK(selftest_call_arraycopy(vm, string_src, 0, object_dst, 0,
                                             2));
        SYSTEM_CHECK(!dvm_exception(vm));
        SYSTEM_CHECK(selftest_refs_equal(vm, object_dst, expected, 2));
    }

    /* Reference arrays: slow path with every element assignable. */
    {
        dvm_ref values[1] = {text_a};
        dvm_ref expected[1] = {text_a};
        only_strings_src =
            selftest_ref_array(vm, "Ljava/lang/Object;", values, 1);
        only_strings_dst = dvm_new_array(vm, 'L', "Ljava/lang/String;", 1);
        SYSTEM_CHECK(only_strings_src && only_strings_dst);
        SYSTEM_CHECK(selftest_call_arraycopy(vm, only_strings_src, 0,
                                             only_strings_dst, 0, 1));
        SYSTEM_CHECK(!dvm_exception(vm));
        SYSTEM_CHECK(selftest_refs_equal(vm, only_strings_dst, expected, 1));
    }

    /* Reference array store check: the first element is copied, then the
     * plain Object fails against String[] and the call throws. */
    object_class = dvm__class_by_desc(vm, "Ljava/lang/Object;");
    plain_object = object_class ? dvm_new_object(vm, object_class) : 0;
    SYSTEM_CHECK(plain_object);
    {
        dvm_ref values[2] = {text_a, plain_object};
        dvm_ref expected[2] = {text_a, 0};
        mixed_src = selftest_ref_array(vm, "Ljava/lang/Object;", values, 2);
        mixed_dst = dvm_new_array(vm, 'L', "Ljava/lang/String;", 2);
        SYSTEM_CHECK(mixed_src && mixed_dst);
        SYSTEM_CHECK(selftest_arraycopy_error(
            vm, "java/lang/ArrayStoreException", mixed_src, 0, mixed_dst, 0,
            2));
        SYSTEM_CHECK(selftest_refs_equal(vm, mixed_dst, expected, 2));
    }

    /* A null reference is always storable; a later incompatible element still
     * throws and the null prefix remains. */
    {
        dvm_ref values[2] = {0, plain_object};
        dvm_ref expected[2] = {0, 0};
        null_mixed_src =
            selftest_ref_array(vm, "Ljava/lang/Object;", values, 2);
        null_mixed_dst = dvm_new_array(vm, 'L', "Ljava/lang/String;", 2);
        SYSTEM_CHECK(null_mixed_src && null_mixed_dst);
        SYSTEM_CHECK(selftest_arraycopy_error(
            vm, "java/lang/ArrayStoreException", null_mixed_src, 0,
            null_mixed_dst, 0, 2));
        SYSTEM_CHECK(selftest_refs_equal(vm, null_mixed_dst, expected, 2));
    }

    /* Reference array self copy uses memmove semantics. */
    {
        dvm_ref values[3] = {text_a, text_b, text_c};
        dvm_ref expected[3] = {text_a, text_a, text_b};
        dvm_ref self = selftest_ref_array(vm, "Ljava/lang/String;", values, 3);
        SYSTEM_CHECK(self);
        SYSTEM_CHECK(selftest_call_arraycopy(vm, self, 0, self, 1, 2));
        SYSTEM_CHECK(!dvm_exception(vm));
        SYSTEM_CHECK(selftest_refs_equal(vm, self, expected, 3));
    }

    ok = true;
done:
    if (dvm_exception(vm)) dvm_clear_exception(vm);
    return ok;
}

static bool selftest_base64_vectors(struct dvm *vm)
{
    static const unsigned char f_bytes[1] = {'f'};
    static const unsigned char fo_bytes[2] = {'f', 'o'};
    static const unsigned char foo_bytes[3] = {'f', 'o', 'o'};
    static const unsigned char foob_bytes[4] = {'f', 'o', 'o', 'b'};
    static const unsigned char fooba_bytes[5] = {'f', 'o', 'o', 'b', 'a'};
    static const unsigned char foobar_bytes[6] = {'f', 'o', 'o', 'b', 'a', 'r'};
    static const unsigned char fbff_bytes[2] = {0xfb, 0xff};
    static const unsigned char zero_byte[1] = {0};
    static const unsigned char encoded_plus_slash[4] = {'+', '/', '8', '='};
    static const unsigned char encoded_dash_underscore[4] = {'-', '_', '8',
                                                             '='};
    static const unsigned char encoded_dash_underscore_nopad[3] = {'-', '_',
                                                                   '8'};
    static const unsigned char bad_chars[4] = {'!', '!', '!', '!'};
    bool ok = false;
    unsigned char zeros57[57];
    unsigned char data256[256];
    char wrapped[160];
    dvm_ref empty_input = 0;
    dvm_ref null_input_result = 0;

    SYSTEM_CHECK(selftest_b64_encode_string(vm, f_bytes, 1, 2, "Zg==", 4));
    SYSTEM_CHECK(selftest_b64_encode_string(vm, fo_bytes, 2, 2, "Zm8=", 4));
    SYSTEM_CHECK(selftest_b64_encode_string(vm, foo_bytes, 3, 2, "Zm9v", 4));
    SYSTEM_CHECK(selftest_b64_encode_string(vm, foob_bytes, 4, 2, "Zm9vYg==",
                                            8));
    SYSTEM_CHECK(selftest_b64_encode_string(vm, fooba_bytes, 5, 2, "Zm9vYmE=",
                                            8));
    SYSTEM_CHECK(
        selftest_b64_encode_string(vm, foobar_bytes, 6, 2, "Zm9vYmFy", 8));

    empty_input = selftest_byte_array(vm, NULL, 0);
    SYSTEM_CHECK(empty_input);
    SYSTEM_CHECK(
        selftest_b64_encode_string(vm, NULL, 0, B64_DEFAULT, "", 0));
    SYSTEM_CHECK(
        selftest_b64_encode_string(vm, NULL, 0, B64_NO_PADDING, "", 0));

    /* Alphabet selection and padding. */
    SYSTEM_CHECK(selftest_b64_encode_string(
        vm, fbff_bytes, 2, B64_NO_WRAP, (const char *)encoded_plus_slash, 4));
    SYSTEM_CHECK(selftest_b64_encode_string(
        vm, fbff_bytes, 2, B64_NO_WRAP | B64_URL_SAFE,
        (const char *)encoded_dash_underscore, 4));
    SYSTEM_CHECK(selftest_b64_encode_string(
        vm, fbff_bytes, 2, B64_NO_WRAP | B64_URL_SAFE | B64_NO_PADDING,
        (const char *)encoded_dash_underscore_nopad, 3));
    SYSTEM_CHECK(
        selftest_b64_encode_string(vm, f_bytes, 1, B64_NO_WRAP | B64_NO_PADDING,
                                   "Zg", 2));

    /* Default RFC 2045 wrapping: 19 tuples per line and a final newline. */
    memset(zeros57, 0, sizeof zeros57);
    memset(wrapped, 'A', 76);
    wrapped[76] = '\n';
    SYSTEM_CHECK(
        selftest_b64_encode_string(vm, zeros57, 57, B64_DEFAULT, wrapped, 77));
    wrapped[76] = '\r';
    wrapped[77] = '\n';
    SYSTEM_CHECK(selftest_b64_encode_string(vm, zeros57, 57, B64_CRLF, wrapped,
                                            78));
    SYSTEM_CHECK(selftest_b64_encode_string(vm, zeros57, 57, B64_NO_WRAP,
                                            wrapped, 76));

    /* One full line followed by a partial line. */
    {
        unsigned char zeros58[58];
        memset(zeros58, 0, sizeof zeros58);
        memset(wrapped, 'A', 76);
        wrapped[76] = '\n';
        memcpy(wrapped + 77, "AA==\n", 5);
        SYSTEM_CHECK(selftest_b64_encode_string(vm, zeros58, 58, B64_DEFAULT,
                                                wrapped, 82));
    }

    /* A single tuple still gets the trailing wrapped newline. */
    SYSTEM_CHECK(
        selftest_b64_encode_string(vm, zero_byte, 1, B64_DEFAULT, "AA==\n", 5));

    /* CRLF only matters when wrapping; it does not add anything with
     * NO_WRAP. */
    SYSTEM_CHECK(selftest_b64_encode_string(vm, zero_byte, 1,
                                            B64_NO_WRAP | B64_CRLF, "AA==", 4));

    /* Decode: standard vectors, optional padding, whitespace. */
    SYSTEM_CHECK(selftest_b64_decode_string(
        vm, "Zg==", B64_DEFAULT, f_bytes, 1));
    SYSTEM_CHECK(
        selftest_b64_decode_string(vm, "Zg", B64_DEFAULT, f_bytes, 1));
    SYSTEM_CHECK(selftest_b64_decode_string(
        vm, "Zm8=", B64_DEFAULT, fo_bytes, 2));
    SYSTEM_CHECK(
        selftest_b64_decode_string(vm, "Zm8", B64_DEFAULT, fo_bytes, 2));
    SYSTEM_CHECK(selftest_b64_decode_string(
        vm, "Zm9v", B64_DEFAULT, foo_bytes, 3));
    SYSTEM_CHECK(selftest_b64_decode_string(
        vm, "Zm9vYmFy", B64_DEFAULT, foobar_bytes, 6));
    SYSTEM_CHECK(
        selftest_b64_decode_string(vm, "", B64_DEFAULT, NULL, 0));
    SYSTEM_CHECK(selftest_b64_decode_string(
        vm, " Zg== \r\n", B64_DEFAULT, f_bytes, 1));
    SYSTEM_CHECK(
        selftest_b64_decode_string(vm, "Zg=\n=", B64_DEFAULT, f_bytes, 1));
    SYSTEM_CHECK(
        selftest_b64_decode_string(vm, "Zg==\n", B64_DEFAULT, f_bytes, 1));
    SYSTEM_CHECK(selftest_b64_decode_string(
        vm, "Zg==", B64_NO_CLOSE, f_bytes, 1));
    SYSTEM_CHECK(
        selftest_b64_decode_string(vm, "Zg", B64_NO_WRAP, f_bytes, 1));

    /* URL-safe alphabet is strict: the other alphabet is rejected. */
    SYSTEM_CHECK(selftest_b64_decode_string(
        vm, "+/8=", B64_DEFAULT, fbff_bytes, 2));
    SYSTEM_CHECK(selftest_b64_decode_string(
        vm, "+/8", B64_DEFAULT, fbff_bytes, 2));
    SYSTEM_CHECK(selftest_b64_decode_string(
        vm, "-_8=", B64_URL_SAFE, fbff_bytes, 2));
    SYSTEM_CHECK(selftest_b64_decode_string(
        vm, "-_8", B64_URL_SAFE, fbff_bytes, 2));

    /* Invalid padding, lengths and characters fail closed. */
    SYSTEM_CHECK(selftest_b64_decode_string_error(
        vm, "A", B64_DEFAULT, "java/lang/IllegalArgumentException"));
    SYSTEM_CHECK(selftest_b64_decode_string_error(
        vm, "Zg=", B64_DEFAULT, "java/lang/IllegalArgumentException"));
    SYSTEM_CHECK(selftest_b64_decode_string_error(
        vm, "Z===", B64_DEFAULT, "java/lang/IllegalArgumentException"));
    SYSTEM_CHECK(selftest_b64_decode_string_error(
        vm, "Zg===", B64_DEFAULT, "java/lang/IllegalArgumentException"));
    SYSTEM_CHECK(selftest_b64_decode_string_error(
        vm, "Zg==Zg==", B64_DEFAULT, "java/lang/IllegalArgumentException"));
    SYSTEM_CHECK(selftest_b64_decode_string_error(
        vm, "!!!!", B64_DEFAULT, "java/lang/IllegalArgumentException"));
    SYSTEM_CHECK(selftest_b64_decode_string_error(
        vm, "+/8=", B64_URL_SAFE, "java/lang/IllegalArgumentException"));
    SYSTEM_CHECK(selftest_b64_decode_string_error(
        vm, "-_8=", B64_DEFAULT, "java/lang/IllegalArgumentException"));

    /* Unknown flag bits are rejected instead of silently ignored. */
    SYSTEM_CHECK(selftest_b64_encode_error(
        vm, empty_input, 32, "java/lang/IllegalArgumentException"));
    SYSTEM_CHECK(selftest_b64_encode_error(
        vm, empty_input, -1, "java/lang/IllegalArgumentException"));
    SYSTEM_CHECK(selftest_b64_decode_string_error(
        vm, "Zg==", 32, "java/lang/IllegalArgumentException"));
    SYSTEM_CHECK(selftest_b64_decode_string_error(
        vm, "Zg==", -1, "java/lang/IllegalArgumentException"));

    /* Null and wrong-type arguments. */
    SYSTEM_CHECK(selftest_b64_encode_error(
        vm, 0, B64_DEFAULT, "java/lang/NullPointerException"));
    null_input_result = dvm_new_string(vm, "not-bytes");
    SYSTEM_CHECK(null_input_result);
    SYSTEM_CHECK(selftest_b64_encode_error(
        vm, null_input_result, B64_DEFAULT,
        "java/lang/IllegalArgumentException"));
    SYSTEM_CHECK(selftest_b64_decode_string_error(
        vm, NULL, B64_DEFAULT, "java/lang/NullPointerException"));
    /* A byte[] passed to the String overload is rejected. */
    {
        dvm_ref bytes =
            selftest_byte_array(vm, (const unsigned char *)"Zg==", 4);
        union dvm_value args[2];
        union dvm_value out = {0};
        SYSTEM_CHECK(bytes);
        args[0].l = bytes;
        args[1].i = B64_DEFAULT;
        SYSTEM_CHECK(!system_runtime_call(
            vm, "android/util/Base64", "decode", "(Ljava/lang/String;I)[B", 0,
            args, 2, &out));
        SYSTEM_CHECK(!strcmp(selftest_exception_name(vm),
                             "java/lang/IllegalArgumentException"));
        if (dvm_exception(vm)) dvm_clear_exception(vm);
    }

    /* decode(byte[]) overload, present in the guest DEX. */
    SYSTEM_CHECK(selftest_b64_decode_bytes(
        vm, (const unsigned char *)"Zg==", 4, B64_DEFAULT, f_bytes, 1));
    SYSTEM_CHECK(selftest_b64_decode_bytes(
        vm, (const unsigned char *)"Zm9vYmFy", 8, B64_DEFAULT, foobar_bytes,
        6));
    SYSTEM_CHECK(selftest_b64_decode_bytes(
        vm, (const unsigned char *)"-_8=", 4, B64_URL_SAFE, fbff_bytes, 2));
    SYSTEM_CHECK(selftest_b64_decode_bytes_error(
        vm, bad_chars, 4, B64_DEFAULT, "java/lang/IllegalArgumentException"));

    /* Round trip through both String and byte[] decoders. */
    for (uint32_t i = 0; i < 256; ++i) data256[i] = (unsigned char)i;
    {
        dvm_ref input = selftest_byte_array(vm, data256, 256);
        union dvm_value args[2];
        union dvm_value encoded = {0};
        union dvm_value decoded = {0};
        SYSTEM_CHECK(input);
        args[0].l = input;
        args[1].i = B64_NO_WRAP;
        SYSTEM_CHECK(system_runtime_call(
            vm, "android/util/Base64", "encodeToString",
            "([BI)Ljava/lang/String;", 0, args, 2, &encoded));
        SYSTEM_CHECK(!dvm_exception(vm) && encoded.l);
        args[0].l = encoded.l;
        args[1].i = B64_DEFAULT;
        SYSTEM_CHECK(system_runtime_call(
            vm, "android/util/Base64", "decode",
            "(Ljava/lang/String;I)[B", 0, args, 2, &decoded));
        SYSTEM_CHECK(!dvm_exception(vm));
        SYSTEM_CHECK(selftest_bytes_equal(vm, decoded.l, data256, 256));

        /* Default wrapping is inserted by the encoder and skipped by the
         * decoder. */
        memset(&encoded, 0, sizeof encoded);
        memset(&decoded, 0, sizeof decoded);
        args[0].l = input;
        args[1].i = B64_DEFAULT;
        SYSTEM_CHECK(system_runtime_call(
            vm, "android/util/Base64", "encodeToString",
            "([BI)Ljava/lang/String;", 0, args, 2, &encoded));
        SYSTEM_CHECK(!dvm_exception(vm) && encoded.l);
        SYSTEM_CHECK(dvm_string_utf8_length(vm, encoded.l) > 256);
        args[0].l = encoded.l;
        args[1].i = B64_DEFAULT;
        SYSTEM_CHECK(system_runtime_call(
            vm, "android/util/Base64", "decode",
            "(Ljava/lang/String;I)[B", 0, args, 2, &decoded));
        SYSTEM_CHECK(!dvm_exception(vm));
        SYSTEM_CHECK(selftest_bytes_equal(vm, decoded.l, data256, 256));

        /* encode([BI)[B shares the encoder. */
        memset(&encoded, 0, sizeof encoded);
        args[0].l = input;
        args[1].i = B64_NO_WRAP | B64_URL_SAFE;
        SYSTEM_CHECK(system_runtime_call(
            vm, "android/util/Base64", "encode", "([BI)[B", 0, args, 2,
            &encoded));
        SYSTEM_CHECK(!dvm_exception(vm) && encoded.l);
        memset(&decoded, 0, sizeof decoded);
        args[0].l = encoded.l;
        args[1].i = B64_URL_SAFE;
        SYSTEM_CHECK(system_runtime_call(
            vm, "android/util/Base64", "decode", "([BI)[B", 0, args, 2,
            &decoded));
        SYSTEM_CHECK(!dvm_exception(vm));
        SYSTEM_CHECK(selftest_bytes_equal(vm, decoded.l, data256, 256));
    }

    ok = true;
done:
    if (dvm_exception(vm)) dvm_clear_exception(vm);
    return ok;
}

bool system_runtime_selftest(struct dvm *vm)
{
    if (!vm || vm != active_vm) {
        sdk_trace_fprintf(
            stderr,
            "SYSTEM_RUNTIME_SELFTEST_FAIL stage=entry vm_set=%d active_vm_set=%d same=%d\n",
            vm ? 1 : 0, active_vm ? 1 : 0, vm == active_vm);
        return false;
    }
    if (dvm_exception(vm)) dvm_clear_exception(vm);

    bool okay = selftest_arraycopy_vectors(vm) &&
                selftest_base64_vectors(vm);
    if (okay) {
        sdk_trace_fprintf(
            stderr,
            "SYSTEM_RUNTIME_SELFTEST PASS arraycopy=1 arraycopy_refs=1 base64=1 base64_url=1 base64_nowrap=1 base64_nopad=1 base64_wrap=1 base64_invalid=1\n");
    } else {
        sdk_trace_fprintf(stderr, "SYSTEM_RUNTIME_SELFTEST FAIL\n");
        if (dvm_exception(vm)) dvm_clear_exception(vm);
    }
    return okay;
}
