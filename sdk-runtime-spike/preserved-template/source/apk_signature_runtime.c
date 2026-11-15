#include "apk_signature_runtime.h"
#include "mbedtls/sha256.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*
 * 原 APK 签名证书（X.509 DER），不是 HAP 证书，也不是伪造值。
 *
 * 来源 APK：BanGDream_10.1.3.0.0.0.0_20260917_031717_9932d.apk
 * 签名条目：META-INF/BILIGAME.RSA（PKCS#7）
 * DER 文件：sdk-runtime-spike/public-certificates/
 *           8d9975b308c79d52b1703900afe35f39afd5d40a4d01c8d29064288078a7af7d.der
 * 长度    ：715 字节
 * SHA256  ：8d9975b308c79d52b1703900afe35f39afd5d40a4d01c8d29064288078a7af7d
 * MD5     ：4502a02a00395dec05a4134ad593224d
 *
 * 已用 openssl pkcs7 从 APK 的 META-INF/BILIGAME.RSA 提取证书并比对：
 * 长度 715，SHA256 与上面的 DER 文件完全一致。
 */
#define APK_SIGNATURE_DER_LEN 715u
#define APK_SIGNATURE_CLASS "android/content/pm/Signature"

static const unsigned char apk_signature_der_bytes[APK_SIGNATURE_DER_LEN] = {
    0x30,0x82,0x02,0xc7,0x30,0x82,0x01,0xaf,0xa0,0x03,0x02,0x01,
    0x02,0x02,0x04,0x65,0x2f,0x3c,0xc3,0x30,0x0d,0x06,0x09,0x2a,
    0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x0b,0x05,0x00,0x30,0x13,
    0x31,0x11,0x30,0x0f,0x06,0x03,0x55,0x04,0x0a,0x13,0x08,0x62,
    0x69,0x6c,0x69,0x62,0x69,0x6c,0x69,0x30,0x20,0x17,0x0d,0x31,
    0x36,0x30,0x37,0x30,0x31,0x31,0x30,0x34,0x33,0x35,0x38,0x5a,
    0x18,0x0f,0x32,0x31,0x31,0x36,0x30,0x36,0x30,0x37,0x31,0x30,
    0x34,0x33,0x35,0x38,0x5a,0x30,0x13,0x31,0x11,0x30,0x0f,0x06,
    0x03,0x55,0x04,0x0a,0x13,0x08,0x62,0x69,0x6c,0x69,0x62,0x69,
    0x6c,0x69,0x30,0x82,0x01,0x22,0x30,0x0d,0x06,0x09,0x2a,0x86,
    0x48,0x86,0xf7,0x0d,0x01,0x01,0x01,0x05,0x00,0x03,0x82,0x01,
    0x0f,0x00,0x30,0x82,0x01,0x0a,0x02,0x82,0x01,0x01,0x00,0xc0,
    0xc3,0xe3,0xd5,0x09,0x83,0xee,0xcb,0x1a,0x40,0xb0,0x57,0xb3,
    0xab,0xd2,0xfd,0x71,0x4e,0xad,0x60,0x44,0x0e,0x5a,0xa1,0x75,
    0xa8,0xb4,0x35,0xef,0xcf,0x14,0x85,0x5f,0x83,0x9a,0xef,0x7a,
    0xf8,0xa5,0x57,0xe9,0x35,0x1a,0xce,0xf2,0x59,0xb8,0x9b,0x40,
    0x35,0x72,0x98,0x77,0x36,0x4c,0x5b,0x6f,0x31,0xfd,0xc2,0x90,
    0x65,0xb8,0x89,0x2e,0x0b,0xbf,0x14,0x5f,0x47,0xcd,0xfd,0xd6,
    0x26,0x00,0xd2,0x53,0x09,0xc3,0xd8,0x47,0x1c,0xae,0x3d,0x78,
    0x92,0x36,0xb5,0xe3,0x67,0x5e,0xb6,0x56,0xa6,0x54,0x16,0xe9,
    0xb7,0xe6,0x6e,0x6b,0xd3,0x3f,0x66,0xfb,0x07,0x9b,0x37,0x07,
    0x26,0x6c,0xe5,0xa2,0xcf,0x93,0x1c,0x79,0x77,0x42,0x53,0x68,
    0x66,0x90,0xaf,0xed,0x63,0x4c,0x11,0xff,0x28,0x15,0x8b,0x0d,
    0x3e,0x64,0xa3,0x4a,0x36,0x8f,0x45,0x65,0x7b,0x52,0x7a,0x0a,
    0xc3,0xed,0x16,0x3d,0xd8,0xb4,0x3c,0x34,0x29,0x7e,0x07,0xf4,
    0xf4,0x6c,0x73,0xc3,0xf9,0x97,0x0c,0xd1,0x1f,0x2c,0x27,0x21,
    0xb3,0xd3,0xa2,0xdf,0xc9,0xef,0x88,0x56,0x6e,0x19,0x75,0x12,
    0xe4,0x8b,0xb2,0x4d,0xa0,0x95,0xd4,0x07,0xea,0x15,0xcc,0x9a,
    0x06,0x93,0xf7,0x46,0x09,0xe1,0x4e,0xea,0x53,0x66,0xa3,0x12,
    0x1d,0xd7,0xf9,0xe2,0xcc,0xb7,0x2c,0x7f,0xbd,0xda,0xdc,0x6d,
    0xbe,0xd4,0xe8,0x78,0x82,0xee,0x51,0x61,0x31,0x8b,0x81,0x32,
    0x78,0x74,0xba,0x3b,0xb3,0x76,0xed,0x4d,0xa8,0x97,0xac,0xc7,
    0x27,0x36,0xb7,0x72,0x66,0xd7,0xb8,0x1e,0xd2,0x52,0x13,0xaf,
    0x88,0x66,0xed,0x02,0x03,0x01,0x00,0x01,0xa3,0x21,0x30,0x1f,
    0x30,0x1d,0x06,0x03,0x55,0x1d,0x0e,0x04,0x16,0x04,0x14,0x41,
    0xf5,0x76,0xbe,0x8a,0x35,0x4e,0xd8,0xa8,0x04,0xfc,0xa1,0x3e,
    0x8d,0xc9,0x7e,0x48,0xbd,0x18,0x6c,0x30,0x0d,0x06,0x09,0x2a,
    0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x0b,0x05,0x00,0x03,0x82,
    0x01,0x01,0x00,0x35,0x89,0x28,0x8e,0x15,0x41,0x6b,0x45,0xaa,
    0xe2,0xd3,0x44,0xb2,0xe9,0x01,0x89,0x26,0x1d,0x11,0xb0,0x3d,
    0x46,0x72,0x96,0x74,0x51,0x30,0xbb,0xdc,0x53,0x18,0x86,0x5f,
    0x0d,0x4b,0xd4,0x9c,0x2f,0xd9,0x9e,0x41,0xca,0xb4,0xf7,0xdb,
    0xc9,0xd6,0x03,0xbe,0xf5,0x1c,0xb4,0x99,0x5d,0xdb,0x4a,0x85,
    0x74,0x2f,0x11,0x80,0x2e,0x9a,0x6f,0x50,0xae,0xd6,0x33,0x9d,
    0x32,0x0a,0x51,0xc6,0x7d,0xab,0x4d,0xe7,0x03,0x9e,0x1e,0xb8,
    0x7b,0xd8,0x5b,0xd0,0x3a,0xf7,0x44,0x68,0x3f,0x37,0xe9,0x84,
    0xb9,0xff,0x72,0x30,0x6e,0x1f,0x52,0xb5,0x89,0x71,0x6d,0x0b,
    0xc9,0x46,0x22,0xca,0x0d,0xd3,0x9f,0xaf,0x6f,0x68,0x4d,0xc2,
    0xbf,0x27,0xcb,0x47,0x98,0x5d,0x47,0xc5,0x40,0x82,0x97,0xae,
    0x87,0xf6,0xbc,0xfe,0xe0,0x1c,0xf6,0x09,0x78,0x7c,0x3e,0x7f,
    0xbb,0x53,0x24,0x09,0xde,0xb9,0x47,0x53,0x2c,0x06,0xc2,0xc3,
    0xe4,0xca,0x27,0x86,0xb7,0x73,0xb1,0xd8,0x4a,0xe7,0x40,0x8b,
    0x14,0x45,0x7c,0x3b,0x54,0x36,0xae,0x04,0x10,0xfb,0xe8,0xca,
    0x44,0x10,0x90,0x60,0x73,0xa6,0x5f,0x52,0x0a,0x5c,0xd4,0x62,
    0x9b,0xe4,0x42,0x49,0x91,0x80,0x2f,0xf1,0xd8,0x60,0x38,0xb1,
    0xc8,0x2a,0x31,0xb8,0x8e,0xbc,0xef,0x05,0x3a,0xb4,0x21,0x53,
    0xc3,0x28,0xbb,0x67,0x2a,0x4b,0xdd,0x12,0xd6,0xdd,0xb6,0xbb,
    0x6e,0xbb,0x0b,0x3a,0xec,0xf9,0xec,0x52,0xb1,0x3a,0x9c,0x68,
    0x74,0xe4,0x39,0xcd,0xb4,0xf5,0x2b,0xb9,0x1a,0x1f,0x20,0x82,
    0x33,0x45,0x08,0x97,0x45,0xfa,0x8e,
};

