#include "jce_runtime.h"
#include "dvm/dvm_internal.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mbedtls/aes.h"

extern int sdk_trace_fprintf(FILE *, const char *, ...);

#define JCE_BLOCK_SIZE 16u
#define JCE_MAX_KEY 32u
#define JCE_MAX_TRANSFORMATION 31u

/* ------------------------------------------------------------------ */
/* Object payloads.  A payload is recognised only by its destructor, so a
 * SecretKeySpec can never be confused with an IvParameterSpec or a Cipher. */
struct cipher_payload {
    char transformation[JCE_MAX_TRANSFORMATION + 1];
    unsigned char key[JCE_MAX_KEY];
    unsigned char iv[JCE_BLOCK_SIZE];
    size_t key_length;
    bool ecb;
    bool encrypt;
    bool initialized;
};

struct secret_key_payload {
    unsigned char *key;
    size_t key_length;
    char *algorithm;
};

struct iv_payload {
    unsigned char *iv;
    size_t iv_length;
};

static struct dvm *active_vm;

static bool is_call(const char *method, const char *signature,
                    const char *wanted_method, const char *wanted_signature)
{
    return method && signature && !strcmp(method, wanted_method) &&
           !strcmp(signature, wanted_signature);
}

static bool fail_message(struct dvm *vm, const char *type, const char *message)
{
    dvm__throw(vm, type, "%s", message ? message : "JCE runtime failure");
    return true;
}

static bool failf1(struct dvm *vm, const char *type, const char *format,
                   const char *value)
{
    dvm__throw(vm, type, format, value ? value : "");
    return true;
}

static void *payload(struct dvm *vm, dvm_ref ref, void (*destroy)(void *))
{
    struct dvm_object *object = dvm__obj(vm, ref);
    if (!object || object->data_destroy != destroy) return NULL;
    return object->data;
}

static bool attach_payload(struct dvm *vm, dvm_ref ref, void *data,
                           void (*destroy)(void *))
{
    struct dvm_object *object = dvm__obj(vm, ref);
    if (!object || object->data) return false;
    object->data = data;
    object->data_destroy = destroy;
    return true;
}

static bool byte_array_view(struct dvm *vm, dvm_ref ref,
                            const unsigned char **data, uint32_t *length)
{
    struct dvm_object *object = dvm__obj(vm, ref);
    if (!object || object->kind != DVM_OBJ_ARRAY || object->elem_kind != 'B')
        return false;
    *data = object->data;
    *length = object->length;
    return true;
}

static dvm_ref new_byte_array(struct dvm *vm, const unsigned char *data,
                              uint32_t length)
{
    dvm_ref ref = dvm_new_array(vm, 'B', "B", length);
    if (!ref) return 0;
    if (length && data) memcpy(dvm_array_data(vm, ref), data, length);
    return ref;
}

static void cipher_payload_drop(void *pointer)
{
    free(pointer);
}

static void secret_key_payload_drop(void *pointer)
{
    struct secret_key_payload *state = pointer;
    if (!state) return;
    free(state->key);
    free(state->algorithm);
    free(state);
}

static void iv_payload_drop(void *pointer)
{
    struct iv_payload *state = pointer;
    if (!state) return;
    free(state->iv);
    free(state);
}

/* ------------------------------------------------------------------ */
/* Transformation parsing.  Java compares algorithm, mode and padding names
 * case-insensitively; only the three-part AES forms below are implemented. */
static bool slice_equal_ci(const char *begin, size_t length, const char *word)
{
    size_t word_length = strlen(word);
    if (length != word_length) return false;
    for (size_t i = 0; i < length; ++i) {
        unsigned char left = (unsigned char)begin[i];
        unsigned char right = (unsigned char)word[i];
        if (left >= 'A' && left <= 'Z') left = (unsigned char)(left - 'A' + 'a');
        if (right >= 'A' && right <= 'Z') right = (unsigned char)(right - 'A' + 'a');
        if (left != right) return false;
    }
    return true;
}

static bool starts_with_ci(const char *text, const char *prefix)
{
    if (!text || !prefix) return false;
    size_t length = strlen(prefix);
    return strlen(text) >= length && slice_equal_ci(text, length, prefix);
}

enum transformation_result {
    TRANSFORMATION_OK,
    TRANSFORMATION_UNKNOWN,
    TRANSFORMATION_BAD_PADDING
};

static enum transformation_result parse_transformation(const char *text,
                                                       bool *ecb)
{
    if (!text || !*text) return TRANSFORMATION_UNKNOWN;
    const char *first = strchr(text, '/');
    if (!first) return TRANSFORMATION_UNKNOWN;
    const char *second = strchr(first + 1, '/');
    if (!second || strchr(second + 1, '/')) return TRANSFORMATION_UNKNOWN;
    if (!slice_equal_ci(text, (size_t)(first - text), "AES"))
        return TRANSFORMATION_UNKNOWN;
    bool cbc = slice_equal_ci(first + 1, (size_t)(second - first - 1), "CBC");
    bool ecb_mode = slice_equal_ci(first + 1, (size_t)(second - first - 1), "ECB");
    if (!cbc && !ecb_mode) return TRANSFORMATION_UNKNOWN;
    const char *padding = second + 1;
    size_t padding_length = strlen(padding);
    if (!slice_equal_ci(padding, padding_length, "PKCS5PADDING") &&
        !slice_equal_ci(padding, padding_length, "PKCS7PADDING"))
        return TRANSFORMATION_BAD_PADDING;
    *ecb = ecb_mode;
    return TRANSFORMATION_OK;
}

/* ------------------------------------------------------------------ */
/* AES primitive.  The saved IV is copied for every operation, matching
 * Cipher.doFinal's reset-to-initial-state semantics. */
static bool aes_crypt(const struct cipher_payload *state,
                      const unsigned char *input, size_t length,
                      unsigned char *output, bool encrypt)
{
    mbedtls_aes_context context;
    int result;
    mbedtls_aes_init(&context);
    result = encrypt
        ? mbedtls_aes_setkey_enc(&context, state->key,
                                 (unsigned int)(state->key_length * 8u))
        : mbedtls_aes_setkey_dec(&context, state->key,
                                 (unsigned int)(state->key_length * 8u));
    if (result == 0) {
        if (state->ecb) {
            for (size_t offset = 0; offset < length; offset += JCE_BLOCK_SIZE) {
                result = mbedtls_aes_crypt_ecb(
                    &context,
                    encrypt ? MBEDTLS_AES_ENCRYPT : MBEDTLS_AES_DECRYPT,
                    input + offset, output + offset);
                if (result != 0) break;
            }
        } else {
            unsigned char iv[JCE_BLOCK_SIZE];
            memcpy(iv, state->iv, sizeof(iv));
            result = mbedtls_aes_crypt_cbc(
                &context,
                encrypt ? MBEDTLS_AES_ENCRYPT : MBEDTLS_AES_DECRYPT,
                length, iv, input, output);
        }
    }
    mbedtls_aes_free(&context);
    return result == 0;
}

