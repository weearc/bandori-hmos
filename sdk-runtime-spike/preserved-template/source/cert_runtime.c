#include "cert_runtime.h"
#include "exception_hierarchy.h"
#include "cert_objects.h"
#include "fixture_der.h"
#include "expected_spki.h"

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mbedtls/md5.h"
#include "mbedtls/sha1.h"
#include "mbedtls/sha256.h"

#define CERT_MAX_INPUT (16u * 1024u * 1024u)
#define CERT_READ_CHUNK 4096u
#define CERT_MAX_ENTRIES 4096u

struct cert_payload {
    cert_object *object;
};

struct public_key_payload {
    unsigned char *spki;
    size_t length;
    char *algorithm;
};

struct cert_entry {
    char *alias;
    cert_object *certificate;
};

struct keystore_payload {
    char *type;
    bool loaded;
    bool system_store;
    struct cert_entry *entries;
    size_t count;
    size_t capacity;
};

struct factory_payload {
    char *type;
};

struct digest_payload {
    char *algorithm;
    unsigned char *data;
    size_t length;
    size_t capacity;
};

static struct dvm *active_vm;

static bool is_call(const char *method, const char *sig,
                    const char *wanted_method, const char *wanted_sig)
{
    return method && sig && !strcmp(method, wanted_method) && !strcmp(sig, wanted_sig);
}

static bool fail_message(struct dvm *vm, const char *type, const char *message)
{
    dvm__throw(vm, type, "%s", message ? message : "certificate runtime failure");
    return true;
}