static const unsigned char apk_signature_der_sha256[32] = {
    0x8d,0x99,0x75,0xb3,0x08,0xc7,0x9d,0x52,0xb1,0x70,0x39,0x00,
    0xaf,0xe3,0x5f,0x39,0xaf,0xd5,0xd4,0x0a,0x4d,0x01,0xc8,0xd2,
    0x90,0x64,0x28,0x80,0x78,0xa7,0xaf,0x7d,
};

#define IS(method_name, signature) \
    (!strcmp(method, (method_name)) && !strcmp(sig, (signature)))

static bool signature_class_ok(struct dvm_class *c)
{
    if (!c || !c->external || !c->ifields || c->nifields != 2 || c->islots != 2)
        return false;
    return !strcmp(c->ifields[0].name, "bytes") &&
           !strcmp(c->ifields[0].type, "[B") &&
           c->ifields[0].slot == 0 &&
           !strcmp(c->ifields[1].name, "mSignature") &&
           !strcmp(c->ifields[1].type, "[B") &&
           c->ifields[1].slot == 1;
}

static bool define_signature_class(struct dvm *vm)
{
    struct dvm_class *c = dvm_find_class_or_external(vm, APK_SIGNATURE_CLASS);
    if (!c || !c->external)
        return false;
    if (c->ifields || c->nifields || c->islots)
        return signature_class_ok(c);
    if (c->super && c->super->islots)
        return false;

    struct dvm_field *fields = calloc(2, sizeof *fields);
    if (!fields)
        return false;
    static const char *names[2] = { "bytes", "mSignature" };
    for (int i = 0; i < 2; ++i) {
        fields[i].cls = c;
        fields[i].name = names[i];
        fields[i].type = "[B";
        fields[i].kind = dvm__kind_of("[B");
        fields[i].width = 1;
        fields[i].access = 1;
        fields[i].slot = (uint16_t)i;
    }
    c->ifields = fields;
    c->nifields = 2;
    c->islots = 2;
    return true;
}