/* ------------------------------------------------------------------ */
/* javax.crypto.Cipher */
static bool cipher_get_instance(struct dvm *vm, const union dvm_value *args,
                                int nargs, union dvm_value *out)
{
    if (nargs != 1) return false;
    const char *transformation =
        args[0].l ? dvm_string_utf8(vm, args[0].l) : NULL;
    if (!transformation || !*transformation)
        return fail_message(vm, "java/security/NoSuchAlgorithmException",
                            "Null or empty transformation");
    bool ecb = false;
    enum transformation_result parsed =
        parse_transformation(transformation, &ecb);
    if (parsed == TRANSFORMATION_BAD_PADDING)
        return failf1(vm, "java/security/NoSuchPaddingException",
                      "Unsupported padding in %s", transformation);
    if (parsed != TRANSFORMATION_OK)
        return failf1(vm, "java/security/NoSuchAlgorithmException",
                      "Unsupported transformation %s", transformation);
    size_t length = strlen(transformation);
    if (length > JCE_MAX_TRANSFORMATION)
        return failf1(vm, "java/security/NoSuchAlgorithmException",
                      "Unsupported transformation %s", transformation);

    struct dvm_class *klass =
        dvm_find_class_or_external(vm, "javax/crypto/Cipher");
    dvm_ref object = klass ? dvm_new_object(vm, klass) : 0;
    if (!object)
        return fail_message(vm, "java/lang/OutOfMemoryError", "Cipher");
    struct cipher_payload *state = calloc(1, sizeof(*state));
    if (!state)
        return fail_message(vm, "java/lang/OutOfMemoryError", "Cipher state");
    state->ecb = ecb;
    memcpy(state->transformation, transformation, length + 1);
    if (!attach_payload(vm, object, state, cipher_payload_drop)) {
        cipher_payload_drop(state);
        return fail_message(vm, "java/lang/IllegalStateException",
                            "Cipher object already initialized");
    }
    out->l = object;
    return true;
}

static bool cipher_init(struct dvm *vm, dvm_ref self,
                        const union dvm_value *args, int nargs,
                        bool has_parameters)
{
    struct cipher_payload *state = payload(vm, self, cipher_payload_drop);
    if (!state) return false;
    (void)nargs; /* arity is fixed by the matched signature */

    int32_t mode = args[0].i;
    if (mode != 1 && mode != 2)
        return fail_message(vm, "java/lang/IllegalArgumentException",
                            "Invalid operation mode");
    struct secret_key_payload *key =
        payload(vm, args[1].l, secret_key_payload_drop);
    if (!key)
        return fail_message(vm, "java/security/InvalidKeyException",
                            "Key is not a SecretKeySpec");
    if (key->key_length != 16u && key->key_length != 24u &&
        key->key_length != 32u)
        return fail_message(vm, "java/security/InvalidKeyException",
                            "Invalid AES key length");
    if (key->algorithm && !starts_with_ci(key->algorithm, "AES"))
        return fail_message(vm, "java/security/InvalidKeyException",
                            "Wrong key algorithm");

    unsigned char iv[JCE_BLOCK_SIZE];
    memset(iv, 0, sizeof(iv));
    if (state->ecb) {
        /* ECB ignores an IV, as required by the guest's Cipher.init(1, key)
         * path.  A non-IV parameter object is still a type error. */
        if (has_parameters && args[2].l &&
            !payload(vm, args[2].l, iv_payload_drop))
            return fail_message(vm,
                                "java/security/InvalidAlgorithmParameterException",
                                "ECB mode cannot use non-IV parameters");
    } else {
        if (!has_parameters || !args[2].l)
            return fail_message(vm,
                                "java/security/InvalidAlgorithmParameterException",
                                "CBC mode requires an IvParameterSpec");
        struct iv_payload *parameters =
            payload(vm, args[2].l, iv_payload_drop);
        if (!parameters)
            return fail_message(vm,
                                "java/security/InvalidAlgorithmParameterException",
                                "Wrong parameter type: IvParameterSpec required");
        if (parameters->iv_length != JCE_BLOCK_SIZE)
            return fail_message(vm,
                                "java/security/InvalidAlgorithmParameterException",
                                "Wrong IV length: must be 16 bytes long");
        memcpy(iv, parameters->iv, JCE_BLOCK_SIZE);
    }

    memcpy(state->key, key->key, key->key_length);
    state->key_length = key->key_length;
    memcpy(state->iv, iv, sizeof(iv));
    state->encrypt = mode == 1;
    state->initialized = true;
    return true;
}

static bool cipher_do_final(struct dvm *vm, dvm_ref self,
                            const union dvm_value *args, int nargs,
                            union dvm_value *out)
{
    if (nargs != 1) return false;
    struct cipher_payload *state = payload(vm, self, cipher_payload_drop);
    if (!state) return false;
    if (!state->initialized)
        return fail_message(vm, "java/lang/IllegalStateException",
                            "Cipher not initialized");
    if (!args[0].l)
        return fail_message(vm, "java/lang/NullPointerException",
                            "data == null");
    const unsigned char *input = NULL;
    uint32_t input_length = 0;
    if (!byte_array_view(vm, args[0].l, &input, &input_length))
        return fail_message(vm, "java/lang/IllegalArgumentException",
                            "input is not a byte array");

    if (state->encrypt) {
        if (input_length > UINT32_MAX - JCE_BLOCK_SIZE)
            return fail_message(vm, "javax/crypto/IllegalBlockSizeException",
                                "Input too large");
        uint32_t padding =
            (uint32_t)(JCE_BLOCK_SIZE - (input_length % JCE_BLOCK_SIZE));
        size_t output_length = (size_t)input_length + padding;
        unsigned char *plain = malloc(output_length);
        unsigned char *cipher = malloc(output_length);
        if (!plain || !cipher) {
            free(plain);
            free(cipher);
            return fail_message(vm, "java/lang/OutOfMemoryError",
                                "Cipher output");
        }
        if (input_length) memcpy(plain, input, input_length);
        memset(plain + input_length, (int)padding, padding);
        bool okay = aes_crypt(state, plain, output_length, cipher, true);
        memset(plain, 0, output_length);
        free(plain);
        if (!okay) {
            free(cipher);
            return fail_message(vm, "java/lang/IllegalStateException",
                                "AES encryption failed");
        }
        dvm_ref result =
            new_byte_array(vm, cipher, (uint32_t)output_length);
        memset(cipher, 0, output_length);
        free(cipher);
        if (!result)
            return fail_message(vm, "java/lang/OutOfMemoryError",
                                "Cipher result");
        out->l = result;
        return true;
    }

    /* Empty ciphertext: Java returns an empty byte array, not a padding
     * error, because no final block exists. */
    if (input_length == 0) {
        out->l = new_byte_array(vm, NULL, 0);
        return out->l != 0;
    }
    if (input_length % JCE_BLOCK_SIZE != 0)
        return fail_message(vm, "javax/crypto/IllegalBlockSizeException",
                            "Input length must be multiple of 16 when decrypting with padded cipher");

    unsigned char *plain = malloc(input_length);
    if (!plain)
        return fail_message(vm, "java/lang/OutOfMemoryError",
                            "Cipher output");
    bool okay = aes_crypt(state, input, input_length, plain, false);
    if (!okay) {
        free(plain);
        return fail_message(vm, "java/lang/IllegalStateException",
                            "AES decryption failed");
    }
    unsigned int padding = plain[input_length - 1];
    bool bad_padding = padding == 0 || padding > JCE_BLOCK_SIZE ||
                       padding > input_length;
    for (uint32_t i = 0; !bad_padding && i < padding; ++i) {
        if (plain[input_length - 1 - i] != padding) bad_padding = true;
    }
    if (bad_padding) {
        memset(plain, 0, input_length);
        free(plain);
        return fail_message(vm, "javax/crypto/BadPaddingException",
                            "Given final block not properly padded");
    }
    uint32_t output_length = input_length - padding;
    dvm_ref result = new_byte_array(vm, plain, output_length);
    memset(plain, 0, input_length);
    free(plain);
    if (!result)
        return fail_message(vm, "java/lang/OutOfMemoryError",
                            "Cipher result");
    out->l = result;
    return true;
}