static bool failf(struct dvm *vm, const char *type, const char *format,
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

static bool string_argument(struct dvm *vm, dvm_ref ref, const char **out,
                            const char *what)
{
    const char *text;
    uint32_t length;
    if (!ref) return fail_message(vm, "java/lang/NullPointerException", what);
    text = dvm_string_utf8(vm, ref);
    length = dvm_string_utf8_length(vm, ref);
    if (!text || strlen(text) != length)
        return fail_message(vm, "java/lang/IllegalArgumentException", what);
    *out = text;
    return true;
}

static char *duplicate_string(const char *text)
{
    size_t length;
    char *copy;
    if (!text) return NULL;
    length = strlen(text);
    copy = malloc(length + 1u);
    if (!copy) return NULL;
    memcpy(copy, text, length + 1u);
    return copy;
}

static bool byte_array(struct dvm *vm, dvm_ref ref, const unsigned char **data,
                       uint32_t *length, const char *what)
{
    struct dvm_object *object = dvm__obj(vm, ref);
    if (!object) return fail_message(vm, "java/lang/NullPointerException", what);
    if (object->kind != DVM_OBJ_ARRAY || object->elem_kind != 'B')
        return fail_message(vm, "java/lang/IllegalArgumentException", what);
    *data = object->data;
    *length = object->length;
    return true;
}

static bool byte_array_range(struct dvm *vm, dvm_ref ref, int offset, int length,
                             const unsigned char **data)
{
    struct dvm_object *object = dvm__obj(vm, ref);
    if (!object) return fail_message(vm, "java/lang/NullPointerException", "byte array");
    if (object->kind != DVM_OBJ_ARRAY || object->elem_kind != 'B')
        return fail_message(vm, "java/lang/IllegalArgumentException", "not a byte array");
    if (offset < 0 || length < 0 || (uint32_t)offset > object->length ||
        (uint32_t)length > object->length - (uint32_t)offset)
        return fail_message(vm, "java/lang/IndexOutOfBoundsException", "byte array range");
    *data = (const unsigned char *)object->data + offset;
    return true;
}

static void cert_payload_drop(void *data)
{
    struct cert_payload *payload_value = data;
    if (!payload_value) return;
    cert_object_free(payload_value->object);
    free(payload_value);
}

static void public_key_payload_drop(void *data)
{
    struct public_key_payload *key = data;
    if (!key) return;
    free(key->spki);
    free(key->algorithm);
    free(key);
}

static void keystore_payload_drop(void *data)
{
    struct keystore_payload *store = data;
    if (!store) return;
    for (size_t i = 0; i < store->count; ++i) {
        free(store->entries[i].alias);
        cert_object_free(store->entries[i].certificate);
    }
    free(store->entries);
    free(store->type);
    free(store);
}

static void factory_payload_drop(void *data)
{
    struct factory_payload *factory = data;
    if (!factory) return;
    free(factory->type);
    free(factory);
}

static void digest_payload_drop(void *data)
{
    struct digest_payload *digest = data;
    if (!digest) return;
    free(digest->algorithm);
    free(digest->data);
    free(digest);
}

static dvm_ref new_public_key(struct dvm *vm, const cert_object *certificate)
{
    struct public_key_payload *state;
    const unsigned char *spki;
    const char *algorithm;
    size_t length;
    dvm_ref object;
    spki = cert_object_spki(certificate, &length);
    algorithm = cert_object_public_key_algorithm(certificate);
    if (!spki || !length || !algorithm || length > CERT_MAX_INPUT) {
        fail_message(vm, "java/security/KeyException", "unsupported public key");
        return 0;
    }
    state = calloc(1, sizeof(*state));
    if (!state) {
        fail_message(vm, "java/lang/OutOfMemoryError", "public key");
        return 0;
    }
    state->spki = malloc(length);
    state->algorithm = duplicate_string(algorithm);
    if (!state->spki || !state->algorithm) {
        public_key_payload_drop(state);
        fail_message(vm, "java/lang/OutOfMemoryError", "public key");
        return 0;
    }
    memcpy(state->spki, spki, length);
    state->length = length;
    object = dvm_new_object(vm, dvm_find_class_or_external(vm, "java/security/PublicKey"));
    if (!object || !attach_payload(vm, object, state, public_key_payload_drop)) {
        public_key_payload_drop(state);
        fail_message(vm, "java/lang/OutOfMemoryError", "public key object");
        return 0;
    }
    return object;
}

static dvm_ref new_certificate(struct dvm *vm, cert_object *parsed)
{
    struct cert_payload *state;
    dvm_ref object;
    state = calloc(1, sizeof(*state));
    if (!state) {
        cert_object_free(parsed);
        fail_message(vm, "java/lang/OutOfMemoryError", "certificate object");
        return 0;
    }
    state->object = parsed;
    object = dvm_new_object(vm, dvm_find_class_or_external(
        vm, "java/security/cert/X509Certificate"));
    if (!object || !attach_payload(vm, object, state, cert_payload_drop)) {
        cert_payload_drop(state);
        fail_message(vm, "java/lang/OutOfMemoryError", "certificate object");
        return 0;
    }
    return object;
}

dvm_ref cert_runtime_new_x509(struct dvm *vm, const unsigned char *der,
                              size_t length)
{
    cert_object *parsed;
    if (!der || !length || length > CERT_MAX_INPUT) {
        fail_message(vm, "java/security/cert/CertificateException", "invalid DER certificate");
        return 0;
    }
    parsed = cert_object_parse(der, length);
    if (!parsed) {
        fail_message(vm, "java/security/cert/CertificateException", "invalid DER certificate");
        return 0;
    }
    return new_certificate(vm, parsed);
}

dvm_ref cert_runtime_new_x509_array(struct dvm *vm,
                                    const unsigned char *const *der,
                                    const size_t *length, size_t count)
{
    dvm_ref array;
    dvm_ref *items;
    if (count > CERT_MAX_ENTRIES || (count && (!der || !length))) {
        fail_message(vm, "java/security/cert/CertificateException", "invalid certificate chain");
        return 0;
    }
    array = dvm_new_array(vm, 'L', "Ljava/security/cert/X509Certificate;",
                          (uint32_t)count);
    if (!array) {
        fail_message(vm, "java/lang/OutOfMemoryError", "certificate chain array");
        return 0;
    }
    items = dvm_array_data(vm, array);
    for (size_t i = 0; i < count; ++i) {
        items[i] = cert_runtime_new_x509(vm, der[i], length[i]);
        if (!items[i]) return 0;
    }
    return array;
}

const unsigned char *cert_runtime_der(struct dvm *vm, dvm_ref certificate,
                                      size_t *length)
{
    struct cert_payload *state = payload(vm, certificate, cert_payload_drop);
    if (!state || !state->object) {
        if (length) *length = 0;
        return NULL;
    }
    return cert_object_der(state->object, length);
}

long cert_runtime_keystore_trusted(struct dvm *vm, dvm_ref keystore,
                                   const unsigned char **der, size_t *length,
                                   size_t max)
{
    struct keystore_payload *store = payload(vm, keystore, keystore_payload_drop);
    if (!store || !store->loaded || store->system_store) return -1;
    if (der && length) {
        size_t count = store->count < max ? store->count : max;
        for (size_t i = 0; i < count; ++i) {
            der[i] = cert_object_der(store->entries[i].certificate, &length[i]);
        }
    }
    return (long)store->count;
}

static bool read_stream(struct dvm *vm, dvm_ref stream,
                        unsigned char **out, size_t *out_length)
{
    struct dvm_class *klass;
    struct dvm_method *method;
    unsigned char *buffer = NULL;
    size_t length = 0;
    size_t capacity = 0;
    dvm_ref chunk;
    union dvm_value args[3] = {{0}}, result = {0};
    if (!stream) return fail_message(vm, "java/lang/NullPointerException", "certificate input stream");
    klass = dvm_object_class(vm, stream);
    method = klass ? dvm_find_method(vm, klass, "read", "([BII)I") : NULL;
    if (!method) {
        return fail_message(vm, "java/io/IOException", "InputStream.read is unavailable");
    }
    chunk = dvm_new_array(vm, 'B', "B", CERT_READ_CHUNK);
    if (!chunk) return fail_message(vm, "java/lang/OutOfMemoryError", "certificate input buffer");
    args[0].l = chunk;
    args[1].i = 0;
    args[2].i = CERT_READ_CHUNK;
    for (;;) {
        int got;
        result.l = 0;
        if (!dvm_call(vm, method, stream, args, 3, &result) || dvm_exception(vm)) {
            free(buffer);
            return true;
        }
        got = result.i;
        if (got < 0) break;
        if (got == 0) {
            free(buffer);
            return fail_message(vm, "java/io/IOException", "InputStream returned zero bytes");
        }
        if ((size_t)got > CERT_READ_CHUNK || length > CERT_MAX_INPUT - (size_t)got) {
            free(buffer);
            return fail_message(vm, "java/security/cert/CertificateException", "certificate input too large");
        }
        if (length + (size_t)got > capacity) {
            size_t next = capacity ? capacity * 2u : CERT_READ_CHUNK;
            unsigned char *grown;
            while (next < length + (size_t)got) {
                if (next > CERT_MAX_INPUT / 2u) { next = CERT_MAX_INPUT; break; }
                next *= 2u;
            }
            grown = realloc(buffer, next);
            if (!grown) {
                free(buffer);
                return fail_message(vm, "java/lang/OutOfMemoryError", "certificate input");
            }
            buffer = grown;
            capacity = next;
        }
        memcpy(buffer + length, dvm_array_data(vm, chunk), (size_t)got);
        length += (size_t)got;
    }
    dvm__discard_temporary(vm, chunk);
    *out = buffer;
    *out_length = length;
    return true;
}

static bool digest_name(const char *name, size_t *digest_length)
{
    if (!name) return false;
    if (!strcasecmp(name, "MD5")) { *digest_length = 16; return true; }
    if (!strcasecmp(name, "SHA-1") || !strcasecmp(name, "SHA1")) {
        *digest_length = 20; return true;
    }
    if (!strcasecmp(name, "SHA-256") || !strcasecmp(name, "SHA256")) {
        *digest_length = 32; return true;
    }
    return false;
}

static bool digest_append(struct dvm *vm, struct digest_payload *digest,
                          const unsigned char *data, size_t length)
{
    unsigned char *grown;
    size_t capacity;
    if (length > CERT_MAX_INPUT - digest->length)
        return fail_message(vm, "java/lang/OutOfMemoryError", "message digest input too large");
    if (digest->length + length <= digest->capacity) {
        memcpy(digest->data + digest->length, data, length);
        digest->length += length;
        return true;
    }
    capacity = digest->capacity ? digest->capacity * 2u : 256u;
    while (capacity < digest->length + length) {
        if (capacity > CERT_MAX_INPUT / 2u) { capacity = CERT_MAX_INPUT; break; }
        capacity *= 2u;
    }
    grown = realloc(digest->data, capacity);
    if (!grown) return fail_message(vm, "java/lang/OutOfMemoryError", "message digest input");
    digest->data = grown;
    digest->capacity = capacity;
    memcpy(digest->data + digest->length, data, length);
    digest->length += length;
    return true;
}

static bool digest_compute(struct dvm *vm, struct digest_payload *digest,
                           unsigned char *output, size_t output_length)
{
    size_t expected;
    if (!digest_name(digest->algorithm, &expected) || expected != output_length)
        return fail_message(vm, "java/security/NoSuchAlgorithmException", "unsupported digest");
    if (!strcasecmp(digest->algorithm, "MD5")) {
        if (mbedtls_md5(digest->data, digest->length, output) != 0)
            return fail_message(vm, "java/security/GeneralSecurityException", "MD5 failed");
    } else if (expected == 20) {
        if (mbedtls_sha1(digest->data, digest->length, output) != 0)
            return fail_message(vm, "java/security/GeneralSecurityException", "SHA-1 failed");
    } else {
        if (mbedtls_sha256(digest->data, digest->length, output, 0) != 0)
            return fail_message(vm, "java/security/GeneralSecurityException", "SHA-256 failed");
    }
    return true;
}

static bool keystore_set_certificate(struct dvm *vm,
                                     struct keystore_payload *store,
                                     dvm_ref alias_ref, dvm_ref certificate)
{
    const char *alias;
    size_t der_length;
    const unsigned char *der;
    cert_object *copy;
    if (!string_argument(vm, alias_ref, &alias, "KeyStore alias")) return true;
    der = cert_runtime_der(vm, certificate, &der_length);
    if (!der) return fail_message(vm, "java/security/KeyStoreException", "not an X509Certificate");
    copy = cert_object_parse(der, der_length);
    if (!copy) return fail_message(vm, "java/security/KeyStoreException", "invalid certificate");
    for (size_t i = 0; i < store->count; ++i) {
        if (!strcmp(store->entries[i].alias, alias)) {
            cert_object_free(store->entries[i].certificate);
            store->entries[i].certificate = copy;
            return true;
        }
    }
    if (store->count == CERT_MAX_ENTRIES) {
        cert_object_free(copy);
        return fail_message(vm, "java/security/KeyStoreException", "too many certificates");
    }
    if (store->count == store->capacity) {
        size_t capacity = store->capacity ? store->capacity * 2u : 8u;
        struct cert_entry *grown = realloc(store->entries, capacity * sizeof(*grown));
        if (!grown) {
            cert_object_free(copy);
            return fail_message(vm, "java/lang/OutOfMemoryError", "KeyStore entries");
        }
        store->entries = grown;
        store->capacity = capacity;
    }
    store->entries[store->count].alias = duplicate_string(alias);
    if (!store->entries[store->count].alias) {
        cert_object_free(copy);
        return fail_message(vm, "java/lang/OutOfMemoryError", "KeyStore alias");
    }
    store->entries[store->count++].certificate = copy;
    return true;
}

static bool public_key_call(struct dvm *vm, struct public_key_payload *key,
                            const char *method, const char *sig,
                            const union dvm_value *args, int nargs,
                            union dvm_value *out)
{
    if (is_call(method, sig, "getAlgorithm", "()Ljava/lang/String;") && nargs == 0) {
        out->l = dvm_new_string(vm, key->algorithm);
        return true;
    }
    if (is_call(method, sig, "getFormat", "()Ljava/lang/String;") && nargs == 0) {
        out->l = dvm_new_string(vm, "X.509");
        return true;
    }
    if (is_call(method, sig, "getEncoded", "()[B") && nargs == 0) {
        out->l = dvm_new_array(vm, 'B', "B", (uint32_t)key->length);
        if (!out->l) return fail_message(vm, "java/lang/OutOfMemoryError", "public key encoding");
        memcpy(dvm_array_data(vm, out->l), key->spki, key->length);
        return true;
    }
    if (is_call(method, sig, "equals", "(Ljava/lang/Object;)Z") && nargs == 1) {
        struct public_key_payload *other = payload(vm, args[0].l, public_key_payload_drop);
        out->i = other && other->length == key->length &&
                 !memcmp(other->spki, key->spki, key->length);
        return true;
    }
    if (is_call(method, sig, "hashCode", "()I") && nargs == 0) {
        uint32_t hash = 1;
        for (size_t i = 0; i < key->length; ++i) hash = hash * 31u + key->spki[i];
        out->i = (int32_t)hash;
        return true;
    }
    return false;
}

static bool certificate_call(struct dvm *vm, struct cert_payload *certificate,
                             const char *method, const char *sig,
                             const union dvm_value *args, int nargs,
                             union dvm_value *out)
{
    size_t length;
    const unsigned char *der = cert_object_der(certificate->object, &length);
    if (is_call(method, sig, "getEncoded", "()[B") && nargs == 0) {
        out->l = dvm_new_array(vm, 'B', "B", (uint32_t)length);
        if (!out->l) return fail_message(vm, "java/lang/OutOfMemoryError", "certificate encoding");
        memcpy(dvm_array_data(vm, out->l), der, length);
        return true;
    }
    if (is_call(method, sig, "getType", "()Ljava/lang/String;") && nargs == 0) {
        out->l = dvm_new_string(vm, "X.509");
        return true;
    }
    if (is_call(method, sig, "equals", "(Ljava/lang/Object;)Z") && nargs == 1) {
        size_t other_length;
        const unsigned char *other = cert_runtime_der(vm, args[0].l, &other_length);
        out->i = other && other_length == length && !memcmp(other, der, length);
        return true;
    }
    if (is_call(method, sig, "hashCode", "()I") && nargs == 0) {
        uint32_t hash = 1;
        for (size_t i = 0; i < length; ++i) hash = hash * 31u + der[i];
        out->i = (int32_t)hash;
        return true;
    }
    if (is_call(method, sig, "getPublicKey", "()Ljava/security/PublicKey;") && nargs == 0) {
        out->l = new_public_key(vm, certificate->object);
        return out->l != 0;
    }
    if (is_call(method, sig, "verify", "(Ljava/security/PublicKey;)V") && nargs == 1)
        return fail_message(vm, "java/security/SignatureException", "certificate signature verification unsupported");
    return false;
}

static bool factory_call(struct dvm *vm, struct factory_payload *factory,
                         const char *method, const char *sig,
                         const union dvm_value *args, int nargs,
                         union dvm_value *out)
{
    if (is_call(method, sig, "getType", "()Ljava/lang/String;") && nargs == 0) {
        out->l = dvm_new_string(vm, factory->type);
        return true;
    }
    if (is_call(method, sig, "generateCertificate",
                "(Ljava/io/InputStream;)Ljava/security/cert/Certificate;") && nargs == 1) {
        unsigned char *bytes = NULL;
        size_t length = 0;
        cert_object *parsed;
        if (!read_stream(vm, args[0].l, &bytes, &length) || dvm_exception(vm)) {
            free(bytes);
            return true;
        }
        parsed = cert_object_parse(bytes, length);
        free(bytes);
        if (!parsed) return fail_message(vm, "java/security/cert/CertificateException", "invalid certificate input");
        out->l = new_certificate(vm, parsed);
        return out->l != 0;
    }
    if (is_call(method, sig, "generateCertificates",
                "(Ljava/io/InputStream;)Ljava/util/Collection;") && nargs == 1)
        return fail_message(vm, "java/security/cert/CertificateException", "certificate collection parsing unsupported");
    return false;
}

static bool keystore_call(struct dvm *vm, struct keystore_payload *store,
                          const char *method, const char *sig,
                          const union dvm_value *args, int nargs,
                          union dvm_value *out)
{
    if (is_call(method, sig, "load", "(Ljava/io/InputStream;[C)V") && nargs == 2) {
        if (store->system_store)
            return fail_message(vm, "java/security/KeyStoreException", "system KeyStore enumeration unavailable");
        if (args[0].l)
            return fail_message(vm, "java/security/KeyStoreException", "serialized KeyStore loading unsupported");
        store->loaded = true;
        return true;
    }
    if (is_call(method, sig, "load", "(Ljava/security/KeyStore$LoadStoreParameter;)V") && nargs == 1) {
        if (store->system_store)
            return fail_message(vm, "java/security/KeyStoreException", "system KeyStore enumeration unavailable");
        if (args[0].l)
            return fail_message(vm, "java/security/KeyStoreException", "KeyStore parameters unsupported");
        store->loaded = true;
        return true;
    }
    if (is_call(method, sig, "setCertificateEntry",
                "(Ljava/lang/String;Ljava/security/cert/Certificate;)V") && nargs == 2) {
        if (!store->loaded) return fail_message(vm, "java/security/KeyStoreException", "KeyStore is not loaded");
        return keystore_set_certificate(vm, store, args[0].l, args[1].l);
    }
    if (is_call(method, sig, "containsAlias", "(Ljava/lang/String;)Z") && nargs == 1) {
        const char *alias;
        if (!string_argument(vm, args[0].l, &alias, "KeyStore alias")) return true;
        for (size_t i = 0; i < store->count; ++i)
            if (!strcmp(store->entries[i].alias, alias)) { out->i = 1; return true; }
        out->i = 0;
        return true;
    }
    if (is_call(method, sig, "size", "()I") && nargs == 0) {
        if (!store->loaded) return fail_message(vm, "java/security/KeyStoreException", "KeyStore is not loaded");
        out->i = store->count > INT_MAX ? INT_MAX : (int)store->count;
        return true;
    }
    if (is_call(method, sig, "getCertificate",
                "(Ljava/lang/String;)Ljava/security/cert/Certificate;") && nargs == 1) {
        const char *alias;
        if (!string_argument(vm, args[0].l, &alias, "KeyStore alias")) return true;
        for (size_t i = 0; i < store->count; ++i) {
            if (!strcmp(store->entries[i].alias, alias)) {
                size_t length;
                const unsigned char *der = cert_object_der(store->entries[i].certificate, &length);
                out->l = cert_runtime_new_x509(vm, der, length);
                return out->l != 0;
            }
        }
        out->l = 0;
        return true;
    }
    if (is_call(method, sig, "isCertificateEntry", "(Ljava/lang/String;)Z") && nargs == 1) {
        return keystore_call(vm, store, "containsAlias", "(Ljava/lang/String;)Z", args, nargs, out);
    }
    if (is_call(method, sig, "getCertificateAlias",
                "(Ljava/security/cert/Certificate;)Ljava/lang/String;") && nargs == 1) {
        size_t wanted_length;
        const unsigned char *wanted = cert_runtime_der(vm, args[0].l, &wanted_length);
        if (!wanted) return fail_message(vm, "java/security/KeyStoreException", "not a certificate");
        for (size_t i = 0; i < store->count; ++i) {
            size_t length;
            const unsigned char *der = cert_object_der(store->entries[i].certificate, &length);
            if (length == wanted_length && !memcmp(der, wanted, length)) {
                out->l = dvm_new_string(vm, store->entries[i].alias);
                return true;
            }
        }
        out->l = 0;
        return true;
    }
    if (is_call(method, sig, "store", "(Ljava/io/OutputStream;[C)V") && nargs == 2)
        return fail_message(vm, "java/security/KeyStoreException", "KeyStore serialization unsupported");
    if (is_call(method, sig, "aliases", "()Ljava/util/Enumeration;") && nargs == 0)
        return fail_message(vm, "java/lang/UnsupportedOperationException", "KeyStore aliases enumeration unsupported");
    return false;
}

static bool digest_call(struct dvm *vm, struct digest_payload *digest,
                        const char *method, const char *sig,
                        const union dvm_value *args, int nargs,
                        union dvm_value *out)
{
    if (is_call(method, sig, "getAlgorithm", "()Ljava/lang/String;") && nargs == 0) {
        out->l = dvm_new_string(vm, digest->algorithm);
        return true;
    }
    if (is_call(method, sig, "getDigestLength", "()I") && nargs == 0) {
        size_t length;
        if (!digest_name(digest->algorithm, &length)) return fail_message(vm, "java/security/NoSuchAlgorithmException", "unsupported digest");
        out->i = (int)length;
        return true;
    }
    if (is_call(method, sig, "reset", "()V") && nargs == 0) {
        digest->length = 0;
        return true;
    }
    if (is_call(method, sig, "update", "([B)V") && nargs == 1) {
        const unsigned char *data;
        uint32_t length;
        if (!byte_array(vm, args[0].l, &data, &length, "digest input")) return true;
        return digest_append(vm, digest, data, length);
    }
    if (is_call(method, sig, "update", "([BII)V") && nargs == 3) {
        const unsigned char *data;
        if (!byte_array_range(vm, args[0].l, args[1].i, args[2].i, &data)) return true;
        return digest_append(vm, digest, data, (size_t)args[2].i);
    }
    if (is_call(method, sig, "update", "(B)V") && nargs == 1) {
        unsigned char byte = (unsigned char)args[0].i;
        return digest_append(vm, digest, &byte, 1);
    }
    if (is_call(method, sig, "digest", "([B)[B") && nargs == 1) {
        const unsigned char *data;
        uint32_t input_length;
        dvm_ref result;
        size_t length;
        if (!byte_array(vm, args[0].l, &data, &input_length, "digest input")) return true;
        if (!digest_name(digest->algorithm, &length))
            return fail_message(vm, "java/security/NoSuchAlgorithmException", "unsupported digest");
        if (!digest_append(vm, digest, data, input_length)) return true;
        result = dvm_new_array(vm, 'B', "B", (uint32_t)length);
        if (!result) return fail_message(vm, "java/lang/OutOfMemoryError", "digest result");
        if (!digest_compute(vm, digest, dvm_array_data(vm, result), length)) return true;
        digest->length = 0;
        out->l = result;
        return true;
    }
    if (is_call(method, sig, "digest", "()[B") && nargs == 0) {
        size_t length;
        dvm_ref result;
        if (!digest_name(digest->algorithm, &length)) return fail_message(vm, "java/security/NoSuchAlgorithmException", "unsupported digest");
        result = dvm_new_array(vm, 'B', "B", (uint32_t)length);
        if (!result) return fail_message(vm, "java/lang/OutOfMemoryError", "digest result");
        if (!digest_compute(vm, digest, dvm_array_data(vm, result), length)) return true;
        digest->length = 0;
        out->l = result;
        return true;
    }
    if (is_call(method, sig, "digest", "([BII)I") && nargs == 3) {
        size_t length;
        unsigned char result[32];
        const unsigned char *destination;
        if (!byte_array_range(vm, args[0].l, args[1].i, args[2].i, &destination)) return true;
        if (!digest_name(digest->algorithm, &length)) return fail_message(vm, "java/security/NoSuchAlgorithmException", "unsupported digest");
        if ((size_t)args[2].i < length) return fail_message(vm, "java/security/ShortBufferException", "digest output buffer too small");
        if (!digest_compute(vm, digest, result, length)) return true;
        memcpy((unsigned char *)destination, result, length);
        digest->length = 0;
        out->i = (int)length;
        return true;
    }
    return false;
}

static bool certificate_factory_static(struct dvm *vm, const char *method,
                                       const char *sig, const union dvm_value *args,
                                       int nargs, union dvm_value *out)
{
    const char *algorithm;
    struct factory_payload *factory;
    dvm_ref object;
    if (!is_call(method, sig, "getInstance",
                 "(Ljava/lang/String;)Ljava/security/cert/CertificateFactory;") || nargs != 1)
        return false;
    if (!string_argument(vm, args[0].l, &algorithm, "certificate factory type")) return true;
    if (strcasecmp(algorithm, "X.509") && strcasecmp(algorithm, "X509"))
        return failf(vm, "java/security/NoSuchAlgorithmException", "unsupported certificate type %s", algorithm);
    factory = calloc(1, sizeof(*factory));
    if (!factory) return fail_message(vm, "java/lang/OutOfMemoryError", "CertificateFactory");
    factory->type = duplicate_string("X.509");
    object = dvm_new_object(vm, dvm_find_class_or_external(vm, "java/security/cert/CertificateFactory"));
    if (!object || !factory->type || !attach_payload(vm, object, factory, factory_payload_drop)) {
        factory_payload_drop(factory);
        return fail_message(vm, "java/lang/OutOfMemoryError", "CertificateFactory");
    }
    out->l = object;
    return true;
}

static bool keystore_static(struct dvm *vm, const char *method, const char *sig,
                            const union dvm_value *args, int nargs,
                            union dvm_value *out)
{
    const char *type;
    struct keystore_payload *store;
    dvm_ref object;
    if (is_call(method, sig, "getDefaultType", "()Ljava/lang/String;") && nargs == 0) {
        out->l = dvm_new_string(vm, "BKS");
        return true;
    }
    if (!is_call(method, sig, "getInstance", "(Ljava/lang/String;)Ljava/security/KeyStore;") || nargs != 1)
        return false;
    if (!string_argument(vm, args[0].l, &type, "KeyStore type")) return true;
    if (strcasecmp(type, "BKS") && strcasecmp(type, "PKCS12") && strcasecmp(type, "AndroidCAStore"))
        return failf(vm, "java/security/KeyStoreException", "unsupported KeyStore type %s", type);
    store = calloc(1, sizeof(*store));
    if (!store) return fail_message(vm, "java/lang/OutOfMemoryError", "KeyStore");
    store->type = duplicate_string(type);
    store->system_store = !strcasecmp(type, "AndroidCAStore");
    object = dvm_new_object(vm, dvm_find_class_or_external(vm, "java/security/KeyStore"));
    if (!object || !store->type || !attach_payload(vm, object, store, keystore_payload_drop)) {
        keystore_payload_drop(store);
        return fail_message(vm, "java/lang/OutOfMemoryError", "KeyStore");
    }
    out->l = object;
    return true;
}

static bool digest_static(struct dvm *vm, const char *method, const char *sig,
                          const union dvm_value *args, int nargs,
                          union dvm_value *out)
{
    const char *algorithm;
    struct digest_payload *digest;
    dvm_ref object;
    size_t length;
    if (is_call(method, sig, "isEqual", "([B[B)Z") && nargs == 2) {
        const unsigned char *left, *right;
        uint32_t left_length, right_length;
        if (!byte_array(vm, args[0].l, &left, &left_length, "digest comparison") ||
            !byte_array(vm, args[1].l, &right, &right_length, "digest comparison")) return true;
        out->i = left_length == right_length && !memcmp(left, right, left_length);
        return true;
    }
    if (!is_call(method, sig, "getInstance", "(Ljava/lang/String;)Ljava/security/MessageDigest;") || nargs != 1)
        return false;
    if (!string_argument(vm, args[0].l, &algorithm, "digest algorithm")) return true;
    if (!digest_name(algorithm, &length))
        return failf(vm, "java/security/NoSuchAlgorithmException", "unsupported digest %s", algorithm);
    (void)length;
    digest = calloc(1, sizeof(*digest));
    if (!digest) return fail_message(vm, "java/lang/OutOfMemoryError", "MessageDigest");
    if (!strcasecmp(algorithm, "SHA1")) digest->algorithm = duplicate_string("SHA-1");
    else if (!strcasecmp(algorithm, "SHA256")) digest->algorithm = duplicate_string("SHA-256");
    else digest->algorithm = duplicate_string(algorithm);
    object = dvm_new_object(vm, dvm_find_class_or_external(vm, "java/security/MessageDigest"));
    if (!object || !digest->algorithm || !attach_payload(vm, object, digest, digest_payload_drop)) {
        digest_payload_drop(digest);
        return fail_message(vm, "java/lang/OutOfMemoryError", "MessageDigest");
    }
    out->l = object;
    return true;
}

static void declare_class(struct dvm *vm, const char *name, const char *super,
                          bool interface_type)
{
    struct dvm_class *klass = dvm_find_class_or_external(vm, name);
    struct dvm_class *parent = super ? dvm_find_class_or_external(vm, super) : NULL;
    if (!klass || !klass->external) return;
    if (parent) klass->super = parent;
    klass->is_interface = interface_type;
}

void cert_runtime_vm_start(struct dvm *vm)
{
    active_vm = vm;
    exception_hierarchy_vm_start(vm);
    declare_class(vm, "java/security/cert/Certificate", "java/lang/Object", false);
    declare_class(vm, "java/security/cert/X509Certificate", "java/security/cert/Certificate", false);
    declare_class(vm, "java/security/PublicKey", "java/lang/Object", true);
    declare_class(vm, "java/security/cert/CertificateFactory", "java/lang/Object", false);
    declare_class(vm, "java/security/KeyStore", "java/lang/Object", false);
    declare_class(vm, "java/security/KeyStore$LoadStoreParameter", "java/lang/Object", true);
    declare_class(vm, "java/security/MessageDigest", "java/lang/Object", false);
}

bool cert_runtime_call(struct dvm *vm, const char *class_name, const char *method,
                       const char *sig, dvm_ref self,
                       const union dvm_value *args, int nargs,
                       union dvm_value *out)
{
    struct cert_payload *certificate;
    struct public_key_payload *public_key;
    struct keystore_payload *store;
    struct factory_payload *factory;
    struct digest_payload *digest;
    if (vm != active_vm) return false;
    if (self) {
        certificate = payload(vm, self, cert_payload_drop);
        if (certificate) return certificate_call(vm, certificate, method, sig, args, nargs, out);
        public_key = payload(vm, self, public_key_payload_drop);
        if (public_key) return public_key_call(vm, public_key, method, sig, args, nargs, out);
        store = payload(vm, self, keystore_payload_drop);
        if (store) return keystore_call(vm, store, method, sig, args, nargs, out);
        factory = payload(vm, self, factory_payload_drop);
        if (factory) return factory_call(vm, factory, method, sig, args, nargs, out);
        digest = payload(vm, self, digest_payload_drop);
        if (digest) return digest_call(vm, digest, method, sig, args, nargs, out);
    }
    if (!strcmp(class_name, "java/security/cert/CertificateFactory"))
        return certificate_factory_static(vm, method, sig, args, nargs, out);
    if (!strcmp(class_name, "java/security/KeyStore"))
        return keystore_static(vm, method, sig, args, nargs, out);
    if (!strcmp(class_name, "java/security/MessageDigest"))
        return digest_static(vm, method, sig, args, nargs, out);
    return false;
}

bool cert_runtime_selftest(struct dvm *vm)
{
    static const unsigned char sha256_abc[32] = {
        0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
        0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
        0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
        0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad
    };
    bool okay;
    union dvm_value out = {0};
    dvm_ref certificate = 0;
    dvm_ref public_key = 0;
    dvm_ref digest = 0;
    dvm_ref store = 0;
    dvm_ref alias = 0;
    dvm_ref input = 0;
    dvm_ref encoded = 0;
    dvm_ref fetched = 0;
    size_t length = 0;
    const unsigned char *der;
    unsigned char hash[32];

    okay = vm == active_vm &&
           dvm_find_class_or_external(vm, "java/security/cert/X509Certificate") != NULL &&
           dvm_find_class_or_external(vm, "java/security/PublicKey") != NULL &&
           dvm_find_class_or_external(vm, "java/security/KeyStore") != NULL &&
           dvm_find_class_or_external(vm, "java/security/MessageDigest") != NULL;
    if (okay) {
        certificate = cert_runtime_new_x509(vm, fixture_valid, sizeof fixture_valid);
        okay = certificate != 0 && !dvm_exception(vm);
    }
    if (okay) {
        der = cert_runtime_der(vm, certificate, &length);
        okay = der && length == sizeof fixture_valid && !memcmp(der, fixture_valid, length);
    }
    if (okay) {
        memset(&out, 0, sizeof out);
        okay = cert_runtime_call(vm, "java/security/cert/X509Certificate", "getPublicKey",
                                 "()Ljava/security/PublicKey;", certificate, NULL, 0, &out) &&
               out.l != 0 && !dvm_exception(vm);
        public_key = out.l;
    }
    if (okay) {
        union dvm_value result = {0};
        okay = cert_runtime_call(vm, "java/security/PublicKey", "getAlgorithm",
                                 "()Ljava/lang/String;", public_key, NULL, 0, &result) &&
               result.l && !strcmp(dvm_string_utf8(vm, result.l), "RSA");
    }
    if (okay) {
        union dvm_value result = {0};
        cert_runtime_call(vm, "java/security/PublicKey", "getEncoded", "()[B",
                          public_key, NULL, 0, &result);
        encoded = result.l;
        if (!encoded)
            okay = false;
        else {
            size_t spki_length = 0;
            struct cert_payload *certificate_state =
                payload(vm, certificate, cert_payload_drop);
            const unsigned char *spki = certificate_state
                ? cert_object_spki(certificate_state->object, &spki_length) : NULL;
            okay = spki && dvm_array_length(vm, encoded) == spki_length &&
                   !memcmp(dvm_array_data(vm, encoded), spki, spki_length) &&
                   cert_sha256(spki, spki_length, hash) == 0 &&
                   !memcmp(hash, expected_spki_hash, sizeof hash);
        }
    }
    if (okay) {
        union dvm_value args[1] = {{0}};
        args[0].l = dvm_new_string(vm, "SHA-256");
        okay = args[0].l && cert_runtime_call(vm, "java/security/MessageDigest", "getInstance",
                                              "(Ljava/lang/String;)Ljava/security/MessageDigest;",
                                              0, args, 1, &out) && out.l != 0;
        digest = out.l;
    }
    if (okay) {
        union dvm_value args[1] = {{0}}, result = {0};
        input = dvm_new_array(vm, 'B', "B", 3);
        if (!input) okay = false;
        else {
            memcpy(dvm_array_data(vm, input), "abc", 3);
            args[0].l = input;
            okay = cert_runtime_call(vm, "java/security/MessageDigest", "digest", "([B)[B",
                                     digest, args, 1, &result) && result.l &&
                   dvm_array_length(vm, result.l) == sizeof sha256_abc &&
                   !memcmp(dvm_array_data(vm, result.l), sha256_abc, sizeof sha256_abc);
        }
    }
    if (okay) {
        union dvm_value args[1] = {{0}};
        args[0].l = dvm_new_string(vm, "BKS");
        okay = args[0].l && cert_runtime_call(vm, "java/security/KeyStore", "getInstance",
                                              "(Ljava/lang/String;)Ljava/security/KeyStore;",
                                              0, args, 1, &out) && out.l != 0;
        store = out.l;
    }
    if (okay) {
        union dvm_value args[2] = {{0}}, result = {0};
        okay = cert_runtime_call(vm, "java/security/KeyStore", "load",
                                 "(Ljava/io/InputStream;[C)V", store, args, 2, &result);
    }
    if (okay) {
        union dvm_value args[2] = {{0}}, result = {0};
        args[0].l = dvm_new_string(vm, "fixture");
        args[1].l = certificate;
        okay = args[0].l && cert_runtime_call(vm, "java/security/KeyStore", "setCertificateEntry",
                                              "(Ljava/lang/String;Ljava/security/cert/Certificate;)V",
                                              store, args, 2, &result);
        alias = args[0].l;
    }
    if (okay) {
        union dvm_value result = {0};
        okay = cert_runtime_call(vm, "java/security/KeyStore", "containsAlias",
                                 "(Ljava/lang/String;)Z", store,
                                 (union dvm_value[]){{.l = alias}}, 1, &result) && result.i &&
               cert_runtime_call(vm, "java/security/KeyStore", "getCertificate",
                                 "(Ljava/lang/String;)Ljava/security/cert/Certificate;", store,
                                 (union dvm_value[]){{.l = alias}}, 1, &result) &&
               result.l != 0;
        fetched = result.l;
    }
    if (dvm_exception(vm)) {
        okay = false;
        dvm_clear_exception(vm);
    }
    fprintf(stderr, "CERT_RUNTIME_SELFTEST %s x509=%d public_key=%d digest=%d keystore=%d\n",
            okay ? "PASS" : "FAIL",
            dvm_find_class_or_external(vm, "java/security/cert/X509Certificate") != NULL,
            public_key != 0, digest != 0, store != 0);
    (void)fetched;
    return okay;
}