bool apk_signature_vm_start(struct dvm *vm)
{
    if (!vm)
        return false;
    if (sizeof apk_signature_der_bytes != APK_SIGNATURE_DER_LEN)
        return false;

    unsigned char hash[32];
    if (mbedtls_sha256(apk_signature_der_bytes, sizeof apk_signature_der_bytes,
                       hash, 0) != 0)
        return false;
    if (memcmp(hash, apk_signature_der_sha256, sizeof hash) != 0)
        return false;

    return define_signature_class(vm);
}

const unsigned char *apk_signature_der(size_t *len)
{
    if (len)
        *len = sizeof apk_signature_der_bytes;
    return apk_signature_der_bytes;
}

static struct dvm_object *signature_object(struct dvm *vm, dvm_ref ref)
{
    struct dvm_object *o = dvm__obj(vm, ref);
    if (!o || !o->cls || !o->cls->name || !o->slots)
        return NULL;
    if (strcmp(o->cls->name, APK_SIGNATURE_CLASS))
        return NULL;
    return o;
}

/* slot 0 是任务要求的 bytes；slot 1 是 AOSP 当前字段名 mSignature 的别名。
 * 两者在创建时指向同一个 byte[]，正常读取以 slot 0 为准。 */
static dvm_ref signature_payload(struct dvm_object *o)
{
    if (!o || !o->slots)
        return 0;
    return o->slots[0].l ? o->slots[0].l : o->slots[1].l;
}