static bool cipher_get_block_size(struct dvm *vm, dvm_ref self,
                                  union dvm_value *out)
{
    struct cipher_payload *state = payload(vm, self, cipher_payload_drop);
    if (!state) return false;
    out->i = (int32_t)JCE_BLOCK_SIZE;
    return true;
}
/* nargs is validated by cipher_call; the helper keeps the common shape. */

static bool cipher_get_output_size(struct dvm *vm, dvm_ref self,
                                   const union dvm_value *args, int nargs,
                                   union dvm_value *out)
{
    if (nargs != 1) return false;
    struct cipher_payload *state = payload(vm, self, cipher_payload_drop);
    if (!state) return false;
    if (!state->initialized)
        return fail_message(vm, "java/lang/IllegalStateException",
                            "Cipher not initialized");
    int32_t input_length = args[0].i;
    if (input_length < 0)
        return fail_message(vm, "java/lang/IllegalArgumentException",
                            "Negative input length");
    int64_t output_length;
    if (state->encrypt)
        output_length = (int64_t)input_length + JCE_BLOCK_SIZE -
                        (input_length % JCE_BLOCK_SIZE);
    else
        output_length = input_length;
    if (output_length > INT32_MAX)
        return fail_message(vm, "java/lang/IllegalArgumentException",
                            "Input length too large");
    out->i = (int32_t)output_length;
    return true;
}

static bool cipher_get_algorithm(struct dvm *vm, dvm_ref self,
                                 union dvm_value *out)
{
    struct cipher_payload *state = payload(vm, self, cipher_payload_drop);
    if (!state) return false;
    out->l = dvm_new_string(vm, state->transformation);
    if (!out->l)
        return fail_message(vm, "java/lang/OutOfMemoryError",
                            "Cipher algorithm");
    return true;
}

static bool cipher_call(struct dvm *vm, const char *method, const char *sig,
                        dvm_ref self, const union dvm_value *args, int nargs,
                        union dvm_value *out)
{
    if (is_call(method, sig, "getInstance",
                "(Ljava/lang/String;)Ljavax/crypto/Cipher;") && nargs == 1)
        return cipher_get_instance(vm, args, nargs, out);
    if (!self) return false;
    if (is_call(method, sig, "init", "(ILjava/security/Key;)V") &&
        nargs == 2)
        return cipher_init(vm, self, args, nargs, false);
    if (is_call(method, sig, "init",
                "(ILjava/security/Key;Ljava/security/spec/AlgorithmParameterSpec;)V") &&
        nargs == 3)
        return cipher_init(vm, self, args, nargs, true);
    if (is_call(method, sig, "doFinal", "([B)[B") && nargs == 1)
        return cipher_do_final(vm, self, args, nargs, out);
    if (is_call(method, sig, "getBlockSize", "()I") && nargs == 0)
        return cipher_get_block_size(vm, self, out);
    if (is_call(method, sig, "getOutputSize", "(I)I") && nargs == 1)
        return cipher_get_output_size(vm, self, args, nargs, out);
    if (is_call(method, sig, "getAlgorithm", "()Ljava/lang/String;") &&
        nargs == 0)
        return cipher_get_algorithm(vm, self, out);
    return false;
}

/* ------------------------------------------------------------------ */
/* javax.crypto.spec.SecretKeySpec */
static bool secret_key_init(struct dvm *vm, dvm_ref self,
                            const union dvm_value *args, int nargs)
{
    if (nargs != 2) return false;
    if (payload(vm, self, secret_key_payload_drop))
        return fail_message(vm, "java/lang/IllegalStateException",
                            "SecretKeySpec already initialized");
    if (!args[0].l || !args[1].l)
        return fail_message(vm, "java/lang/IllegalArgumentException",
                            "Missing argument");
    const unsigned char *key = NULL;
    uint32_t key_length = 0;
    if (!byte_array_view(vm, args[0].l, &key, &key_length))
        return fail_message(vm, "java/lang/IllegalArgumentException",
                            "key is not a byte array");
    if (key_length == 0)
        return fail_message(vm, "java/lang/IllegalArgumentException",
                            "Empty key");
    const char *algorithm = dvm_string_utf8(vm, args[1].l);
    if (!algorithm)
        return fail_message(vm, "java/lang/IllegalArgumentException",
                            "Missing argument");
    uint32_t algorithm_length = dvm_string_utf8_length(vm, args[1].l);

    struct secret_key_payload *state = calloc(1, sizeof(*state));
    if (!state)
        return fail_message(vm, "java/lang/OutOfMemoryError",
                            "SecretKeySpec state");
    state->key = malloc(key_length);
    state->algorithm = malloc((size_t)algorithm_length + 1u);
    if (!state->key || !state->algorithm) {
        secret_key_payload_drop(state);
        return fail_message(vm, "java/lang/OutOfMemoryError",
                            "SecretKeySpec payload");
    }
    memcpy(state->key, key, key_length);
    memcpy(state->algorithm, algorithm, algorithm_length);
    state->algorithm[algorithm_length] = '\0';
    state->key_length = key_length;
    if (!attach_payload(vm, self, state, secret_key_payload_drop)) {
        secret_key_payload_drop(state);
        return fail_message(vm, "java/lang/IllegalStateException",
                            "SecretKeySpec already initialized");
    }
    return true;
}

static bool secret_key_get_encoded(struct dvm *vm, dvm_ref self,
                                   union dvm_value *out)
{
    struct secret_key_payload *state =
        payload(vm, self, secret_key_payload_drop);
    if (!state) return false;
    out->l = new_byte_array(vm, state->key, (uint32_t)state->key_length);
    if (!out->l)
        return fail_message(vm, "java/lang/OutOfMemoryError",
                            "SecretKeySpec encoded key");
    return true;
}

static bool secret_key_get_algorithm(struct dvm *vm, dvm_ref self,
                                     union dvm_value *out)
{
    struct secret_key_payload *state =
        payload(vm, self, secret_key_payload_drop);
    if (!state) return false;
    out->l = dvm_new_string(vm, state->algorithm ? state->algorithm : "");
    if (!out->l)
        return fail_message(vm, "java/lang/OutOfMemoryError",
                            "SecretKeySpec algorithm");
    return true;
}

static bool secret_key_get_format(struct dvm *vm, dvm_ref self,
                                  union dvm_value *out)
{
    if (!payload(vm, self, secret_key_payload_drop)) return false;
    out->l = dvm_new_string(vm, "RAW");
    if (!out->l)
        return fail_message(vm, "java/lang/OutOfMemoryError",
                            "SecretKeySpec format");
    return true;
}

static bool secret_key_call(struct dvm *vm, const char *method,
                            const char *sig, dvm_ref self,
                            const union dvm_value *args, int nargs,
                            union dvm_value *out)
{
    if (is_call(method, sig, "<init>", "([BLjava/lang/String;)V") &&
        nargs == 2)
        return secret_key_init(vm, self, args, nargs);
    if (!self) return false;
    if (is_call(method, sig, "getEncoded", "()[B") && nargs == 0)
        return secret_key_get_encoded(vm, self, out);
    if (is_call(method, sig, "getAlgorithm", "()Ljava/lang/String;") &&
        nargs == 0)
        return secret_key_get_algorithm(vm, self, out);
    if (is_call(method, sig, "getFormat", "()Ljava/lang/String;") &&
        nargs == 0)
        return secret_key_get_format(vm, self, out);
    return false;
}

/* ------------------------------------------------------------------ */
/* javax.crypto.spec.IvParameterSpec */
static bool iv_init(struct dvm *vm, dvm_ref self,
                    const union dvm_value *args, int nargs)
{
    if (nargs != 1) return false;
    if (payload(vm, self, iv_payload_drop))
        return fail_message(vm, "java/lang/IllegalStateException",
                            "IvParameterSpec already initialized");
    if (!args[0].l)
        return fail_message(vm, "java/lang/NullPointerException",
                            "iv == null");
    const unsigned char *iv = NULL;
    uint32_t iv_length = 0;
    if (!byte_array_view(vm, args[0].l, &iv, &iv_length))
        return fail_message(vm, "java/lang/IllegalArgumentException",
                            "iv is not a byte array");
    struct iv_payload *state = calloc(1, sizeof(*state));
    if (!state)
        return fail_message(vm, "java/lang/OutOfMemoryError",
                            "IvParameterSpec state");
    state->iv = malloc(iv_length ? iv_length : 1u);
    if (!state->iv) {
        iv_payload_drop(state);
        return fail_message(vm, "java/lang/OutOfMemoryError",
                            "IvParameterSpec payload");
    }
    if (iv_length) memcpy(state->iv, iv, iv_length);
    state->iv_length = iv_length;
    if (!attach_payload(vm, self, state, iv_payload_drop)) {
        iv_payload_drop(state);
        return fail_message(vm, "java/lang/IllegalStateException",
                            "IvParameterSpec already initialized");
    }
    return true;
}

static bool iv_get_iv(struct dvm *vm, dvm_ref self, union dvm_value *out)
{
    struct iv_payload *state = payload(vm, self, iv_payload_drop);
    if (!state) return false;
    out->l = new_byte_array(vm, state->iv, (uint32_t)state->iv_length);
    if (!out->l)
        return fail_message(vm, "java/lang/OutOfMemoryError",
                            "IvParameterSpec IV");
    return true;
}

static bool iv_call(struct dvm *vm, const char *method, const char *sig,
                    dvm_ref self, const union dvm_value *args, int nargs,
                    union dvm_value *out)
{
    if (is_call(method, sig, "<init>", "([B)V") && nargs == 1)
        return iv_init(vm, self, args, nargs);
    if (!self) return false;
    if (is_call(method, sig, "getIV", "()[B") && nargs == 0)
        return iv_get_iv(vm, self, out);
    return false;
}

/* ------------------------------------------------------------------ */
/* Class registration */
static void declare_class(struct dvm *vm, const char *name, const char *super,
                          const char *const *interfaces, int interface_count,
                          bool is_interface)
{
    struct dvm_class *klass = dvm_find_class_or_external(vm, name);
    struct dvm_class *parent =
        super ? dvm_find_class_or_external(vm, super) : NULL;
    if (!klass || !klass->external) return;
    if (parent) klass->super = parent;
    klass->is_interface = is_interface;
    if (interface_count && !klass->ifaces) {
        klass->ifaces = calloc((size_t)interface_count, sizeof(*klass->ifaces));
        if (!klass->ifaces) return;
        klass->nifaces = interface_count;
        for (int i = 0; i < interface_count; ++i)
            klass->ifaces[i] = dvm_find_class_or_external(vm, interfaces[i]);
    }
}

static const char *const secret_key_spec_interfaces[] = {
    "java/security/spec/KeySpec",
    "javax/crypto/SecretKey"
};

static const char *const iv_parameter_spec_interfaces[] = {
    "java/security/spec/AlgorithmParameterSpec"
};

void jce_runtime_vm_start(struct dvm *vm)
{
    if (!vm) return;
    active_vm = vm;
    declare_class(vm, "javax/crypto/Cipher", "java/lang/Object", NULL, 0,
                  false);
    declare_class(vm, "javax/crypto/spec/SecretKeySpec", "java/lang/Object",
                  secret_key_spec_interfaces,
                  (int)(sizeof(secret_key_spec_interfaces) /
                        sizeof(secret_key_spec_interfaces[0])),
                  false);
    declare_class(vm, "javax/crypto/spec/IvParameterSpec", "java/lang/Object",
                  iv_parameter_spec_interfaces,
                  (int)(sizeof(iv_parameter_spec_interfaces) /
                        sizeof(iv_parameter_spec_interfaces[0])),
                  false);
    declare_class(vm, "java/security/Key", NULL, NULL, 0, true);
    declare_class(vm, "java/security/spec/KeySpec", NULL, NULL, 0, true);
    declare_class(vm, "java/security/spec/AlgorithmParameterSpec", NULL, NULL,
                  0, true);
    declare_class(vm, "javax/crypto/SecretKey", "java/security/Key", NULL, 0,
                  true);
    declare_class(vm, "java/security/GeneralSecurityException",
                  "java/lang/Exception", NULL, 0, false);
    declare_class(vm, "java/security/NoSuchAlgorithmException",
                  "java/security/GeneralSecurityException", NULL, 0, false);
    declare_class(vm, "java/security/NoSuchPaddingException",
                  "java/security/GeneralSecurityException", NULL, 0, false);
    declare_class(vm, "java/security/InvalidKeyException",
                  "java/security/GeneralSecurityException", NULL, 0, false);
    declare_class(vm, "java/security/InvalidAlgorithmParameterException",
                  "java/security/GeneralSecurityException", NULL, 0, false);
    declare_class(vm, "javax/crypto/BadPaddingException",
                  "java/security/GeneralSecurityException", NULL, 0, false);
    declare_class(vm, "javax/crypto/IllegalBlockSizeException",
                  "java/security/GeneralSecurityException", NULL, 0, false);
    declare_class(vm, "java/security/InvalidParameterException",
                  "java/lang/IllegalArgumentException", NULL, 0, false);
}

void jce_runtime_vm_end(struct dvm *vm)
{
    if (vm && vm == active_vm) active_vm = NULL;
}

bool jce_runtime_call(struct dvm *vm, const char *class_name,
                      const char *method, const char *sig, dvm_ref self,
                      const union dvm_value *args, int nargs,
                      union dvm_value *out)
{
    if (!vm || vm != active_vm || !class_name || !out) return false;
    if (!strcmp(class_name, "javax/crypto/Cipher"))
        return cipher_call(vm, method, sig, self, args, nargs, out);
    if (!strcmp(class_name, "javax/crypto/spec/SecretKeySpec"))
        return secret_key_call(vm, method, sig, self, args, nargs, out);
    if (!strcmp(class_name, "javax/crypto/spec/IvParameterSpec"))
        return iv_call(vm, method, sig, self, args, nargs, out);
    return false;
}

/* ------------------------------------------------------------------ */
/* Selftest.  Vectors were produced by the host Java oracle
 * (DevEco JBR, AES/CBC/PKCS5Padding and AES/ECB/PKCS5Padding); they live only
 * in this test, never in the production paths above. */
static const char *test_exception_name(struct dvm *vm)
{
    dvm_ref exception = dvm_exception(vm);
    struct dvm_object *object =
        exception ? dvm__obj(vm, exception) : NULL;
    return object && object->cls ? object->cls->name : "0";
}