dvm_ref apk_signature_new(struct dvm *vm, const unsigned char *der, size_t len)
{
    if (!vm)
        return 0;
    if (!der) {
        dvm__throw(vm, "java/lang/NullPointerException", "Signature bytes");
        return 0;
    }
    if (len > UINT32_MAX) {
        dvm__throw(vm, "java/lang/IllegalArgumentException", "Signature bytes too long");
        return 0;
    }

    struct dvm_class *cls = dvm_find_class_or_external(vm, APK_SIGNATURE_CLASS);
    if (!cls) {
        dvm__throw(vm, "java/lang/IllegalStateException", "Signature class missing");
        return 0;
    }

    dvm_ref sig = dvm_new_object(vm, cls);
    if (!sig) {
        dvm__throw(vm, "java/lang/OutOfMemoryError", "Signature");
        return 0;
    }

    dvm_ref bytes = dvm_new_array(vm, 'B', "B", (uint32_t)len);
    if (!bytes) {
        dvm__discard_temporary(vm, sig);
        dvm__throw(vm, "java/lang/OutOfMemoryError", "Signature bytes");
        return 0;
    }
    if (len)
        memcpy(dvm_array_data(vm, bytes), der, len);

    struct dvm_object *o = dvm__obj(vm, sig);
    if (!o || !o->slots) {
        dvm__discard_temporary(vm, sig);
        dvm__discard_temporary(vm, bytes);
        dvm__throw(vm, "java/lang/IllegalStateException", "Signature slots missing");
        return 0;
    }
    o->slots[0].l = bytes;
    o->slots[1].l = bytes;
    return sig;
}

static bool signature_to_byte_array(struct dvm *vm, struct dvm_object *o,
                                    union dvm_value *out)
{
    dvm_ref src = signature_payload(o);
    if (!src) {
        dvm__throw(vm, "java/lang/NullPointerException", "Signature bytes");
        return true;
    }
    uint32_t n = dvm_array_length(vm, src);
    dvm_ref copy = dvm_new_array(vm, 'B', "B", n);
    if (!copy) {
        dvm__throw(vm, "java/lang/OutOfMemoryError", "Signature copy");
        return true;
    }
    if (n) {
        void *dst = dvm_array_data(vm, copy);
        const void *from = dvm_array_data(vm, src);
        if (!dst || !from) {
            dvm__discard_temporary(vm, copy);
            dvm__throw(vm, "java/lang/IllegalStateException", "Signature array data");
            return true;
        }
        memcpy(dst, from, n);
    }
    out->l = copy;
    return true;
}

static bool signature_to_chars_string(struct dvm *vm, struct dvm_object *o,
                                      union dvm_value *out)
{
    dvm_ref src = signature_payload(o);
    if (!src) {
        dvm__throw(vm, "java/lang/NullPointerException", "Signature bytes");
        return true;
    }
    uint32_t n = dvm_array_length(vm, src);
    if ((size_t)n > (SIZE_MAX - 1) / 2) {
        dvm__throw(vm, "java/lang/OutOfMemoryError", "Signature text");
        return true;
    }
    char *text = malloc((size_t)n * 2 + 1);
    if (!text) {
        dvm__throw(vm, "java/lang/OutOfMemoryError", "Signature text");
        return true;
    }
    const unsigned char *bytes = n ? dvm_array_data(vm, src) : NULL;
    if (n && !bytes) {
        free(text);
        dvm__throw(vm, "java/lang/IllegalStateException", "Signature array data");
        return true;
    }
    static const char hex[] = "0123456789abcdef";
    for (uint32_t i = 0; i < n; ++i) {
        text[i * 2] = hex[bytes[i] >> 4];
        text[i * 2 + 1] = hex[bytes[i] & 0x0f];
    }
    text[(size_t)n * 2] = '\0';
    out->l = dvm_new_string_n(vm, text, (size_t)n * 2);
    free(text);
    if (!out->l) {
        dvm__throw(vm, "java/lang/OutOfMemoryError", "Signature text");
        return true;
    }
    return true;
}