static dvm_ref test_bytes(struct dvm *vm, const unsigned char *data,
                          size_t length)
{
    return new_byte_array(vm, data, (uint32_t)length);
}

static dvm_ref test_make_key(struct dvm *vm, const unsigned char *key,
                             size_t length, const char *algorithm)
{
    struct dvm_class *klass =
        dvm_find_class_or_external(vm, "javax/crypto/spec/SecretKeySpec");
    dvm_ref object = klass ? dvm_new_object(vm, klass) : 0;
    dvm_ref key_array = test_bytes(vm, key, length);
    dvm_ref algorithm_ref = dvm_new_string(vm, algorithm);
    union dvm_value args[2];
    union dvm_value result = {0};
    if (!object || !key_array || !algorithm_ref) {
        sdk_trace_fprintf(stderr,
                          "JCE_SELFTEST_HELPER_FAIL helper=make_key algorithm=%s allocate=%d exception=%s\n",
                          algorithm, object && key_array && algorithm_ref,
                          test_exception_name(vm));
        return 0;
    }
    args[0].l = key_array;
    args[1].l = algorithm_ref;
    if (!jce_runtime_call(vm, "javax/crypto/spec/SecretKeySpec", "<init>",
                          "([BLjava/lang/String;)V", object, args, 2,
                          &result)) {
        sdk_trace_fprintf(stderr,
                          "JCE_SELFTEST_HELPER_FAIL helper=make_key algorithm=%s call=0 exception=%s\n",
                          algorithm, test_exception_name(vm));
        return 0;
    }
    if (dvm_exception(vm)) {
        sdk_trace_fprintf(stderr,
                          "JCE_SELFTEST_HELPER_FAIL helper=make_key algorithm=%s call=1 exception=%s\n",
                          algorithm, test_exception_name(vm));
        dvm_clear_exception(vm);
        return 0;
    }
    return object;
}

static dvm_ref test_make_iv(struct dvm *vm, const unsigned char *iv,
                            size_t length)
{
    struct dvm_class *klass =
        dvm_find_class_or_external(vm, "javax/crypto/spec/IvParameterSpec");
    dvm_ref object = klass ? dvm_new_object(vm, klass) : 0;
    dvm_ref iv_array = test_bytes(vm, iv, length);
    union dvm_value arg;
    union dvm_value result = {0};
    if (!object || !iv_array) {
        sdk_trace_fprintf(stderr,
                          "JCE_SELFTEST_HELPER_FAIL helper=make_iv length=%u allocate=%d exception=%s\n",
                          (unsigned)length, object && iv_array,
                          test_exception_name(vm));
        return 0;
    }
    arg.l = iv_array;
    if (!jce_runtime_call(vm, "javax/crypto/spec/IvParameterSpec", "<init>",
                          "([B)V", object, &arg, 1, &result)) {
        sdk_trace_fprintf(stderr,
                          "JCE_SELFTEST_HELPER_FAIL helper=make_iv length=%u call=0 exception=%s\n",
                          (unsigned)length, test_exception_name(vm));
        return 0;
    }
    if (dvm_exception(vm)) {
        sdk_trace_fprintf(stderr,
                          "JCE_SELFTEST_HELPER_FAIL helper=make_iv length=%u call=1 exception=%s\n",
                          (unsigned)length, test_exception_name(vm));
        dvm_clear_exception(vm);
        return 0;
    }
    return object;
}

static dvm_ref test_get_cipher(struct dvm *vm, const char *transformation)
{
    dvm_ref text = dvm_new_string(vm, transformation);
    union dvm_value arg;
    union dvm_value result = {0};
    if (!text) {
        sdk_trace_fprintf(stderr,
                          "JCE_SELFTEST_HELPER_FAIL helper=get_cipher transformation=%s allocate=0 exception=%s\n",
                          transformation, test_exception_name(vm));
        return 0;
    }
    arg.l = text;
    if (!jce_runtime_call(vm, "javax/crypto/Cipher", "getInstance",
                          "(Ljava/lang/String;)Ljavax/crypto/Cipher;", 0,
                          &arg, 1, &result)) {
        sdk_trace_fprintf(stderr,
                          "JCE_SELFTEST_HELPER_FAIL helper=get_cipher transformation=%s call=0 exception=%s\n",
                          transformation, test_exception_name(vm));
        return 0;
    }
    if (dvm_exception(vm)) {
        sdk_trace_fprintf(stderr,
                          "JCE_SELFTEST_HELPER_FAIL helper=get_cipher transformation=%s call=1 exception=%s\n",
                          transformation, test_exception_name(vm));
        dvm_clear_exception(vm);
        return 0;
    }
    return result.l;
}

static bool test_expect_exception(struct dvm *vm, const char *type)
{
    dvm_ref exception = dvm_exception(vm);
    struct dvm_object *object =
        exception ? dvm__obj(vm, exception) : NULL;
    bool matches = object && object->cls && !strcmp(object->cls->name, type);
    if (exception) dvm_clear_exception(vm);
    return matches;
}

static bool test_get_instance_error(struct dvm *vm, const char *transformation,
                                    const char *expected)
{
    dvm_ref text = transformation ? dvm_new_string(vm, transformation) : 0;
    union dvm_value arg;
    union dvm_value result = {0};
    arg.l = text;
    if (!jce_runtime_call(vm, "javax/crypto/Cipher", "getInstance",
                          "(Ljava/lang/String;)Ljavax/crypto/Cipher;", 0,
                          &arg, 1, &result)) {
        sdk_trace_fprintf(stderr,
                          "JCE_SELFTEST_HELPER_FAIL helper=get_instance_error transformation=%s expected=%s call=0 exception=%s\n",
                          transformation ? transformation : "(null)", expected,
                          test_exception_name(vm));
        return false;
    }
    {
        dvm_ref exception = dvm_exception(vm);
        struct dvm_object *object =
            exception ? dvm__obj(vm, exception) : NULL;
        bool matches =
            object && object->cls && !strcmp(object->cls->name, expected);
        if (!matches)
            sdk_trace_fprintf(stderr,
                              "JCE_SELFTEST_HELPER_FAIL helper=get_instance_error transformation=%s expected=%s actual=%s\n",
                              transformation ? transformation : "(null)",
                              expected, test_exception_name(vm));
        if (exception) dvm_clear_exception(vm);
        return matches;
    }
}

static bool test_cipher_init(struct dvm *vm, dvm_ref cipher, int mode,
                             dvm_ref key, dvm_ref iv, bool with_iv)
{
    union dvm_value args[3];
    union dvm_value result = {0};
    args[0].i = mode;
    args[1].l = key;
    args[2].l = iv;
    if (with_iv)
        return jce_runtime_call(
            vm, "javax/crypto/Cipher", "init",
            "(ILjava/security/Key;Ljava/security/spec/AlgorithmParameterSpec;)V",
            cipher, args, 3, &result);
    return jce_runtime_call(vm, "javax/crypto/Cipher", "init",
                            "(ILjava/security/Key;)V", cipher, args, 2,
                            &result);
}

static bool test_cipher_do_final(struct dvm *vm, dvm_ref cipher,
                                 dvm_ref input, dvm_ref *result)
{
    union dvm_value arg;
    union dvm_value output = {0};
    arg.l = input;
    if (!jce_runtime_call(vm, "javax/crypto/Cipher", "doFinal", "([B)[B",
                          cipher, &arg, 1, &output))
        return false;
    *result = output.l;
    return true;
}

static bool test_array_equals(struct dvm *vm, dvm_ref array,
                              const unsigned char *expected, size_t length)
{
    if (!array || dvm_exception(vm)) return false;
    if ((size_t)dvm_array_length(vm, array) != length) return false;
    if (!length) return true;
    const unsigned char *data = dvm_array_data(vm, array);
    return data && !memcmp(data, expected, length);
}

static void test_print_hex(const unsigned char *data, size_t length)
{
    size_t shown = length < 32u ? length : 32u;
    if (!data) {
        sdk_trace_fprintf(stderr, "<null>");
        return;
    }
    for (size_t i = 0; i < shown; ++i)
        sdk_trace_fprintf(stderr, "%02x", data[i]);
    if (length > shown) sdk_trace_fprintf(stderr, "...");
}

/* Byte-vector comparison with a diagnostic that names the vector and prints
 * the actual length plus the first 32 bytes, so an AES/key mismatch is
 * distinguishable from a length or comparison failure on device. */
static bool test_expect_bytes(struct dvm *vm, const char *label,
                              dvm_ref array, const unsigned char *expected,
                              size_t length)
{
    if (!array || dvm_exception(vm)) {
        sdk_trace_fprintf(stderr,
                          "JCE_SELFTEST_VECTOR label=%s actual=missing exception=%s\n",
                          label, test_exception_name(vm));
        return false;
    }
    uint32_t actual_length = dvm_array_length(vm, array);
    const unsigned char *actual =
        actual_length ? (const unsigned char *)dvm_array_data(vm, array)
                      : NULL;
    bool matches = (size_t)actual_length == length &&
                   (!length || (actual && !memcmp(actual, expected, length)));
    if (!matches) {
        sdk_trace_fprintf(
            stderr,
            "JCE_SELFTEST_VECTOR label=%s actual_length=%u expected_length=%u actual_hex=",
            label, (unsigned)actual_length, (unsigned)length);
        test_print_hex(actual, (size_t)actual_length);
        sdk_trace_fprintf(stderr, " expected_hex=");
        test_print_hex(expected, length);
        sdk_trace_fprintf(stderr, "\n");
    }
    return matches;
}

#define JCE_CHECK(expr) do { if (!(expr)) { sdk_trace_fprintf(stderr, "JCE_RUNTIME_SELFTEST_FAIL line=%d expression=%s\n", __LINE__, #expr); goto done; } } while (0)