static bool signature_equals(struct dvm *vm, struct dvm_object *self,
                             dvm_ref other_ref, union dvm_value *out)
{
    if (!other_ref) {
        out->i = 0;
        return true;
    }
    struct dvm_object *other = signature_object(vm, other_ref);
    if (!other) {
        out->i = 0;
        return true;
    }
    dvm_ref a = signature_payload(self);
    dvm_ref b = signature_payload(other);
    if (!a || !b) {
        out->i = (a == b);
        return true;
    }
    uint32_t an = dvm_array_length(vm, a);
    uint32_t bn = dvm_array_length(vm, b);
    if (an != bn) {
        out->i = 0;
        return true;
    }
    if (an == 0) {
        out->i = 1;
        return true;
    }
    const void *ad = dvm_array_data(vm, a);
    const void *bd = dvm_array_data(vm, b);
    out->i = ad && bd && !memcmp(ad, bd, an);
    return true;
}

static bool signature_hash_code(struct dvm *vm, struct dvm_object *o,
                                union dvm_value *out)
{
    dvm_ref src = signature_payload(o);
    if (!src) {
        dvm__throw(vm, "java/lang/NullPointerException", "Signature bytes");
        return true;
    }
    uint32_t n = dvm_array_length(vm, src);
    const unsigned char *bytes = n ? dvm_array_data(vm, src) : NULL;
    if (n && !bytes) {
        dvm__throw(vm, "java/lang/IllegalStateException", "Signature array data");
        return true;
    }
    uint32_t hash = 1;
    for (uint32_t i = 0; i < n; ++i)
        hash = hash * 31u + bytes[i];
    out->i = (int32_t)hash;
    return true;
}

bool apk_signature_call(struct dvm *vm, const char *class_name, const char *method,
                        const char *sig, dvm_ref self,
                        const union dvm_value *args, int nargs,
                        union dvm_value *out)
{
    if (!vm || !class_name || strcmp(class_name, APK_SIGNATURE_CLASS))
        return false;
    if (!method || !sig)
        return false;

    if (IS("<init>", "([B)V")) {
        if (nargs != 1)
            return false;
        struct dvm_object *o = signature_object(vm, self);
        if (!o) {
            dvm__throw(vm, "java/lang/NullPointerException", "Signature receiver");
            return true;
        }
        dvm_ref src = args[0].l;
        if (!src) {
            dvm__throw(vm, "java/lang/NullPointerException", "Signature bytes");
            return true;
        }
        uint32_t n = dvm_array_length(vm, src);
        dvm_ref copy = dvm_new_array(vm, 'B', "B", n);
        if (!copy) {
            dvm__throw(vm, "java/lang/OutOfMemoryError", "Signature copy");
            return true;
        }
        if (n) {
            void *dst = dvm_array_data(vm, copy);
            const void *from = dvm_array_data(vm, src);
            if (!dst || !from) {
                dvm__discard_temporary(vm, copy);
                dvm__throw(vm, "java/lang/IllegalStateException", "Signature array data");
                return true;
            }
            memcpy(dst, from, n);
        }
        o->slots[0].l = copy;
        o->slots[1].l = copy;
        return true;
    }

    if (IS("toByteArray", "()[B")) {
        if (nargs != 0)
            return false;
        struct dvm_object *o = signature_object(vm, self);
        if (!o) {
            dvm__throw(vm, "java/lang/NullPointerException", "Signature receiver");
            return true;
        }
        return signature_to_byte_array(vm, o, out);
    }

    if (IS("toCharsString", "()Ljava/lang/String;")) {
        if (nargs != 0)
            return false;
        struct dvm_object *o = signature_object(vm, self);
        if (!o) {
            dvm__throw(vm, "java/lang/NullPointerException", "Signature receiver");
            return true;
        }
        return signature_to_chars_string(vm, o, out);
    }

    if (IS("equals", "(Ljava/lang/Object;)Z")) {
        if (nargs != 1)
            return false;
        struct dvm_object *o = signature_object(vm, self);
        if (!o) {
            dvm__throw(vm, "java/lang/NullPointerException", "Signature receiver");
            return true;
        }
        if (self == args[0].l) {
            out->i = 1;
            return true;
        }
        return signature_equals(vm, o, args[0].l, out);
    }

    if (IS("hashCode", "()I")) {
        if (nargs != 0)
            return false;
        struct dvm_object *o = signature_object(vm, self);
        if (!o) {
            dvm__throw(vm, "java/lang/NullPointerException", "Signature receiver");
            return true;
        }
        return signature_hash_code(vm, o, out);
    }

    return false;
}