bool jce_runtime_selftest(struct dvm *vm)
{
    static const unsigned char ecb_key[16] = {
        0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
        0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f
    };
    static const unsigned char ecb_plain[16] = {
        0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,
        0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff
    };
    static const unsigned char ecb_cipher[32] = {
        0x69,0xc4,0xe0,0xd8,0x6a,0x7b,0x04,0x30,
        0xd8,0xcd,0xb7,0x80,0x70,0xb4,0xc5,0x5a,
        0x95,0x4f,0x64,0xf2,0xe4,0xe8,0x6e,0x9e,
        0xee,0x82,0xd2,0x02,0x16,0x68,0x48,0x99
    };
    static const unsigned char ecb_empty_cipher[16] = {
        0x95,0x4f,0x64,0xf2,0xe4,0xe8,0x6e,0x9e,
        0xee,0x82,0xd2,0x02,0x16,0x68,0x48,0x99
    };
    static const unsigned char cbc_key[16] = {
        0x2b,0x7e,0x15,0x16,0x28,0xae,0xd2,0xa6,
        0xab,0xf7,0x15,0x88,0x09,0xcf,0x4f,0x3c
    };
    static const unsigned char cbc_iv[16] = {
        0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
        0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f
    };
    static const unsigned char cbc_plain[16] = {
        0x6b,0xc1,0xbe,0xe2,0x2e,0x40,0x9f,0x96,
        0xe9,0x3d,0x7e,0x11,0x73,0x93,0x17,0x2a
    };
    static const unsigned char cbc_cipher[32] = {
        0x76,0x49,0xab,0xac,0x81,0x19,0xb2,0x46,
        0xce,0xe9,0x8e,0x9b,0x12,0xe9,0x19,0x7d,
        0x89,0x64,0xe0,0xb1,0x49,0xc1,0x0b,0x7b,
        0x68,0x2e,0x6e,0x39,0xaa,0xeb,0x73,0x1c
    };
    static const unsigned char cbc_empty_cipher[16] = {
        0xc8,0x4a,0xf0,0xb6,0x13,0x43,0x5d,0x5d,
        0x91,0x82,0x80,0x1a,0x9b,0xd9,0x32,0x0b
    };
    /* Java oracle: AES-ECB(cbc_key, 15*0x41 0x0f); AES-CBC-decrypting this
     * block under cbc_iv yields a final byte of 0x00, which is an illegal
     * PKCS#5 padding length regardless of the preceding bytes. */
    static const unsigned char bad_padding_block[16] = {
        0x46,0x3e,0xa0,0xf5,0xec,0xcb,0x59,0x04,
        0x6a,0xe6,0x89,0x54,0xb3,0xd3,0x15,0xa0
    };
    static const unsigned char bad_length_block[15] = {
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
    };
    static const unsigned char short_key[8] = {0,1,2,3,4,5,6,7};

    bool okay = false;
    dvm_ref ecb_key_object = 0, cbc_key_object = 0, short_key_object = 0;
    dvm_ref iv_object = 0, short_iv_object = 0;
    dvm_ref cbc = 0, ecb = 0, pkcs7 = 0;
    dvm_ref plain_ref = 0, cipher_ref = 0, empty_ref = 0;
    dvm_ref ecb_plain_ref = 0;
    dvm_ref bad_padding_ref = 0, bad_length_ref = 0;
    dvm_ref result = 0, encoded = 0, encoded_copy = 0, iv_copy = 0;
    union dvm_value call_out = {0};
    union dvm_value args[3];
    unsigned char short_iv[8] = {0};

    if (!vm || vm != active_vm) {
        sdk_trace_fprintf(stderr,
                          "JCE_RUNTIME_SELFTEST_FAIL stage=entry vm_set=%d active_vm_set=%d same=%d\n",
                          vm ? 1 : 0, active_vm ? 1 : 0, vm == active_vm);
        return false;
    }
    {
        dvm_ref stale = dvm_exception(vm);
        struct dvm_object *stale_object =
            stale ? dvm__obj(vm, stale) : NULL;
        sdk_trace_fprintf(stderr,
                          "JCE_RUNTIME_SELFTEST PRESTAGE exception=%s\n",
                          stale_object && stale_object->cls
                              ? stale_object->cls->name : "0");
        if (stale) dvm_clear_exception(vm);
    }

    cbc = test_get_cipher(vm, "AES/CBC/PKCS5Padding");
    JCE_CHECK(!(!cbc));
    ecb = test_get_cipher(vm, "AES/ECB/PKCS5Padding");
    JCE_CHECK(!(!ecb));
    pkcs7 = test_get_cipher(vm, "AES/ECB/PKCS7Padding");
    JCE_CHECK(!(!pkcs7));

    JCE_CHECK(!(!jce_runtime_call(vm, "javax/crypto/Cipher", "getBlockSize", "()I",
                          cbc, NULL, 0, &call_out) ||
        call_out.i != 16));
    JCE_CHECK(!(!jce_runtime_call(vm, "javax/crypto/Cipher", "getAlgorithm",
                          "()Ljava/lang/String;", cbc, NULL, 0, &call_out) ||
        !call_out.l ||
        strcmp(dvm_string_utf8(vm, call_out.l), "AES/CBC/PKCS5Padding")));
    /* getOutputSize before init is an error, as in Java. */
    args[0].i = 10;
    JCE_CHECK(!(!jce_runtime_call(vm, "javax/crypto/Cipher", "getOutputSize", "(I)I",
                          cbc, args, 1, &call_out) ||
        !test_expect_exception(vm, "java/lang/IllegalStateException")));

    JCE_CHECK(!(test_get_instance_error(vm, "AES/XYZ/PKCS5Padding",
                                "java/security/NoSuchAlgorithmException") == false));
    JCE_CHECK(!(!test_get_instance_error(vm, "AES/CBC/XYZ",
                                 "java/security/NoSuchPaddingException")));
    JCE_CHECK(!(!test_get_instance_error(vm, NULL,
                                 "java/security/NoSuchAlgorithmException")));

    ecb_key_object = test_make_key(vm, ecb_key, sizeof(ecb_key), "AES");
    JCE_CHECK(!(!ecb_key_object));
    cbc_key_object = test_make_key(vm, cbc_key, sizeof(cbc_key), "AES");
    JCE_CHECK(!(!cbc_key_object));
    short_key_object = test_make_key(vm, short_key, sizeof(short_key), "AES");
    JCE_CHECK(!(!short_key_object));
    iv_object = test_make_iv(vm, cbc_iv, sizeof(cbc_iv));
    JCE_CHECK(!(!iv_object));
    short_iv_object = test_make_iv(vm, short_iv, sizeof(short_iv));
    JCE_CHECK(!(!short_iv_object));

    /* SecretKeySpec copies its payload and reports Java-compatible values. */
    JCE_CHECK(!(!jce_runtime_call(vm, "javax/crypto/spec/SecretKeySpec",
                          "getEncoded", "()[B", ecb_key_object, NULL, 0,
                          &call_out) ||
        !test_array_equals(vm, call_out.l, ecb_key, sizeof(ecb_key))));
    encoded = call_out.l;
    JCE_CHECK(!(!jce_runtime_call(vm, "javax/crypto/spec/SecretKeySpec",
                          "getEncoded", "()[B", ecb_key_object, NULL, 0,
                          &call_out) ||
        !test_array_equals(vm, call_out.l, ecb_key, sizeof(ecb_key))));
    encoded_copy = call_out.l;
    JCE_CHECK(!(encoded == encoded_copy));
    ((unsigned char *)dvm_array_data(vm, encoded))[0] ^= 0xff;
    JCE_CHECK(!(!jce_runtime_call(vm, "javax/crypto/spec/SecretKeySpec",
                          "getEncoded", "()[B", ecb_key_object, NULL, 0,
                          &call_out) ||
        !test_array_equals(vm, call_out.l, ecb_key, sizeof(ecb_key))));
    JCE_CHECK(!(!jce_runtime_call(vm, "javax/crypto/spec/SecretKeySpec",
                          "getAlgorithm", "()Ljava/lang/String;",
                          ecb_key_object, NULL, 0, &call_out) ||
        !call_out.l || strcmp(dvm_string_utf8(vm, call_out.l), "AES")));
    JCE_CHECK(!(!jce_runtime_call(vm, "javax/crypto/spec/SecretKeySpec", "getFormat",
                          "()Ljava/lang/String;", ecb_key_object, NULL, 0,
                          &call_out) ||
        !call_out.l || strcmp(dvm_string_utf8(vm, call_out.l), "RAW")));

    /* IvParameterSpec.getIV returns a fresh copy as well. */
    JCE_CHECK(!(!jce_runtime_call(vm, "javax/crypto/spec/IvParameterSpec", "getIV",
                          "()[B", iv_object, NULL, 0, &call_out) ||
        !test_array_equals(vm, call_out.l, cbc_iv, sizeof(cbc_iv))));
    iv_copy = call_out.l;
    ((unsigned char *)dvm_array_data(vm, iv_copy))[0] ^= 0xff;
    JCE_CHECK(!(!jce_runtime_call(vm, "javax/crypto/spec/IvParameterSpec", "getIV",
                          "()[B", iv_object, NULL, 0, &call_out) ||
        !test_array_equals(vm, call_out.l, cbc_iv, sizeof(cbc_iv))));

    /* Negative key and parameter paths. */
    JCE_CHECK(!(!test_cipher_init(vm, cbc, 1, short_key_object, iv_object, true) ||
        !test_expect_exception(vm, "java/security/InvalidKeyException")));
    JCE_CHECK(!(!test_cipher_init(vm, cbc, 1, cbc_key_object, short_iv_object, true) ||
        !test_expect_exception(
            vm, "java/security/InvalidAlgorithmParameterException")));
    JCE_CHECK(!(!test_cipher_init(vm, cbc, 1, cbc_key_object, 0, false) ||
        !test_expect_exception(
            vm, "java/security/InvalidAlgorithmParameterException")));
    JCE_CHECK(!(!test_cipher_init(vm, cbc, 0, cbc_key_object, iv_object, true) ||
        !test_expect_exception(vm, "java/lang/IllegalArgumentException")));

    /* SecretKeySpec constructor validation. */
    {
        struct dvm_class *klass = dvm_find_class_or_external(
            vm, "javax/crypto/spec/SecretKeySpec");
        dvm_ref object = klass ? dvm_new_object(vm, klass) : 0;
        dvm_ref empty_array = test_bytes(vm, NULL, 0);
        dvm_ref key_array = test_bytes(vm, ecb_key, sizeof(ecb_key));
        dvm_ref algorithm = dvm_new_string(vm, "AES");
        union dvm_value key_args[2];
        JCE_CHECK(!(!object || !empty_array || !key_array || !algorithm));
        key_args[0].l = 0;
        key_args[1].l = algorithm;
        JCE_CHECK(!(!jce_runtime_call(vm, "javax/crypto/spec/SecretKeySpec",
                              "<init>", "([BLjava/lang/String;)V", object,
                              key_args, 2, &call_out) ||
            !test_expect_exception(vm, "java/lang/IllegalArgumentException")));
        object = dvm_new_object(vm, klass);
        key_args[0].l = empty_array;
        JCE_CHECK(!(!object ||
            !jce_runtime_call(vm, "javax/crypto/spec/SecretKeySpec",
                              "<init>", "([BLjava/lang/String;)V", object,
                              key_args, 2, &call_out) ||
            !test_expect_exception(vm, "java/lang/IllegalArgumentException")));
        object = dvm_new_object(vm, klass);
        key_args[0].l = key_array;
        key_args[1].l = 0;
        JCE_CHECK(!(!object ||
            !jce_runtime_call(vm, "javax/crypto/spec/SecretKeySpec",
                              "<init>", "([BLjava/lang/String;)V", object,
                              key_args, 2, &call_out) ||
            !test_expect_exception(vm, "java/lang/IllegalArgumentException")));
    }

    /* IvParameterSpec rejects null and preserves an empty array until init. */
    {
        struct dvm_class *klass = dvm_find_class_or_external(
            vm, "javax/crypto/spec/IvParameterSpec");
        dvm_ref object = klass ? dvm_new_object(vm, klass) : 0;
        dvm_ref empty_array = test_bytes(vm, NULL, 0);
        union dvm_value iv_args[1];
        JCE_CHECK(!(!object || !empty_array));
        iv_args[0].l = 0;
        JCE_CHECK(!(!jce_runtime_call(vm, "javax/crypto/spec/IvParameterSpec",
                              "<init>", "([B)V", object, iv_args, 1,
                              &call_out) ||
            !test_expect_exception(vm, "java/lang/NullPointerException")));
        object = dvm_new_object(vm, klass);
        iv_args[0].l = empty_array;
        JCE_CHECK(!(!object ||
            !jce_runtime_call(vm, "javax/crypto/spec/IvParameterSpec",
                              "<init>", "([B)V", object, iv_args, 1,
                              &call_out) ||
            dvm_exception(vm)));
        JCE_CHECK(!(!jce_runtime_call(vm, "javax/crypto/spec/IvParameterSpec",
                              "getIV", "()[B", object, NULL, 0, &call_out) ||
            !test_array_equals(vm, call_out.l, NULL, 0)));
    }

    /* CBC NIST vector, roundtrip, empty input and padding errors. */
    plain_ref = test_bytes(vm, cbc_plain, sizeof(cbc_plain));
    empty_ref = test_bytes(vm, NULL, 0);
    bad_padding_ref = test_bytes(vm, bad_padding_block,
                                 sizeof(bad_padding_block));
    bad_length_ref = test_bytes(vm, bad_length_block,
                                sizeof(bad_length_block));
    JCE_CHECK(!(!plain_ref || !empty_ref || !bad_padding_ref || !bad_length_ref));
    JCE_CHECK(!(!test_cipher_init(vm, cbc, 1, cbc_key_object, iv_object, true) ||
        dvm_exception(vm)));
    JCE_CHECK(!(!test_cipher_do_final(vm, cbc, plain_ref, &result) ||
        dvm_exception(vm) ||
        !test_expect_bytes(vm, "cbc_encrypt_nist", result, cbc_cipher,
                           sizeof(cbc_cipher))));
    cipher_ref = result;
    JCE_CHECK(!(!test_cipher_init(vm, cbc, 2, cbc_key_object, iv_object, true) ||
        dvm_exception(vm)));
    JCE_CHECK(!(!test_cipher_do_final(vm, cbc, cipher_ref, &result) ||
        dvm_exception(vm) ||
        !test_expect_bytes(vm, "cbc_decrypt_nist", result, cbc_plain,
                           sizeof(cbc_plain))));
    JCE_CHECK(!(!test_cipher_init(vm, cbc, 1, cbc_key_object, iv_object, true) ||
        dvm_exception(vm)));
    JCE_CHECK(!(!test_cipher_do_final(vm, cbc, empty_ref, &result) ||
        dvm_exception(vm) ||
        !test_expect_bytes(vm, "cbc_encrypt_empty", result,
                           cbc_empty_cipher, sizeof(cbc_empty_cipher))));
    JCE_CHECK(!(!test_cipher_init(vm, cbc, 2, cbc_key_object, iv_object, true) ||
        dvm_exception(vm)));
    JCE_CHECK(!(!test_cipher_do_final(vm, cbc, empty_ref, &result) ||
        dvm_exception(vm) || dvm_array_length(vm, result) != 0));
    JCE_CHECK(!(!test_cipher_init(vm, cbc, 2, cbc_key_object, iv_object, true) ||
        dvm_exception(vm)));
    JCE_CHECK(!(!test_cipher_do_final(vm, cbc, bad_padding_ref, &result) ||
        !test_expect_exception(vm, "javax/crypto/BadPaddingException")));
    JCE_CHECK(!(!test_cipher_init(vm, cbc, 2, cbc_key_object, iv_object, true) ||
        dvm_exception(vm)));
    JCE_CHECK(!(!test_cipher_do_final(vm, cbc, bad_length_ref, &result) ||
        !test_expect_exception(vm, "javax/crypto/IllegalBlockSizeException")));
    JCE_CHECK(!(!test_cipher_init(vm, cbc, 1, cbc_key_object, iv_object, true) ||
        dvm_exception(vm)));
    args[0].i = 10;
    JCE_CHECK(!(!jce_runtime_call(vm, "javax/crypto/Cipher", "getOutputSize", "(I)I",
                          cbc, args, 1, &call_out) ||
        dvm_exception(vm) || call_out.i != 16));

    /* Null input is an error, and doFinal([B)[B null must not crash. */
    JCE_CHECK(!(!test_cipher_do_final(vm, cbc, 0, &result) ||
        !test_expect_exception(vm, "java/lang/NullPointerException")));
    /* update remains unimplemented and must fail closed, not fabricate. */
    args[0].l = empty_ref;
    JCE_CHECK(!(jce_runtime_call(vm, "javax/crypto/Cipher", "update", "([B)[B", cbc,
                         args, 1, &call_out) ||
        dvm_exception(vm)));

    /* ECB NIST vector and empty input, plus the ignored-IV rule.  ecb_cipher
     * is the Java oracle for ecb_key/ecb_plain, not for cbc_plain. */
    ecb_plain_ref = test_bytes(vm, ecb_plain, sizeof(ecb_plain));
    JCE_CHECK(!(!ecb_plain_ref));
    JCE_CHECK(!(!test_cipher_init(vm, ecb, 1, ecb_key_object, 0, false) ||
        dvm_exception(vm)));
    JCE_CHECK(!(!test_cipher_do_final(vm, ecb, ecb_plain_ref, &result) ||
        dvm_exception(vm) ||
        !test_expect_bytes(vm, "ecb_encrypt_nist", result, ecb_cipher,
                           sizeof(ecb_cipher))));
    cipher_ref = result;
    JCE_CHECK(!(!test_cipher_init(vm, ecb, 2, ecb_key_object, 0, false) ||
        dvm_exception(vm)));
    JCE_CHECK(!(!test_cipher_do_final(vm, ecb, cipher_ref, &result) ||
        dvm_exception(vm) ||
        !test_expect_bytes(vm, "ecb_decrypt_nist", result, ecb_plain,
                           sizeof(ecb_plain))));
    JCE_CHECK(!(!test_cipher_init(vm, ecb, 1, ecb_key_object, 0, false) ||
        dvm_exception(vm)));
    JCE_CHECK(!(!test_cipher_do_final(vm, ecb, empty_ref, &result) ||
        dvm_exception(vm) ||
        !test_expect_bytes(vm, "ecb_encrypt_empty", result,
                           ecb_empty_cipher, sizeof(ecb_empty_cipher))));
    JCE_CHECK(!(!test_cipher_init(vm, ecb, 1, ecb_key_object, iv_object, true) ||
        dvm_exception(vm)));
    JCE_CHECK(!(!test_cipher_do_final(vm, ecb, empty_ref, &result) ||
        dvm_exception(vm) ||
        !test_expect_bytes(vm, "ecb_encrypt_empty_with_iv", result,
                           ecb_empty_cipher, sizeof(ecb_empty_cipher))));
    JCE_CHECK(!(!jce_runtime_call(vm, "javax/crypto/Cipher", "getBlockSize", "()I",
                          pkcs7, NULL, 0, &call_out) ||
        call_out.i != 16));

    okay = true;
done:
    if (okay) {
        sdk_trace_fprintf(
            stderr,
            "JCE_RUNTIME_SELFTEST PASS vectors=java_oracle cbc=1 ecb=1 pkcs5=1 pkcs7_alias=1 padding_rejected=1 empty=1 copies=1 negative=1\n");
    } else {
        sdk_trace_fprintf(stderr, "JCE_RUNTIME_SELFTEST FAIL\n");
        if (dvm_exception(vm)) dvm_clear_exception(vm);
    }
    return okay;
}
