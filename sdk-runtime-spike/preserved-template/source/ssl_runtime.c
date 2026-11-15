#include "ssl_runtime.h"
#include "cert_runtime.h"
#include "socket_shared.h"
#include "socket_runtime.h"
#include "exception_hierarchy.h"
#include "tls_client.h"
#include "trust.h"
#include "fixture_der.h"

#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
extern int sdk_trace_fprintf(FILE *, const char *, ...);
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#ifndef EAI_NONAME
#define EAI_NONAME -2
#endif

#define SSL_ARRAY_MAX 128
#define SSL_IO_CHUNK 65536u

static struct dvm *active_vm;
static dvm_ref default_context_ref;
static dvm_ref default_factory_ref;

struct trust_config {
    bool system;
    bool custom;
    unsigned char **ca;
    size_t *ca_len;
    size_t count;
    struct dvm *vm;
    dvm_ref manager;
};

struct trust_manager_state {
    struct trust_config config;
};

struct context_state {
    struct trust_config config;
    bool initialized;
    char *protocol;
};

struct factory_state {
    struct trust_config config;
    char *protocol;
    bool initialized;
};

struct ssl_socket;

struct trust_call_context {
    struct dvm *vm;
    struct trust_config *config;
    const char *host;
    int reject_reason;
};

struct ssl_socket {
    pthread_mutex_t lock;
    pthread_mutex_t operation_lock;
    pthread_cond_t operation_cond;
    pthread_t operation_owner;
    unsigned operation_depth;
    bool operation_busy;
    unsigned refs;
    unsigned active;
    bool closed;
    bool handshake;
    bool handshake_started;
    bool auto_close;
    bool plain_release_pending;
    bool plain_ref_pinned;
    bool client_auth_requested;
    struct dvm *vm;
    struct socket_state *plain;
    dvm_ref plain_ref;
    tls_client *client;
    char *host;
    char **protocols;
    char **ciphers;
    struct trust_config trust;
    struct trust_call_context trust_call;
};

struct ssl_stream {
    struct ssl_socket *socket;
    bool input;
};

struct ssl_session {
    struct ssl_socket *socket;
    bool valid;
};

struct ssl_session_context {
    struct dvm *vm;
};

static void trust_config_clear(struct trust_config *config);
static void ssl_socket_close(struct ssl_socket *socket);
static void ssl_socket_release(struct ssl_socket *socket);
static void ssl_socket_drop(void *data);
static bool ssl_operation_enter(struct dvm *vm, struct ssl_socket *socket);
static void ssl_operation_leave(struct ssl_socket *socket);
static void ssl_stream_drop(void *data);
static void ssl_session_drop(void *data);
static void ssl_context_drop(void *data);
static void ssl_factory_drop(void *data);
static void trust_manager_drop(void *data);
static void session_context_drop(void *data);

static bool is_call(const char *method, const char *sig,
                    const char *want_method, const char *want_sig)
{
    return method && sig && !strcmp(method, want_method) && !strcmp(sig, want_sig);
}

static bool fail_message(struct dvm *vm, const char *type, const char *message)
{
    dvm__throw(vm, type, "%s", message ? message : "TLS operation failed");
    return true;
}

static bool failf(struct dvm *vm, const char *type, const char *format, ...)
{
    va_list ap;
    char message[256];
    va_start(ap, format);
    vsnprintf(message, sizeof(message), format, ap);
    va_end(ap);
    return fail_message(vm, type, message);
}

static bool ssl_error(struct dvm *vm, const char *message)
{
    return fail_message(vm, "javax/net/ssl/SSLException", message);
}

static bool handshake_error(struct dvm *vm, const char *message)
{
    return fail_message(vm, "javax/net/ssl/SSLHandshakeException", message);
}

static bool socket_error(struct dvm *vm, const char *message)
{
    return fail_message(vm, "java/net/SocketException", message);
}

static void *object_payload(struct dvm *vm, dvm_ref ref,
                            void (*destroy)(void *))
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

static bool class_is(struct dvm *vm, dvm_ref ref, const char *name)
{
    struct dvm_class *klass = ref ? dvm_object_class(vm, ref) : NULL;
    return klass && klass->name && !strcmp(klass->name, name);
}

static bool class_assignable(struct dvm *vm, dvm_ref ref, const char *name)
{
    struct dvm_class *from = ref ? dvm_object_class(vm, ref) : NULL;
    struct dvm_class *to = dvm_find_class_or_external(vm, name);
    return from && to && dvm__class_assignable(vm, from, to);
}

static bool string_argument(struct dvm *vm, dvm_ref ref, const char **out,
                            const char *what)
{
    const char *text;
    size_t length;
    if (!ref) return failf(vm, "java/lang/NullPointerException", "%s", what);
    text = dvm_string_utf8(vm, ref);
    length = dvm_string_utf8_length(vm, ref);
    if (!text || strlen(text) != length)
        return failf(vm, "java/lang/IllegalArgumentException", "%s contains NUL", what);
    if (!*text) return failf(vm, "java/lang/IllegalArgumentException", "%s is empty", what);
    *out = text;
    return true;
}

static bool copy_string_argument(struct dvm *vm, dvm_ref ref, char **out,
                                 const char *what)
{
    const char *text;
    if (!string_argument(vm, ref, &text, what)) return false;
    *out = strdup(text);
    if (!*out) {
        fail_message(vm, "java/lang/OutOfMemoryError", what);
        return false;
    }
    return true;
}

static void trust_config_init_system(struct trust_config *config, struct dvm *vm)
{
    memset(config, 0, sizeof(*config));
    config->system = true;
    config->vm = vm;
}

static void trust_config_clear(struct trust_config *config)
{
    if (!config) return;
    if (config->manager && config->vm) dvm_unpin(config->vm, config->manager);
    for (size_t i = 0; i < config->count; ++i) free(config->ca[i]);
    free(config->ca);
    free(config->ca_len);
    memset(config, 0, sizeof(*config));
}

static bool trust_config_copy(struct dvm *vm, struct trust_config *dst,
                              const struct trust_config *src)
{
    memset(dst, 0, sizeof(*dst));
    dst->system = src->system;
    dst->custom = src->custom;
    dst->vm = vm;
    dst->count = src->count;
    if (src->manager) {
        dst->manager = src->manager;
        dvm_pin(vm, dst->manager);
    }
    if (!src->count) return true;
    dst->ca = calloc(src->count, sizeof(*dst->ca));
    dst->ca_len = calloc(src->count, sizeof(*dst->ca_len));
    if (!dst->ca || !dst->ca_len) {
        trust_config_clear(dst);
        fail_message(vm, "java/lang/OutOfMemoryError", "TLS trust anchors");
        return false;
    }
    for (size_t i = 0; i < src->count; ++i) {
        dst->ca[i] = malloc(src->ca_len[i]);
        if (!dst->ca[i]) {
            trust_config_clear(dst);
            fail_message(vm, "java/lang/OutOfMemoryError", "TLS trust anchor");
            return false;
        }
        memcpy(dst->ca[i], src->ca[i], src->ca_len[i]);
        dst->ca_len[i] = src->ca_len[i];
    }
    return true;
}

static bool trust_config_from_keystore(struct dvm *vm, dvm_ref keystore,
                                       struct trust_config *config)
{
    const unsigned char **der = NULL;
    size_t *length = NULL;
    long total;
    memset(config, 0, sizeof(*config));
    if (!keystore) {
        trust_config_init_system(config, vm);
        return true;
    }
    total = cert_runtime_keystore_trusted(vm, keystore, NULL, NULL, 0);
    if (total < 0) {
        return fail_message(vm, "java/security/KeyStoreException", "unsupported KeyStore");
    }
    config->vm = vm;
    config->system = false;
    config->count = (size_t)total;
    if (!total) return true;
    der = calloc((size_t)total, sizeof(*der));
    length = calloc((size_t)total, sizeof(*length));
    config->ca = calloc((size_t)total, sizeof(*config->ca));
    config->ca_len = calloc((size_t)total, sizeof(*config->ca_len));
    if (!der || !length || !config->ca || !config->ca_len) {
        free(der); free(length); trust_config_clear(config);
        return fail_message(vm, "java/lang/OutOfMemoryError", "KeyStore trust anchors");
    }
    if (cert_runtime_keystore_trusted(vm, keystore, der, length,
                                      (size_t)total) != total) {
        free(der); free(length); trust_config_clear(config);
        return fail_message(vm, "java/security/KeyStoreException", "cannot read KeyStore anchors");
    }
    for (long i = 0; i < total; ++i) {
        if (!der[i] || !length[i]) {
            free(der); free(length); trust_config_clear(config);
            return fail_message(vm, "java/security/KeyStoreException", "invalid KeyStore certificate");
        }
        config->ca[i] = malloc(length[i]);
        if (!config->ca[i]) {
            free(der); free(length); trust_config_clear(config);
            return fail_message(vm, "java/lang/OutOfMemoryError", "KeyStore certificate");
        }
        memcpy(config->ca[i], der[i], length[i]);
        config->ca_len[i] = length[i];
    }
    free(der);
    free(length);
    return true;
}

static struct trust_manager_state *trust_manager_payload(struct dvm *vm,
                                                          dvm_ref ref)
{
    return object_payload(vm, ref, trust_manager_drop);
}

static bool trust_config_from_manager(struct dvm *vm, dvm_ref manager,
                                      struct trust_config *config)
{
    struct trust_manager_state *state;
    struct dvm_class *klass;
    if (!manager) return fail_message(vm, "java/lang/NullPointerException", "TrustManager");
    state = trust_manager_payload(vm, manager);
    if (state) return trust_config_copy(vm, config, &state->config);
    klass = dvm_object_class(vm, manager);
    if (!klass || !dvm__class_assignable(vm, klass,
                                         dvm_find_class_or_external(vm,
                                                                    "javax/net/ssl/X509TrustManager")))
        return fail_message(vm, "java/security/KeyManagementException",
                            "no X509TrustManager supplied");
    memset(config, 0, sizeof(*config));
    config->vm = vm;
    config->custom = true;
    config->manager = manager;
    dvm_pin(vm, manager);
    return true;
}

static void trust_manager_drop(void *data)
{
    struct trust_manager_state *state = data;
    if (!state) return;
    trust_config_clear(&state->config);
    free(state);
}

static void ssl_context_drop(void *data)
{
    struct context_state *state = data;
    if (!state) return;
    trust_config_clear(&state->config);
    free(state->protocol);
    free(state);
}

static void ssl_factory_drop(void *data)
{
    struct factory_state *state = data;
    if (!state) return;
    trust_config_clear(&state->config);
    free(state);
}

static void session_context_drop(void *data)
{
    free(data);
}

/* A minimal DER reader is used only for endpoint identity. It accepts no
 * malformed input and never treats an invalid certificate as a match. */
struct der_reader {
    const unsigned char *p;
    const unsigned char *end;
};

static bool der_tlv(struct der_reader *reader, unsigned *tag,
                    const unsigned char **value, size_t *length)
{
    unsigned char first;
    size_t n = 0;
    size_t size;
    if (!reader || reader->p >= reader->end) return false;
    first = *reader->p++;
    *tag = first;
    if ((first & 0x1f) == 0x1f) return false;
    if (reader->p >= reader->end) return false;
    first = *reader->p++;
    if (!(first & 0x80)) {
        size = first;
    } else {
        n = first & 0x7f;
        if (!n || n > sizeof(size_t) || (size_t)(reader->end - reader->p) < n)
            return false;
        size = 0;
        for (size_t i = 0; i < n; ++i) {
            if (size > (SIZE_MAX >> 8)) return false;
            size = (size << 8) | reader->p[i];
        }
        reader->p += n;
    }
    if ((size_t)(reader->end - reader->p) < size) return false;
    *value = reader->p;
    *length = size;
    reader->p += size;
    return true;
}

static bool der_nested(const unsigned char *value, size_t length,
                       unsigned expected, struct der_reader *nested)
{
    struct der_reader reader = { value, value + length };
    unsigned tag;
    const unsigned char *body;
    size_t body_len;
    if (!der_tlv(&reader, &tag, &body, &body_len) || tag != expected || reader.p != reader.end)
        return false;
    nested->p = body;
    nested->end = body + body_len;
    return true;
}

static bool der_oid_is(const unsigned char *value, size_t length,
                       const unsigned char *oid, size_t oid_len)
{
    return length == oid_len && !memcmp(value, oid, oid_len);
}

static bool der_ascii_equal(const unsigned char *value, size_t length,
                            const char *text, size_t text_len)
{
    if (length != text_len) return false;
    for (size_t i = 0; i < length; ++i) {
        unsigned a = value[i], b = (unsigned char)text[i];
        if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
        if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
        if (a != b) return false;
    }
    return true;
}

static bool hostname_is_ip(const char *host, unsigned char *bytes, int *family)
{
    char buffer[INET6_ADDRSTRLEN + 2];
    size_t n;
    const char *text = host;
    if (!host) return false;
    n = strlen(host);
    if (n >= sizeof(buffer)) return false;
    if (n > 2 && host[0] == '[' && host[n - 1] == ']') {
        memcpy(buffer, host + 1, n - 2);
        buffer[n - 2] = 0;
        text = buffer;
    }
    if (inet_pton(AF_INET, text, bytes) == 1) {
        *family = AF_INET;
        return true;
    }
    if (inet_pton(AF_INET6, text, bytes) == 1) {
        *family = AF_INET6;
        return true;
    }
    return false;
}

static bool hostname_dns_valid(const char *host, size_t *length)
{
    size_t n;
    if (!host || !*host) return false;
    n = strlen(host);
    if (n > 253) return false;
    if (host[n - 1] == '.') --n;
    if (!n) return false;
    size_t label = 0;
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)host[i];
        if (c == '.') {
            if (!label) return false;
            label = 0;
            continue;
        }
        if (c > 0x7f || c <= 0x20 || c == '/' || c == '\\') return false;
        if (!(c == '-' || c == '_' || (c >= 'a' && c <= 'z') ||
              (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) return false;
        ++label;
        if (label > 63) return false;
    }
    if (!label) return false;
    *length = n;
    return true;
}

static bool wildcard_dns_match(const unsigned char *pattern, size_t pattern_len,
                               const char *host, size_t host_len)
{
    if (pattern_len >= 2 && pattern[0] == '*' && pattern[1] == '.') {
        const unsigned char *suffix = pattern + 2;
        size_t suffix_len = pattern_len - 2;
        size_t prefix_len;
        if (!suffix_len || memchr(suffix, '*', suffix_len)) return false;
        if (host_len <= suffix_len + 1 ||
            strncasecmp((const char *)host + host_len - suffix_len,
                        (const char *)suffix, suffix_len) != 0 ||
            host[host_len - suffix_len - 1] != '.') return false;
        prefix_len = host_len - suffix_len - 1;
        return prefix_len && !memchr(host, '.', prefix_len);
    }
    return der_ascii_equal(pattern, pattern_len, host, host_len);
}

static bool certificate_hostname_match(const unsigned char *der, size_t der_len,
                                       const char *host)
{
    static const unsigned char san_oid[] = { 0x55, 0x1d, 0x11 };
    static const unsigned char cn_oid[] = { 0x55, 0x04, 0x03 };
    struct der_reader outer, cert, tbs, subject, extensions;
    const unsigned char *value;
    size_t length;
    unsigned tag;
    bool has_san = false;
    bool has_dns_san = false;
    bool matched = false;
    unsigned char host_ip[16];
    int host_family = 0;
    size_t host_len = 0;

    if (!der || !der_len || !host || !*host) return false;
    if (hostname_is_ip(host, host_ip, &host_family)) {
        host_len = host_family == AF_INET ? 4 : 16;
    } else if (!hostname_dns_valid(host, &host_len)) {
        return false;
    }
    outer.p = der;
    outer.end = der + der_len;
    if (!der_tlv(&outer, &tag, &value, &length) || tag != 0x30 || outer.p != outer.end)
        return false;
    /* der_tlv already removed this SEQUENCE's tag and length. Its value is
     * the sequence body, not another complete encoded SEQUENCE. */
    cert = (struct der_reader){ value, value + length };
    if (!der_tlv(&cert, &tag, &value, &length) || tag != 0x30) return false;
    tbs.p = value;
    tbs.end = value + length;

    if (tbs.p < tbs.end && *tbs.p == 0xa0) {
        if (!der_tlv(&tbs, &tag, &value, &length)) return false;
    }
    /* serial, signature, issuer, validity, subject, subjectPublicKeyInfo */
    for (int i = 0; i < 5; ++i) {
        if (!der_tlv(&tbs, &tag, &value, &length)) return false;
        if (i == 4) {
            if (tag != 0x30) return false;
            subject = (struct der_reader){ value, value + length };
        }
    }
    if (!der_tlv(&tbs, &tag, &value, &length)) return false;
    while (tbs.p < tbs.end && (*tbs.p == 0x81 || *tbs.p == 0x82)) {
        if (!der_tlv(&tbs, &tag, &value, &length)) return false;
    }
    if (tbs.p < tbs.end && *tbs.p == 0xa3) {
        if (!der_tlv(&tbs, &tag, &value, &length) || !der_nested(value, length, 0x30, &extensions))
            return false;
        while (extensions.p < extensions.end) {
            struct der_reader extension;
            const unsigned char *ext_value;
            size_t ext_length;
            if (!der_tlv(&extensions, &tag, &value, &length) || tag != 0x30)
                return false;
            extension = (struct der_reader){ value, value + length };
            if (!der_tlv(&extension, &tag, &value, &length) || tag != 0x06) return false;
            bool is_san = der_oid_is(value, length, san_oid, sizeof(san_oid));
            if (extension.p < extension.end && *extension.p == 0x01) {
                if (!der_tlv(&extension, &tag, &value, &length)) return false;
            }
            if (!der_tlv(&extension, &tag, &ext_value, &ext_length) || tag != 0x04)
                return false;
            if (!is_san) continue;
            has_san = true;
            struct der_reader names;
            if (!der_nested(ext_value, ext_length, 0x30, &names)) return false;
            while (names.p < names.end) {
                if (!der_tlv(&names, &tag, &value, &length)) return false;
                if (tag == 0x82) {
                    has_dns_san = true;
                    if (!host_family && wildcard_dns_match(value, length, host, host_len))
                        matched = true;
                } else if (tag == 0x87) {
                    if (host_family && length == host_len && !memcmp(value, host_ip, host_len))
                        matched = true;
                }
            }
        }
    }
    if (matched) return true;
    if (has_san) return false;
    if (host_family) return false;

    /* Legacy CN fallback is used only when there was no SAN extension. */
    while (subject.p < subject.end) {
        struct der_reader set_reader, atv;
        if (!der_tlv(&subject, &tag, &value, &length) || tag != 0x31) return false;
        set_reader = (struct der_reader){ value, value + length };
        while (set_reader.p < set_reader.end) {
            if (!der_tlv(&set_reader, &tag, &value, &length) || tag != 0x30) return false;
            atv = (struct der_reader){ value, value + length };
            const unsigned char *oid;
            size_t oid_len;
            if (!der_tlv(&atv, &tag, &oid, &oid_len) || tag != 0x06) return false;
            if (!der_tlv(&atv, &tag, &value, &length)) return false;
            if (der_oid_is(oid, oid_len, cn_oid, sizeof(cn_oid)) &&
                (tag == 0x0c || tag == 0x13 || tag == 0x16 || tag == 0x1e) &&
                wildcard_dns_match(value, length, host, host_len)) return true;
        }
    }
    (void)has_dns_san;
    return false;
}

static const char *auth_name(const char *auth_type)
{
    return auth_type && *auth_type ? auth_type : "GENERIC";
}

static int custom_trust_check(struct trust_call_context *call,
                              const tls_cert_view *chain, size_t count,
                              const char *auth_type)
{
    struct dvm *vm = call->vm;
    dvm_ref manager = call->config->manager;
    dvm_ref certificates = 0;
    union dvm_value args[3] = {{0}}, out = {0};
    struct dvm_class *klass;
    struct dvm_method *method;
    const unsigned char *der[SSL_ARRAY_MAX];
    size_t lengths[SSL_ARRAY_MAX];
    size_t n;
    bool accepted;
    if (!vm || !manager || count > SSL_ARRAY_MAX) {
        call->reject_reason = 2;
        return -1;
    }
    n = count;
    for (size_t i = 0; i < n; ++i) {
        der[i] = chain[i].der;
        lengths[i] = chain[i].length;
    }
    certificates = cert_runtime_new_x509_array(vm, der, lengths, n);
    if (!certificates) {
        call->reject_reason = 2;
        return -1;
    }
    klass = dvm_object_class(vm, manager);
    method = klass ? dvm_find_method(vm, klass, "checkServerTrusted",
                                     "([Ljava/security/cert/X509Certificate;Ljava/lang/String;Ljava/lang/String;)V") : NULL;
    args[0].l = certificates;
    args[1].l = dvm_new_string(vm, auth_name(auth_type));
    args[2].l = dvm_new_string(vm, call->host);
    if (method) {
        accepted = dvm_call(vm, method, manager, args, 3, &out) && !dvm_exception(vm);
    } else {
        method = klass ? dvm_find_method(vm, klass, "checkServerTrusted",
                                         "([Ljava/security/cert/X509Certificate;Ljava/lang/String;)V") : NULL;
        if (!method) {
            call->reject_reason = 2;
            return -1;
        }
        accepted = dvm_call(vm, method, manager, args, 2, &out) && !dvm_exception(vm);
    }
    if (!accepted) call->reject_reason = 2;
    return accepted ? 0 : -1;
}

static int trust_callback(void *context, const tls_cert_view *chain,
                          size_t count, const char *auth_type)
{
    struct trust_call_context *call = context;
    TlsTrustCert peers[SSL_ARRAY_MAX];
    TlsTrustCert ca;
    TlsTrustChain verified;
    int32_t result;
    if (!call || !call->config || !call->host || !*call->host || !chain ||
        !count || count > SSL_ARRAY_MAX) {
        if (call) call->reject_reason = 3;
        return -1;
    }
    if (!certificate_hostname_match(chain[0].der, chain[0].length, call->host)) {
        call->reject_reason = 1;
        return -1;
    }
    if (call->config->custom) {
        /* Mbed TLS calls trust synchronously on the handshake thread. This
         * thread already owns the socket operation gate; reacquiring that gate
         * is intentionally avoided, while GIL acquisition restores its DVM
         * thread state before touching guest refs. */
        dvm_gil_acquire(call->vm);
        if (!dvm_gil_held(call->vm) || dvm_current() != call->vm) {
            if (dvm_gil_held(call->vm)) dvm_gil_release(call->vm);
            call->reject_reason = 2;
            return -1;
        }
        result = custom_trust_check(call, chain, count, auth_type);
        dvm_gil_release(call->vm);
        return result;
    }
    for (size_t i = 0; i < count; ++i) {
        peers[i].encoding = TLS_TRUST_DER;
        peers[i].data = chain[i].der;
        peers[i].size = chain[i].length;
    }
    if (call->config->system) {
        memset(&verified, 0, sizeof(verified));
        result = tls_trust_verify(peers, count, NULL, call->host, &verified);
        if (!result) tls_trust_free_chain(&verified);
        if (!result) return 0;
        call->reject_reason = 3;
        return -1;
    }
    for (size_t i = 0; i < call->config->count; ++i) {
        ca.encoding = TLS_TRUST_DER;
        ca.data = call->config->ca[i];
        ca.size = call->config->ca_len[i];
        memset(&verified, 0, sizeof(verified));
        result = tls_trust_verify(peers, count, &ca, call->host, &verified);
        if (!result) {
            tls_trust_free_chain(&verified);
            return 0;
        }
        sdk_trace_fprintf(stderr, "TLS_TRUST_ANCHOR_FAIL index=%zu ca_bytes=%zu peer=%zu code=%d\n",
                          i, call->config->ca_len[i], count, result);
    }
    call->reject_reason = 3;
    sdk_trace_fprintf(stderr, "TLS_TRUST_BUILTIN_FAIL host=%s system=%d anchors=%zu chain=%zu\n",
                      call->host, call->config->system, call->config->count, count);
    return -1;
}

static void ssl_socket_drop(void *data)
{
    struct ssl_socket *socket = data;
    if (!socket) return;
    ssl_socket_close(socket);
    ssl_socket_release(socket);
}

static void ssl_stream_drop(void *data)
{
    struct ssl_stream *stream = data;
    if (!stream) return;
    ssl_socket_release(stream->socket);
    free(stream);
}

static void ssl_session_drop(void *data)
{
    struct ssl_session *session = data;
    if (!session) return;
    ssl_socket_release(session->socket);
    free(session);
}

static bool ssl_socket_detach_resources_locked(struct ssl_socket *socket,
                                               tls_client **client_out,
                                               struct socket_state **plain_out)
{
    if (socket->active) return false;
    if (client_out && socket->client) {
        *client_out = socket->client;
        socket->client = NULL;
    }
    if (plain_out && socket->plain) {
        *plain_out = socket->plain;
        socket->plain = NULL;
        socket->plain_release_pending = false;
    }
    return true;
}

static void ssl_socket_close(struct ssl_socket *socket)
{
    tls_client *client = NULL;
    struct socket_state *plain = NULL;
    dvm_ref plain_ref = 0;
    bool unpin_plain = false;
    if (!socket) return;
    pthread_mutex_lock(&socket->lock);
    if (!socket->closed) {
        socket->closed = true;
        if (socket->client) tls_client_cancel(socket->client);
        if (socket->plain && socket->auto_close) socket_state_close(socket->plain);
        if (socket->plain && socket->active) socket->plain_release_pending = true;
        if (socket->plain && !socket->active)
            ssl_socket_detach_resources_locked(socket, &client, &plain);
        if (socket->plain_ref_pinned && !socket->active) {
            plain_ref = socket->plain_ref;
            socket->plain_ref_pinned = false;
            socket->plain_ref = 0;
            unpin_plain = true;
        }
    } else if (!socket->active) {
        ssl_socket_detach_resources_locked(socket, &client, &plain);
    }
    pthread_mutex_unlock(&socket->lock);
    if (client) tls_client_free(client);
    if (plain) socket_state_release(plain);
    if (unpin_plain && socket->vm) dvm_unpin(socket->vm, plain_ref);
}

static void ssl_socket_release(struct ssl_socket *socket)
{
    tls_client *client = NULL;
    struct socket_state *plain = NULL;
    dvm_ref plain_ref = 0;
    bool unpin_plain = false;
    bool destroy = false;
    if (!socket) return;
    pthread_mutex_lock(&socket->lock);
    if (socket->refs) --socket->refs;
    if (!socket->refs) {
        socket->closed = true;
        if (socket->client) tls_client_cancel(socket->client);
        if (socket->plain && socket->auto_close) socket_state_close(socket->plain);
        if (!socket->active) {
            ssl_socket_detach_resources_locked(socket, &client, &plain);
            if (socket->plain_ref_pinned) {
                plain_ref = socket->plain_ref;
                socket->plain_ref_pinned = false;
                socket->plain_ref = 0;
                unpin_plain = true;
            }
        }
        if (!socket->active && !socket->client && !socket->plain) destroy = true;
    }
    pthread_mutex_unlock(&socket->lock);
    if (client) tls_client_free(client);
    if (plain) socket_state_release(plain);
    if (unpin_plain && socket->vm) dvm_unpin(socket->vm, plain_ref);
    if (destroy) {
        trust_config_clear(&socket->trust);
        free(socket->host);
        if (socket->protocols) {
            for (size_t i = 0; socket->protocols[i]; ++i) free(socket->protocols[i]);
            free(socket->protocols);
        }
        if (socket->ciphers) {
            for (size_t i = 0; socket->ciphers[i]; ++i) free(socket->ciphers[i]);
            free(socket->ciphers);
        }
        pthread_cond_destroy(&socket->operation_cond);
        pthread_mutex_destroy(&socket->operation_lock);
        pthread_mutex_destroy(&socket->lock);
        free(socket);
    }
}

static bool ssl_operation_enter(struct dvm *vm, struct ssl_socket *socket)
{
    pthread_t self = pthread_self();
    pthread_mutex_lock(&socket->operation_lock);
    for (;;) {
        if (!socket->operation_busy) {
            socket->operation_busy = true;
            socket->operation_owner = self;
            socket->operation_depth = 1;
            pthread_mutex_unlock(&socket->operation_lock);
            return true;
        }
        if (pthread_equal(socket->operation_owner, self)) {
            ++socket->operation_depth;
            pthread_mutex_unlock(&socket->operation_lock);
            return true;
        }
        /* Register on the condition variable while holding operation_lock,
         * then release the GIL before waiting. The owner may re-enter the VM
         * from the trust callback, so waiting with the GIL held would deadlock. */
        unsigned depth = dvm_gil_unlock_all(vm);
        (void)pthread_cond_wait(&socket->operation_cond, &socket->operation_lock);
        pthread_mutex_unlock(&socket->operation_lock);
        dvm_gil_relock(vm, depth);
        pthread_mutex_lock(&socket->operation_lock);
    }
}

static void ssl_operation_leave(struct ssl_socket *socket)
{
    pthread_t self = pthread_self();
    pthread_mutex_lock(&socket->operation_lock);
    if (socket->operation_busy && pthread_equal(socket->operation_owner, self)) {
        if (socket->operation_depth > 1) {
            --socket->operation_depth;
        } else {
            socket->operation_depth = 0;
            socket->operation_busy = false;
            memset(&socket->operation_owner, 0, sizeof(socket->operation_owner));
            pthread_cond_broadcast(&socket->operation_cond);
        }
    }
    pthread_mutex_unlock(&socket->operation_lock);
}

static bool ssl_socket_begin(struct dvm *vm, struct ssl_socket *socket,
                             tls_client **client, struct socket_state **plain)
{
    int fd;
    if (!ssl_operation_enter(vm, socket)) return false;
    pthread_mutex_lock(&socket->lock);
    if (socket->closed || !socket->client || !socket->plain) {
        pthread_mutex_unlock(&socket->lock);
        ssl_operation_leave(socket);
        return false;
    }
    fd = socket_state_io_begin(socket->plain);
    if (fd < 0) {
        pthread_mutex_unlock(&socket->lock);
        ssl_operation_leave(socket);
        return false;
    }
    socket->active++;
    socket->refs++;
    *client = socket->client;
    *plain = socket->plain;
    (void)fd;
    pthread_mutex_unlock(&socket->lock);
    return true;
}

static void ssl_socket_end(struct ssl_socket *socket, struct socket_state *plain)
{
    tls_client *client = NULL;
    struct socket_state *release_plain = NULL;
    pthread_mutex_lock(&socket->lock);
    socket_state_io_end(plain);
    if (socket->active) --socket->active;
    if (socket->closed && !socket->active)
        ssl_socket_detach_resources_locked(socket, &client, &release_plain);
    pthread_mutex_unlock(&socket->lock);
    if (client) tls_client_free(client);
    if (release_plain) socket_state_release(release_plain);
    dvm_ref plain_ref = 0;
    bool unpin_plain = false;
    pthread_mutex_lock(&socket->lock);
    if (socket->closed && !socket->active && socket->plain_ref_pinned) {
        plain_ref = socket->plain_ref;
        socket->plain_ref_pinned = false;
        socket->plain_ref = 0;
        unpin_plain = true;
    }
    pthread_mutex_unlock(&socket->lock);
    if (unpin_plain && socket->vm) dvm_unpin(socket->vm, plain_ref);
    ssl_operation_leave(socket);
    ssl_socket_release(socket);
}

static bool ssl_socket_fd_for_create(struct ssl_socket *socket,
                                     int *fd, struct socket_state **plain)
{
    /* Caller owns the operation gate; this is the create-only I/O pin. */
    pthread_mutex_lock(&socket->lock);
    if (socket->closed || socket->client || !socket->plain) {
        pthread_mutex_unlock(&socket->lock);
        ssl_operation_leave(socket);
        return false;
    }
    *fd = socket_state_io_begin(socket->plain);
    if (*fd < 0) {
        pthread_mutex_unlock(&socket->lock);
        ssl_operation_leave(socket);
        return false;
    }
    socket->active++;
    socket->refs++;
    *plain = socket->plain;
    pthread_mutex_unlock(&socket->lock);
    return true;
}

static bool ssl_socket_ensure_client(struct dvm *vm, struct ssl_socket *socket)
{
    tls_client_config config;
    tls_client *created = NULL;
    struct socket_state *plain = NULL;
    int fd;
    int rc;
    char *server_name = NULL;
    bool closed;
    if (!socket) return false;
    if (!ssl_operation_enter(vm, socket)) return false;
    pthread_mutex_lock(&socket->lock);
    if (socket->closed) {
        pthread_mutex_unlock(&socket->lock);
        ssl_operation_leave(socket);
        return socket_error(vm, "Socket is closed");
    }
    if (socket->client) {
        pthread_mutex_unlock(&socket->lock);
        ssl_operation_leave(socket);
        return true;
    }
    if (!socket->plain || !socket->host) {
        pthread_mutex_unlock(&socket->lock);
        ssl_operation_leave(socket);
        return fail_message(vm, "java/net/SocketException", "Socket is not connected");
    }
    if (!hostname_is_ip(socket->host, (unsigned char[16]){0}, &(int){0}))
        server_name = socket->host;
    memset(&config, 0, sizeof(config));
    config.server_name = server_name;
    config.protocols = (const char *const *)socket->protocols;
    config.cipher_suites = (const char *const *)socket->ciphers;
    config.trust = trust_callback;
    config.trust_context = &socket->trust_call;
    pthread_mutex_unlock(&socket->lock);

    if (!ssl_socket_fd_for_create(socket, &fd, &plain))
        return socket_error(vm, "Socket is closed");
    rc = tls_client_create(fd, &config, &created);
    pthread_mutex_lock(&socket->lock);
    closed = socket->closed;
    bool installed = !rc && !closed && !socket->client;
    if (installed) socket->client = created;
    else if (created) tls_client_cancel(created);
    socket_state_io_end(plain);
    if (socket->active) --socket->active;
    pthread_mutex_unlock(&socket->lock);
    if (created && !installed) tls_client_free(created);
    if (closed) {
        ssl_operation_leave(socket);
        ssl_socket_release(socket);
        return socket_error(vm, "Socket is closed");
    }
    if (rc) {
        ssl_operation_leave(socket);
        ssl_socket_release(socket);
        if (rc == TLS_CLIENT_ERR_ARGUMENT)
            return fail_message(vm, "java/lang/IllegalArgumentException", "unsupported TLS protocol or cipher suite");
        return ssl_error(vm, tls_client_error_text(rc));
    }
    /* Release the create gate before the caller acquires the handshake gate.
     * The extra ref held by ssl_socket_fd_for_create protects the payload while
     * native construction is in progress. */
    ssl_operation_leave(socket);
    ssl_socket_release(socket);
    return true;
}

static bool ssl_socket_handshake(struct dvm *vm, struct ssl_socket *socket)
{
    tls_client *client;
    struct socket_state *plain;
    int timeout;
    int rc;
    int reject_reason;
    if (!ssl_socket_ensure_client(vm, socket)) return true;
    if (dvm_exception(vm)) return true;
    if (!ssl_operation_enter(vm, socket)) return socket_error(vm, "Socket is closed");
    pthread_mutex_lock(&socket->lock);
    if (socket->handshake) {
        pthread_mutex_unlock(&socket->lock);
        ssl_operation_leave(socket);
        return true;
    }
    if (socket->closed || socket->handshake_started) {
        pthread_mutex_unlock(&socket->lock);
        ssl_operation_leave(socket);
        return socket_error(vm, "TLS handshake already started or socket closed");
    }
    socket->handshake_started = true;
    timeout = socket_state_timeout(socket->plain);
    socket->trust_call.reject_reason = 0;
    pthread_mutex_unlock(&socket->lock);
    if (!ssl_socket_begin(vm, socket, &client, &plain)) {
        ssl_operation_leave(socket);
        ssl_socket_close(socket);
        return socket_error(vm, "Socket is closed");
    }
    {
        unsigned depth = dvm_gil_unlock_all(vm);
        rc = tls_client_handshake(client, timeout);
        dvm_gil_relock(vm, depth);
    }
    reject_reason = socket->trust_call.reject_reason;
    pthread_mutex_lock(&socket->lock);
    if (!rc && !socket->closed) socket->handshake = true;
    pthread_mutex_unlock(&socket->lock);
    ssl_socket_end(socket, plain);
    ssl_operation_leave(socket);
    if (!rc) return true;
    if (dvm_exception(vm)) dvm_clear_exception(vm);
    ssl_socket_close(socket);
    if (rc == TLS_CLIENT_ERR_TRUST_REJECTED || reject_reason)
        return handshake_error(vm, reject_reason == 1 ? "TLS hostname verification failed" :
                                "TLS certificate trust verification failed");
    if (rc == TLS_CLIENT_ERR_TIMEOUT)
        return fail_message(vm, "java/net/SocketTimeoutException", "TLS handshake timed out");
    if (rc == TLS_CLIENT_ERR_ARGUMENT)
        return fail_message(vm, "java/lang/IllegalArgumentException", "unsupported TLS configuration");
    return handshake_error(vm, tls_client_error_text(rc));
}

static bool byte_array_range(struct dvm *vm, dvm_ref ref, int offset, int length)
{
    struct dvm_object *object = dvm__obj(vm, ref);
    if (!object) return fail_message(vm, "java/lang/NullPointerException", "byte array");
    if (object->kind != DVM_OBJ_ARRAY || object->elem_kind != 'B')
        return fail_message(vm, "java/lang/IllegalArgumentException", "not a byte array");
    if (offset < 0 || length < 0 || (uint32_t)offset > object->length ||
        (uint32_t)length > object->length - (uint32_t)offset)
        return failf(vm, "java/lang/IndexOutOfBoundsException", "off=%d len=%d", offset, length);
    return true;
}

static bool ssl_read(struct dvm *vm, struct ssl_socket *socket, dvm_ref array,
                     int offset, int length, union dvm_value *out)
{
    unsigned char *buffer;
    size_t want;
    long result;
    tls_client *client;
    struct socket_state *plain;
    int timeout;
    if (!byte_array_range(vm, array, offset, length)) return true;
    if (!length) { out->i = 0; return true; }
    if (!ssl_socket_handshake(vm, socket) || dvm_exception(vm)) return true;
    want = length > (int)SSL_IO_CHUNK ? SSL_IO_CHUNK : (size_t)length;
    buffer = malloc(want);
    if (!buffer) return fail_message(vm, "java/lang/OutOfMemoryError", "TLS read buffer");
    pthread_mutex_lock(&socket->lock);
    timeout = socket_state_timeout(socket->plain);
    pthread_mutex_unlock(&socket->lock);
    if (!ssl_socket_begin(vm, socket, &client, &plain)) {
        free(buffer);
        return socket_error(vm, "Socket is closed");
    }
    {
        unsigned depth = dvm_gil_unlock_all(vm);
        result = tls_client_read(client, buffer, want, timeout);
        dvm_gil_relock(vm, depth);
    }
    ssl_socket_end(socket, plain);
    pthread_mutex_lock(&socket->lock);
    bool closed = socket->closed;
    pthread_mutex_unlock(&socket->lock);
    if (closed) {
        free(buffer);
        return socket_error(vm, "Socket is closed");
    }
    if (result >= 0) {
        if (result) memcpy((unsigned char *)dvm_array_data(vm, array) + offset,
                           buffer, (size_t)result);
        out->i = result == 0 ? -1 : (int)result;
        if (result > 0) dvm__note_io_progress(vm, (size_t)result);
        free(buffer);
        return true;
    }
    free(buffer);
    if (result == TLS_CLIENT_ERR_TIMEOUT)
        return fail_message(vm, "java/net/SocketTimeoutException", "TLS read timed out");
    if (result == TLS_CLIENT_ERR_PEER_CLOSED) { out->i = -1; return true; }
    if (result == TLS_CLIENT_ERR_CANCELLED)
        return socket_error(vm, "Socket closed");
    return ssl_error(vm, tls_client_error_text((int)result));
}

static bool ssl_write(struct dvm *vm, struct ssl_socket *socket, dvm_ref array,
                      int offset, int length)
{
    unsigned char *buffer;
    size_t want;
    long result;
    tls_client *client;
    struct socket_state *plain;
    int timeout;
    if (!byte_array_range(vm, array, offset, length)) return true;
    if (!ssl_socket_handshake(vm, socket) || dvm_exception(vm)) return true;
    while (length > 0) {
        want = length > (int)SSL_IO_CHUNK ? SSL_IO_CHUNK : (size_t)length;
        buffer = malloc(want);
        if (!buffer) return fail_message(vm, "java/lang/OutOfMemoryError", "TLS write buffer");
        memcpy(buffer, (unsigned char *)dvm_array_data(vm, array) + offset, want);
        pthread_mutex_lock(&socket->lock);
        timeout = socket_state_timeout(socket->plain);
        pthread_mutex_unlock(&socket->lock);
        if (!ssl_socket_begin(vm, socket, &client, &plain)) {
            free(buffer);
            return socket_error(vm, "Socket is closed");
        }
        {
            unsigned depth = dvm_gil_unlock_all(vm);
            result = tls_client_write(client, buffer, want, timeout);
            dvm_gil_relock(vm, depth);
        }
        ssl_socket_end(socket, plain);
        free(buffer);
        if (result < 0) {
            if (result == TLS_CLIENT_ERR_TIMEOUT)
                return fail_message(vm, "java/net/SocketTimeoutException", "TLS write timed out");
            if (result == TLS_CLIENT_ERR_CANCELLED)
                return socket_error(vm, "Socket closed");
            return ssl_error(vm, tls_client_error_text((int)result));
        }
        if ((size_t)result != want) return ssl_error(vm, "short TLS write");
        offset += (int)want;
        length -= (int)want;
        dvm__note_io_progress(vm, want);
    }
    return true;
}

static bool peer_certificate_array(struct dvm *vm, struct ssl_socket *socket,
                                   union dvm_value *out)
{
    tls_cert_view views[SSL_ARRAY_MAX];
    const unsigned char *der[SSL_ARRAY_MAX];
    size_t lengths[SSL_ARRAY_MAX];
    tls_client *client;
    struct socket_state *plain;
    size_t total;
    if (!ssl_socket_handshake(vm, socket) || dvm_exception(vm)) return true;
    if (!ssl_socket_begin(vm, socket, &client, &plain))
        return socket_error(vm, "Socket is closed");
    total = tls_client_peer_chain(client, views, SSL_ARRAY_MAX);
    if (!total || total > SSL_ARRAY_MAX) {
        ssl_socket_end(socket, plain);
        return fail_message(vm, "javax/net/ssl/SSLPeerUnverifiedException", "peer certificate unavailable");
    }
    for (size_t i = 0; i < total; ++i) {
        if (!views[i].der || !views[i].length) {
            ssl_socket_end(socket, plain);
            return fail_message(vm, "javax/net/ssl/SSLPeerUnverifiedException", "invalid peer certificate");
        }
        der[i] = views[i].der;
        lengths[i] = views[i].length;
    }
    out->l = cert_runtime_new_x509_array(vm, der, lengths, total);
    ssl_socket_end(socket, plain);
    return out->l != 0 || dvm_exception(vm);
}

static dvm_ref string_array(struct dvm *vm, const char *const *names, size_t count)
{
    dvm_ref result = dvm_new_array(vm, 'L', "Ljava/lang/String;", (uint32_t)count);
    if (!result) {
        fail_message(vm, "java/lang/OutOfMemoryError", "TLS string array");
        return 0;
    }
    dvm_ref *items = dvm_array_data(vm, result);
    for (size_t i = 0; i < count; ++i) {
        items[i] = dvm_new_string(vm, names[i]);
        if (!items[i]) {
            fail_message(vm, "java/lang/OutOfMemoryError", "TLS name");
            return 0;
        }
    }
    return result;
}

static bool copy_name_array(struct dvm *vm, dvm_ref array, char ***out,
                            const char *what)
{
    uint32_t count;
    char **items;
    struct dvm_object *object;
    if (!array) return fail_message(vm, "java/lang/NullPointerException", what);
    object = dvm__obj(vm, array);
    if (!object || object->kind != DVM_OBJ_ARRAY || object->elem_kind != 'L')
        return fail_message(vm, "java/lang/IllegalArgumentException", what);
    count = object->length;
    if (!count || count >= SSL_ARRAY_MAX)
        return fail_message(vm, "java/lang/IllegalArgumentException", "empty or oversized TLS name list");
    items = calloc((size_t)count + 1, sizeof(*items));
    if (!items) return fail_message(vm, "java/lang/OutOfMemoryError", what);
    for (uint32_t i = 0; i < count; ++i) {
        dvm_ref ref = ((dvm_ref *)object->data)[i];
        const char *text;
        if (!ref || !string_argument(vm, ref, &text, what)) {
            for (uint32_t j = 0; j < i; ++j) free(items[j]);
            free(items);
            return false;
        }
        items[i] = strdup(text);
        if (!items[i]) {
            for (uint32_t j = 0; j < i; ++j) free(items[j]);
            free(items);
            return fail_message(vm, "java/lang/OutOfMemoryError", what);
        }
    }
    *out = items;
    return true;
}

static void free_name_array(char **items)
{
    if (!items) return;
    for (size_t i = 0; items[i]; ++i) free(items[i]);
    free(items);
}

static bool socket_connect_plain_impl(struct dvm *vm, struct ssl_socket *socket,
                                      dvm_ref address, int timeout)
{
    union dvm_value args[2] = {{0}}, ignored = {0};
    dvm_ref host_string = 0;
    union dvm_value host_result = {0};
    const char *host;
    struct socket_state *plain;
    if (!socket->plain_ref) return fail_message(vm, "java/net/SocketException", "Socket is already layered");
    if (!address) return fail_message(vm, "java/lang/NullPointerException", "SocketAddress");
    pthread_mutex_lock(&socket->lock);
    if (socket->closed) {
        pthread_mutex_unlock(&socket->lock);
        return socket_error(vm, "Socket is closed");
    }
    if (socket->plain) {
        pthread_mutex_unlock(&socket->lock);
        return socket_error(vm, "Socket is already connected");
    }
    plain = socket_runtime_retain_for_connect(vm, socket->plain_ref);
    if (!plain) {
        pthread_mutex_unlock(&socket->lock);
        return socket_error(vm, "Socket is not available for connect");
    }
    socket->plain = plain;
    pthread_mutex_unlock(&socket->lock);
    if (!socket_runtime_call(vm, "java/net/InetSocketAddress", "getHostString",
                             "()Ljava/lang/String;", address, NULL, 0, &host_result))
        return fail_message(vm, "java/net/SocketException", "unsupported SocketAddress");
    host_string = host_result.l;
    if (dvm_exception(vm)) return true;
    if (!string_argument(vm, host_string, &host, "TLS peer host")) return false;
    free(socket->host);
    socket->host = strdup(host);
    if (!socket->host) return fail_message(vm, "java/lang/OutOfMemoryError", "TLS peer host");
    /* The trust callback runs from the native handshake thread and reads the
     * copied host independently of the SocketAddress object. Keep both views
     * synchronized for the factory.createSocket(host, port) path. */
    socket->trust_call.host = socket->host;
    args[0].l = address;
    if (timeout) {
        args[1].i = timeout;
        if (!socket_runtime_call(vm, "java/net/Socket", "connect",
                                 "(Ljava/net/SocketAddress;I)V", socket->plain_ref,
                                 args, 2, &ignored)) return fail_message(vm, "java/net/SocketException", "plain socket connect");
    } else if (!socket_runtime_call(vm, "java/net/Socket", "connect",
                                    "(Ljava/net/SocketAddress;)V", socket->plain_ref,
                                    args, 1, &ignored)) {
        return fail_message(vm, "java/net/SocketException", "plain socket connect");
    }
    if (dvm_exception(vm)) return true;
    return true;
}

static bool socket_connect_plain(struct dvm *vm, struct ssl_socket *socket,
                                 dvm_ref address, int timeout)
{
    bool result;
    if (!ssl_operation_enter(vm, socket))
        return socket_error(vm, "Socket is closed");
    pthread_mutex_lock(&socket->lock);
    if (socket->closed || !socket->plain_ref) {
        pthread_mutex_unlock(&socket->lock);
        ssl_operation_leave(socket);
        return socket_error(vm, "Socket is closed");
    }
    ++socket->active;
    ++socket->refs;
    pthread_mutex_unlock(&socket->lock);

    result = socket_connect_plain_impl(vm, socket, address, timeout);

    struct socket_state *release_plain = NULL;
    bool close_plain = false;
    pthread_mutex_lock(&socket->lock);
    if (socket->plain && (!socket_state_connected(socket->plain) || socket->closed)) {
        release_plain = socket->plain;
        socket->plain = NULL;
        socket->plain_release_pending = false;
        close_plain = socket->closed;
        if (close_plain) result = socket_error(vm, "Socket is closed");
    }
    if (socket->active) --socket->active;
    if (socket->closed && !socket->active && socket->plain_ref_pinned) {
        dvm_ref plain_ref = socket->plain_ref;
        socket->plain_ref_pinned = false;
        socket->plain_ref = 0;
        pthread_mutex_unlock(&socket->lock);
        if (socket->vm) dvm_unpin(socket->vm, plain_ref);
    } else {
        pthread_mutex_unlock(&socket->lock);
    }
    if (release_plain) {
        if (close_plain) socket_state_close(release_plain);
        socket_state_release(release_plain);
    }
    ssl_operation_leave(socket);
    ssl_socket_release(socket);
    return result;
}

static bool socket_connect_host(struct dvm *vm, struct ssl_socket *socket,
                                const char *host, int port)
{
    dvm_ref target;
    union dvm_value args[2] = {{0}}, ignored = {0};
    if (!host || !*host) return fail_message(vm, "java/lang/IllegalArgumentException", "TLS peer host");
    target = dvm_new_object(vm, dvm_find_class_or_external(vm, "java/net/InetSocketAddress"));
    if (!target) return fail_message(vm, "java/lang/OutOfMemoryError", "SocketAddress");
    args[0].l = dvm_new_string(vm, host);
    args[1].i = port;
    if (!args[0].l || !socket_runtime_call(vm, "java/net/InetSocketAddress", "<init>",
                                           "(Ljava/lang/String;I)V", target, args, 2, &ignored))
        return fail_message(vm, "java/net/SocketException", "SocketAddress");
    return socket_connect_plain(vm, socket, target, 0);
}

static bool socket_connect_address(struct dvm *vm, struct ssl_socket *socket,
                                   dvm_ref address, int port)
{
    dvm_ref host_string = 0;
    union dvm_value host_result = {0};
    const char *host;
    dvm_ref target;
    union dvm_value args[2] = {{0}}, ignored = {0};
    if (!address) return fail_message(vm, "java/lang/NullPointerException", "InetAddress");
    if (!socket_runtime_call(vm, "java/net/InetAddress", "getHostAddress",
                             "()Ljava/lang/String;", address, NULL, 0, &host_result)) return true;
    host_string = host_result.l;
    if (!string_argument(vm, host_string, &host, "TLS peer host")) return true;
    target = dvm_new_object(vm, dvm_find_class_or_external(vm, "java/net/InetSocketAddress"));
    if (!target) return fail_message(vm, "java/lang/OutOfMemoryError", "SocketAddress");
    args[0].l = address;
    args[1].i = port;
    if (!socket_runtime_call(vm, "java/net/InetSocketAddress", "<init>",
                             "(Ljava/net/InetAddress;I)V", target, args, 2, &ignored))
        return fail_message(vm, "java/net/SocketException", "SocketAddress");
    (void)host;
    return socket_connect_plain(vm, socket, target, 0);
}

static bool make_plain_socket(struct dvm *vm, dvm_ref *out)
{
    dvm_ref socket = dvm_new_object(vm, dvm_find_class_or_external(vm, "java/net/Socket"));
    union dvm_value ignored = {0};
    if (!socket) return fail_message(vm, "java/lang/OutOfMemoryError", "plain Socket");
    if (!socket_runtime_call(vm, "java/net/Socket", "<init>", "()V", socket,
                             NULL, 0, &ignored) || dvm_exception(vm)) return true;
    dvm_pin(vm, socket);
    *out = socket;
    return true;
}

static struct ssl_socket *new_ssl_socket(struct dvm *vm,
                                         const struct trust_config *config,
                                         dvm_ref plain_ref,
                                         struct socket_state *plain,
                                         bool auto_close,
                                         dvm_ref *object_out)
{
    struct ssl_socket *socket = calloc(1, sizeof(*socket));
    dvm_ref object;
    if (!socket) {
        fail_message(vm, "java/lang/OutOfMemoryError", "SSLSocket");
        return NULL;
    }
    if (pthread_mutex_init(&socket->lock, NULL)) {
        free(socket);
        fail_message(vm, "java/lang/OutOfMemoryError", "SSLSocket lock");
        return NULL;
    }
    if (pthread_mutex_init(&socket->operation_lock, NULL)) {
        pthread_mutex_destroy(&socket->lock);
        free(socket);
        fail_message(vm, "java/lang/OutOfMemoryError", "SSLSocket operation lock");
        return NULL;
    }
    if (pthread_cond_init(&socket->operation_cond, NULL)) {
        pthread_mutex_destroy(&socket->operation_lock);
        pthread_mutex_destroy(&socket->lock);
        free(socket);
        fail_message(vm, "java/lang/OutOfMemoryError", "SSLSocket operation condition");
        return NULL;
    }
    socket->refs = 1;
    socket->vm = vm;
    socket->plain_ref = plain_ref;
    socket->plain_ref_pinned = plain_ref != 0;
    socket->plain = plain;
    socket->auto_close = auto_close;
    if (!trust_config_copy(vm, &socket->trust, config)) {
        pthread_cond_destroy(&socket->operation_cond);
        pthread_mutex_destroy(&socket->operation_lock);
        pthread_mutex_destroy(&socket->lock);
        free(socket);
        return NULL;
    }
    socket->trust_call.vm = vm;
    socket->trust_call.config = &socket->trust;
    object = dvm_new_object(vm, dvm_find_class_or_external(vm, "javax/net/ssl/SSLSocket"));
    if (!object || !attach_payload(vm, object, socket, ssl_socket_drop)) {
        trust_config_clear(&socket->trust);
        pthread_cond_destroy(&socket->operation_cond);
        pthread_mutex_destroy(&socket->operation_lock);
        pthread_mutex_destroy(&socket->lock);
        free(socket);
        return NULL;
    }
    *object_out = object;
    return socket;
}

static bool set_socket_host(struct dvm *vm, struct ssl_socket *socket,
                            dvm_ref host_ref)
{
    const char *host;
    char *copy;
    if (!string_argument(vm, host_ref, &host, "TLS peer host")) return false;
    copy = strdup(host);
    if (!copy) {
        fail_message(vm, "java/lang/OutOfMemoryError", "TLS peer host");
        return false;
    }
    free(socket->host);
    socket->host = copy;
    socket->trust_call.host = socket->host;
    return true;
}

static bool create_unconnected_ssl(struct dvm *vm, const struct factory_state *factory,
                                   dvm_ref *out)
{
    dvm_ref plain_ref = 0;
    if (!make_plain_socket(vm, &plain_ref) || dvm_exception(vm)) return true;
    if (!new_ssl_socket(vm, &factory->config, plain_ref, NULL, true, out)) {
        dvm_unpin(vm, plain_ref);
        return true;
    }
    return true;
}

static struct factory_state *factory_payload(struct dvm *vm, dvm_ref ref)
{
    return object_payload(vm, ref, ssl_factory_drop);
}

static struct context_state *context_payload(struct dvm *vm, dvm_ref ref)
{
    return object_payload(vm, ref, ssl_context_drop);
}

static struct ssl_socket *socket_payload(struct dvm *vm, dvm_ref ref)
{
    return object_payload(vm, ref, ssl_socket_drop);
}

static struct ssl_stream *stream_payload(struct dvm *vm, dvm_ref ref)
{
    struct ssl_stream *stream = object_payload(vm, ref, ssl_stream_drop);
    return stream;
}

static struct ssl_session *session_payload(struct dvm *vm, dvm_ref ref)
{
    return object_payload(vm, ref, ssl_session_drop);
}

static bool make_stream(struct dvm *vm, struct ssl_socket *socket, bool input,
                        union dvm_value *out)
{
    struct ssl_stream *stream;
    dvm_ref object;
    pthread_mutex_lock(&socket->lock);
    bool closed = socket->closed;
    bool connected = socket->plain != NULL;
    pthread_mutex_unlock(&socket->lock);
    if (closed) return socket_error(vm, "Socket is closed");
    if (!connected) return socket_error(vm, "Socket is not connected");
    stream = calloc(1, sizeof(*stream));
    if (!stream) return fail_message(vm, "java/lang/OutOfMemoryError", "TLS stream");
    stream->socket = socket;
    stream->input = input;
    pthread_mutex_lock(&socket->lock);
    ++socket->refs;
    pthread_mutex_unlock(&socket->lock);
    object = dvm_new_object(vm, dvm_find_class_or_external(vm,
        input ? "javax/net/ssl/SSLSocketInputStream" : "javax/net/ssl/SSLSocketOutputStream"));
    if (!object || !attach_payload(vm, object, stream, ssl_stream_drop)) {
        ssl_socket_release(socket);
        free(stream);
        return fail_message(vm, "java/lang/OutOfMemoryError", "TLS stream object");
    }
    out->l = object;
    return true;
}

static bool make_session(struct dvm *vm, struct ssl_socket *socket,
                         bool valid, union dvm_value *out)
{
    struct ssl_session *session = calloc(1, sizeof(*session));
    dvm_ref object;
    if (!session) return fail_message(vm, "java/lang/OutOfMemoryError", "SSLSession");
    session->socket = socket;
    session->valid = valid;
    pthread_mutex_lock(&socket->lock);
    ++socket->refs;
    pthread_mutex_unlock(&socket->lock);
    object = dvm_new_object(vm, dvm_find_class_or_external(vm, "javax/net/ssl/SSLSession"));
    if (!object || !attach_payload(vm, object, session, ssl_session_drop)) {
        ssl_socket_release(socket);
        free(session);
        return fail_message(vm, "java/lang/OutOfMemoryError", "SSLSession object");
    }
    out->l = object;
    return true;
}

static bool make_session_context(struct dvm *vm, union dvm_value *out)
{
    struct ssl_session_context *context = calloc(1, sizeof(*context));
    dvm_ref object;
    if (!context) return fail_message(vm, "java/lang/OutOfMemoryError", "SSLSessionContext");
    context->vm = vm;
    object = dvm_new_object(vm, dvm_find_class_or_external(vm, "javax/net/ssl/SSLSessionContext"));
    if (!object || !attach_payload(vm, object, context, session_context_drop)) {
        free(context);
        return fail_message(vm, "java/lang/OutOfMemoryError", "SSLSessionContext object");
    }
    out->l = object;
    return true;
}

static bool protocol_list(struct dvm *vm, bool supported, union dvm_value *out)
{
    const char *const *names = NULL;
    size_t count = supported ? tls_client_supported_protocols(&names) :
                              tls_client_default_protocols(&names);
    out->l = string_array(vm, names, count);
    return out->l != 0;
}

static bool cipher_list(struct dvm *vm, bool supported, union dvm_value *out)
{
    const char *const *names = NULL;
    size_t count = supported ? tls_client_supported_cipher_suites(&names) :
                              tls_client_default_cipher_suites(&names);
    out->l = string_array(vm, names, count);
    return out->l != 0;
}

static bool ssl_socket_call(struct dvm *vm, struct ssl_socket *socket,
                            const char *method, const char *sig,
                            const union dvm_value *args, int nargs,
                            union dvm_value *out)
{
    if (is_call(method, sig, "<init>", "()V") && nargs == 0) return true;
    if (is_call(method, sig, "startHandshake", "()V") && nargs == 0)
        return ssl_socket_handshake(vm, socket);
    if (is_call(method, sig, "getInputStream", "()Ljava/io/InputStream;") && nargs == 0)
        return make_stream(vm, socket, true, out);
    if (is_call(method, sig, "getOutputStream", "()Ljava/io/OutputStream;") && nargs == 0)
        return make_stream(vm, socket, false, out);
    if (is_call(method, sig, "close", "()V") && nargs == 0) {
        ssl_socket_close(socket);
        return true;
    }
    if (is_call(method, sig, "isClosed", "()Z") && nargs == 0) {
        pthread_mutex_lock(&socket->lock); out->i = socket->closed; pthread_mutex_unlock(&socket->lock);
        return true;
    }
    if (is_call(method, sig, "isConnected", "()Z") && nargs == 0) {
        pthread_mutex_lock(&socket->lock); out->i = socket->plain != NULL; pthread_mutex_unlock(&socket->lock);
        return true;
    }
    if (is_call(method, sig, "isBound", "()Z") && nargs == 0) {
        pthread_mutex_lock(&socket->lock); out->i = socket->plain && socket_state_local_port(socket->plain) >= 0; pthread_mutex_unlock(&socket->lock);
        return true;
    }
    if (is_call(method, sig, "getInetAddress", "()Ljava/net/InetAddress;") && nargs == 0) {
        pthread_mutex_lock(&socket->lock);
        out->l = socket->plain ? socket_state_remote_address(vm, socket->plain) : 0;
        pthread_mutex_unlock(&socket->lock);
        return true;
    }
    if (is_call(method, sig, "getPort", "()I") && nargs == 0) {
        pthread_mutex_lock(&socket->lock); out->i = socket->plain ? socket_state_port(socket->plain) : 0; pthread_mutex_unlock(&socket->lock);
        return true;
    }
    if (is_call(method, sig, "getLocalPort", "()I") && nargs == 0) {
        pthread_mutex_lock(&socket->lock); out->i = socket->plain ? socket_state_local_port(socket->plain) : -1; pthread_mutex_unlock(&socket->lock);
        return true;
    }
    if (is_call(method, sig, "getSoTimeout", "()I") && nargs == 0) {
        pthread_mutex_lock(&socket->lock);
        if (socket->closed) { pthread_mutex_unlock(&socket->lock); return socket_error(vm, "Socket is closed"); }
        out->i = socket->plain ? socket_state_timeout(socket->plain) : 0;
        pthread_mutex_unlock(&socket->lock);
        return true;
    }
    if (is_call(method, sig, "setSoTimeout", "(I)V") && nargs == 1) {
        if (args[0].i < 0) return fail_message(vm, "java/lang/IllegalArgumentException", "negative timeout");
        pthread_mutex_lock(&socket->lock);
        if (socket->closed) { pthread_mutex_unlock(&socket->lock); return socket_error(vm, "Socket is closed"); }
        if (socket->plain) socket_state_set_timeout(socket->plain, args[0].i);
        pthread_mutex_unlock(&socket->lock);
        return true;
    }
    if ((is_call(method, sig, "connect", "(Ljava/net/SocketAddress;)V") && nargs == 1) ||
        (is_call(method, sig, "connect", "(Ljava/net/SocketAddress;I)V") && nargs == 2)) {
        int timeout = nargs == 2 ? args[1].i : 0;
        if (timeout < 0) return fail_message(vm, "java/lang/IllegalArgumentException", "negative connect timeout");
        return socket_connect_plain(vm, socket, args[0].l, timeout);
    }
    if (is_call(method, sig, "shutdownInput", "()V") && nargs == 0) {
        ssl_socket_close(socket);
        return true;
    }
    if (is_call(method, sig, "shutdownOutput", "()V") && nargs == 0) {
        ssl_socket_close(socket);
        return true;
    }
    if (is_call(method, sig, "getSession", "()Ljavax/net/ssl/SSLSession;") && nargs == 0) {
        if (!ssl_socket_handshake(vm, socket) || dvm_exception(vm)) return true;
        return make_session(vm, socket, true, out);
    }
    if (is_call(method, sig, "getSupportedProtocols", "()[Ljava/lang/String;") && nargs == 0)
        return protocol_list(vm, true, out);
    if (is_call(method, sig, "getEnabledProtocols", "()[Ljava/lang/String;") && nargs == 0) {
        pthread_mutex_lock(&socket->lock);
        if (socket->protocols) {
            size_t n = 0; while (socket->protocols[n]) ++n;
            out->l = string_array(vm, (const char *const *)socket->protocols, n);
        } else {
            const char *const *names = NULL; size_t n = tls_client_default_protocols(&names);
            out->l = string_array(vm, names, n);
        }
        pthread_mutex_unlock(&socket->lock);
        return out->l != 0;
    }
    if (is_call(method, sig, "setEnabledProtocols", "([Ljava/lang/String;)V") && nargs == 1) {
        char **names = NULL;
        if (!copy_name_array(vm, args[0].l, &names, "TLS protocols")) return true;
        pthread_mutex_lock(&socket->lock);
        if (socket->handshake || socket->closed) { pthread_mutex_unlock(&socket->lock); free_name_array(names); return socket_error(vm, "TLS socket is already started or closed"); }
        free_name_array(socket->protocols); socket->protocols = names;
        pthread_mutex_unlock(&socket->lock);
        return true;
    }
    if (is_call(method, sig, "getSupportedCipherSuites", "()[Ljava/lang/String;") && nargs == 0)
        return cipher_list(vm, true, out);
    if (is_call(method, sig, "getEnabledCipherSuites", "()[Ljava/lang/String;") && nargs == 0) {
        pthread_mutex_lock(&socket->lock);
        if (socket->ciphers) {
            size_t n = 0; while (socket->ciphers[n]) ++n;
            out->l = string_array(vm, (const char *const *)socket->ciphers, n);
        } else {
            const char *const *names = NULL; size_t n = tls_client_default_cipher_suites(&names);
            out->l = string_array(vm, names, n);
        }
        pthread_mutex_unlock(&socket->lock);
        return out->l != 0;
    }
    if (is_call(method, sig, "setEnabledCipherSuites", "([Ljava/lang/String;)V") && nargs == 1) {
        char **names = NULL;
        if (!copy_name_array(vm, args[0].l, &names, "TLS cipher suites")) return true;
        pthread_mutex_lock(&socket->lock);
        if (socket->handshake || socket->closed) { pthread_mutex_unlock(&socket->lock); free_name_array(names); return socket_error(vm, "TLS socket is already started or closed"); }
        free_name_array(socket->ciphers); socket->ciphers = names;
        pthread_mutex_unlock(&socket->lock);
        return true;
    }
    if (is_call(method, sig, "setUseClientMode", "(Z)V") && nargs == 1) {
        if (!args[0].i) return fail_message(vm, "java/net/SocketException", "TLS server mode is unsupported");
        return true;
    }
    if (is_call(method, sig, "getUseClientMode", "()Z") && nargs == 0) { out->i = 1; return true; }
    if (is_call(method, sig, "setNeedClientAuth", "(Z)V") && nargs == 1) {
        if (args[0].i) return fail_message(vm, "javax/net/ssl/SSLException", "client authentication keys are unsupported");
        socket->client_auth_requested = false; return true;
    }
    if (is_call(method, sig, "setWantClientAuth", "(Z)V") && nargs == 1) {
        if (args[0].i) return fail_message(vm, "javax/net/ssl/SSLException", "client authentication keys are unsupported");
        return true;
    }
    if (is_call(method, sig, "getNeedClientAuth", "()Z") && nargs == 0) { out->i = socket->client_auth_requested; return true; }
    if (is_call(method, sig, "getWantClientAuth", "()Z") && nargs == 0) { out->i = 0; return true; }
    if (is_call(method, sig, "setEnableSessionCreation", "(Z)V") && nargs == 1) {
        if (!args[0].i) return fail_message(vm, "javax/net/ssl/SSLException", "session creation cannot be disabled");
        return true;
    }
    if (is_call(method, sig, "getEnableSessionCreation", "()Z") && nargs == 0) { out->i = 1; return true; }
    return false;
}

static bool stream_read_call(struct dvm *vm, struct ssl_stream *stream,
                             const char *method, const char *sig,
                             const union dvm_value *args, int nargs,
                             union dvm_value *out)
{
    struct dvm_object *object;
    if (!stream->input) return false;
    if (is_call(method, sig, "read", "([BII)I") && nargs == 3)
        return ssl_read(vm, stream->socket, args[0].l, args[1].i, args[2].i, out);
    if (is_call(method, sig, "read", "([B)I") && nargs == 1) {
        object = dvm__obj(vm, args[0].l);
        if (!object) return fail_message(vm, "java/lang/NullPointerException", "byte array");
        return ssl_read(vm, stream->socket, args[0].l, 0, (int)object->length, out);
    }
    if (is_call(method, sig, "read", "()I") && nargs == 0) {
        dvm_ref one = dvm_new_array(vm, 'B', "B", 1);
        union dvm_value result = {0};
        if (!one) return fail_message(vm, "java/lang/OutOfMemoryError", "TLS byte read");
        if (!ssl_read(vm, stream->socket, one, 0, 1, &result) || dvm_exception(vm)) return true;
        out->i = result.i < 0 ? -1 : ((unsigned char *)dvm_array_data(vm, one))[0];
        dvm__discard_temporary(vm, one);
        return true;
    }
    if (is_call(method, sig, "available", "()I") && nargs == 0) {
        tls_client *client;
        struct socket_state *plain;
        size_t pending;
        if (!ssl_socket_handshake(vm, stream->socket) || dvm_exception(vm)) return true;
        if (!ssl_socket_begin(vm, stream->socket, &client, &plain)) return socket_error(vm, "Socket is closed");
        pending = tls_client_pending(client);
        ssl_socket_end(stream->socket, plain);
        out->i = pending > INT_MAX ? INT_MAX : (int)pending;
        return true;
    }
    if (is_call(method, sig, "skip", "(J)J") && nargs == 1) {
        int want = args[0].j <= 0 ? 0 : args[0].j > 8192 ? 8192 : (int)args[0].j;
        dvm_ref buffer;
        union dvm_value result = {0};
        if (!want) { out->j = 0; return true; }
        buffer = dvm_new_array(vm, 'B', "B", (uint32_t)want);
        if (!buffer) return fail_message(vm, "java/lang/OutOfMemoryError", "TLS skip buffer");
        if (!ssl_read(vm, stream->socket, buffer, 0, want, &result) || dvm_exception(vm)) return true;
        out->j = result.i < 0 ? 0 : result.i;
        dvm__discard_temporary(vm, buffer);
        return true;
    }
    if (is_call(method, sig, "markSupported", "()Z") && nargs == 0) { out->i = 0; return true; }
    if (is_call(method, sig, "close", "()V") && nargs == 0) { ssl_socket_close(stream->socket); return true; }
    return false;
}

static bool stream_write_call(struct dvm *vm, struct ssl_stream *stream,
                              const char *method, const char *sig,
                              const union dvm_value *args, int nargs,
                              union dvm_value *out)
{
    (void)out;
    if (stream->input) return false;
    if (is_call(method, sig, "write", "([BII)V") && nargs == 3)
        return ssl_write(vm, stream->socket, args[0].l, args[1].i, args[2].i);
    if (is_call(method, sig, "write", "([B)V") && nargs == 1) {
        struct dvm_object *object = dvm__obj(vm, args[0].l);
        if (!object) return fail_message(vm, "java/lang/NullPointerException", "byte array");
        return ssl_write(vm, stream->socket, args[0].l, 0, (int)object->length);
    }
    if (is_call(method, sig, "write", "(I)V") && nargs == 1) {
        dvm_ref one = dvm_new_array(vm, 'B', "B", 1);
        if (!one) return fail_message(vm, "java/lang/OutOfMemoryError", "TLS byte write");
        ((unsigned char *)dvm_array_data(vm, one))[0] = (unsigned char)args[0].i;
        if (!ssl_write(vm, stream->socket, one, 0, 1)) return true;
        dvm__discard_temporary(vm, one);
        return true;
    }
    if (is_call(method, sig, "flush", "()V") && nargs == 0) {
        pthread_mutex_lock(&stream->socket->lock);
        bool closed = stream->socket->closed;
        pthread_mutex_unlock(&stream->socket->lock);
        return closed ? socket_error(vm, "Socket is closed") : true;
    }
    if (is_call(method, sig, "close", "()V") && nargs == 0) { ssl_socket_close(stream->socket); return true; }
    return false;
}

static bool session_call(struct dvm *vm, struct ssl_session *session,
                         const char *method, const char *sig,
                         const union dvm_value *args, int nargs,
                         union dvm_value *out)
{
    (void)args;
    if (is_call(method, sig, "getProtocol", "()Ljava/lang/String;") && nargs == 0) {
        pthread_mutex_lock(&session->socket->lock);
        const char *name = session->socket->client ? tls_client_protocol(session->socket->client) : NULL;
        out->l = dvm_new_string(vm, name ? name : "NONE");
        pthread_mutex_unlock(&session->socket->lock);
        return true;
    }
    if (is_call(method, sig, "getCipherSuite", "()Ljava/lang/String;") && nargs == 0) {
        pthread_mutex_lock(&session->socket->lock);
        const char *name = session->socket->client ? tls_client_cipher_suite(session->socket->client) : NULL;
        out->l = dvm_new_string(vm, name ? name : "TLS_NULL_WITH_NULL_NULL");
        pthread_mutex_unlock(&session->socket->lock);
        return true;
    }
    if (is_call(method, sig, "getPeerHost", "()Ljava/lang/String;") && nargs == 0) {
        pthread_mutex_lock(&session->socket->lock);
        out->l = dvm_new_string(vm, session->socket->host ? session->socket->host : "");
        pthread_mutex_unlock(&session->socket->lock);
        return true;
    }
    if (is_call(method, sig, "getPeerPort", "()I") && nargs == 0) {
        pthread_mutex_lock(&session->socket->lock); out->i = session->socket->plain ? socket_state_port(session->socket->plain) : -1; pthread_mutex_unlock(&session->socket->lock);
        return true;
    }
    if (is_call(method, sig, "isValid", "()Z") && nargs == 0) { out->i = session->valid; return true; }
    if (is_call(method, sig, "invalidate", "()V") && nargs == 0) { session->valid = false; return true; }
    if (is_call(method, sig, "getPeerCertificates", "()[Ljava/security/cert/Certificate;") && nargs == 0)
        return peer_certificate_array(vm, session->socket, out);
    if (is_call(method, sig, "getPeerCertificates", "()[Ljavax/security/cert/Certificate;") && nargs == 0)
        return peer_certificate_array(vm, session->socket, out);
    if (is_call(method, sig, "getLocalCertificates", "()[Ljava/security/cert/Certificate;") && nargs == 0) { out->l = 0; return true; }
    if (is_call(method, sig, "getLocalCertificates", "()[Ljavax/security/cert/Certificate;") && nargs == 0) { out->l = 0; return true; }
    if (is_call(method, sig, "getId", "()[B") && nargs == 0) {
        out->l = dvm_new_array(vm, 'B', "B", 0);
        return out->l != 0;
    }
    if (is_call(method, sig, "getValue", "(Ljava/lang/String;)Ljava/lang/Object;") && nargs == 1) { out->l = 0; return true; }
    if (is_call(method, sig, "putValue", "(Ljava/lang/String;Ljava/lang/Object;)V") && nargs == 2) return true;
    if (is_call(method, sig, "removeValue", "(Ljava/lang/String;)V") && nargs == 1) return true;
    if (is_call(method, sig, "getValueNames", "()[Ljava/lang/String;") && nargs == 0) {
        out->l = dvm_new_array(vm, 'L', "Ljava/lang/String;", 0); return out->l != 0;
    }
    if (is_call(method, sig, "getSessionContext", "()Ljavax/net/ssl/SSLSessionContext;") && nargs == 0) { out->l = 0; return true; }
    return false;
}

static bool session_context_call(struct dvm *vm, struct ssl_session_context *context,
                                 const char *method, const char *sig,
                                 const union dvm_value *args, int nargs,
                                 union dvm_value *out)
{
    (void)context; (void)args;
    if (is_call(method, sig, "getSession", "([B)Ljavax/net/ssl/SSLSession;") && nargs == 1) { out->l = 0; return true; }
    if (is_call(method, sig, "getIds", "()Ljava/util/Enumeration;") && nargs == 0)
        return fail_message(vm, "java/lang/UnsupportedOperationException", "SSL session enumeration unsupported");
    if ((is_call(method, sig, "setSessionTimeout", "(I)V") && nargs == 1) ||
        (is_call(method, sig, "setSessionCacheSize", "(I)V") && nargs == 1)) {
        if (args[0].i < 0) return fail_message(vm, "java/lang/IllegalArgumentException", "negative session setting");
        return true;
    }
    if (is_call(method, sig, "getSessionTimeout", "()I") && nargs == 0) { out->i = 0; return true; }
    if (is_call(method, sig, "getSessionCacheSize", "()I") && nargs == 0) { out->i = 0; return true; }
    return false;
}

static bool trust_manager_call(struct dvm *vm, struct trust_manager_state *state,
                               const char *method, const char *sig,
                               const union dvm_value *args, int nargs,
                               union dvm_value *out)
{
    if (is_call(method, sig, "getAcceptedIssuers", "()[Ljava/security/cert/X509Certificate;") && nargs == 0) {
        out->l = dvm_new_array(vm, 'L', "Ljava/security/cert/X509Certificate;", 0);
        return out->l != 0;
    }
    if (is_call(method, sig, "checkServerTrusted", "([Ljava/security/cert/X509Certificate;Ljava/lang/String;Ljava/lang/String;)V") && nargs == 3) {
        struct dvm_object *array = dvm__obj(vm, args[0].l);
        const char *host;
        if (!array || array->kind != DVM_OBJ_ARRAY || !args[2].l ||
            !string_argument(vm, args[2].l, &host, "TLS peer host")) return true;
        uint32_t count = array->length;
        const unsigned char **ders = calloc(count ? count : 1, sizeof(*ders));
        size_t *lengths = calloc(count ? count : 1, sizeof(*lengths));
        if (!ders || !lengths) { free(ders); free(lengths); return fail_message(vm, "java/lang/OutOfMemoryError", "certificate chain"); }
        for (uint32_t i = 0; i < count; ++i) {
            dvm_ref cert = ((dvm_ref *)array->data)[i];
            ders[i] = cert_runtime_der(vm, cert, &lengths[i]);
            if (!ders[i]) { free(ders); free(lengths); return handshake_error(vm, "invalid certificate chain"); }
        }
        struct trust_call_context call = {vm, &state->config, host, 0};
        tls_cert_view *views = calloc(count ? count : 1, sizeof(*views));
        if (!views) { free(ders); free(lengths); return fail_message(vm, "java/lang/OutOfMemoryError", "certificate chain"); }
        for (uint32_t i = 0; i < count; ++i) { views[i].der = ders[i]; views[i].length = lengths[i]; }
        int accepted = trust_callback(&call, views, count, nargs == 3 ? dvm_string_utf8(vm, args[1].l) : "GENERIC");
        free(views); free(ders); free(lengths);
        if (accepted) return handshake_error(vm, call.reject_reason == 1 ? "hostname verification failed" : "certificate trust verification failed");
        return true;
    }
    if (is_call(method, sig, "checkServerTrusted", "([Ljava/security/cert/X509Certificate;Ljava/lang/String;)V") && nargs == 2)
        return fail_message(vm, "javax/net/ssl/CertificateException", "host is required for trust verification");
    return false;
}

static bool factory_call(struct dvm *vm, struct factory_state *factory,
                         const char *method, const char *sig,
                         const union dvm_value *args, int nargs,
                         union dvm_value *out)
{
    if (is_call(method, sig, "getDefaultCipherSuites", "()[Ljava/lang/String;") && nargs == 0)
        return cipher_list(vm, false, out);
    if (is_call(method, sig, "getSupportedCipherSuites", "()[Ljava/lang/String;") && nargs == 0)
        return cipher_list(vm, true, out);
    if (is_call(method, sig, "createSocket", "()Ljava/net/Socket;") && nargs == 0)
        return create_unconnected_ssl(vm, factory, &out->l);
    if (is_call(method, sig, "createSocket", "(Ljava/lang/String;I)Ljava/net/Socket;") && nargs == 2) {
        const char *host;
        if (!string_argument(vm, args[0].l, &host, "TLS peer host")) return true;
        if (args[1].i < 0 || args[1].i > 65535) return fail_message(vm, "java/lang/IllegalArgumentException", "port out of range");
        if (!create_unconnected_ssl(vm, factory, &out->l) || dvm_exception(vm)) return true;
        struct ssl_socket *socket = socket_payload(vm, out->l);
        if (!socket || !set_socket_host(vm, socket, args[0].l) || !socket_connect_host(vm, socket, host, args[1].i)) return true;
        return true;
    }
    if (is_call(method, sig, "createSocket", "(Ljava/net/InetAddress;I)Ljava/net/Socket;") && nargs == 2) {
        if (args[1].i < 0 || args[1].i > 65535) return fail_message(vm, "java/lang/IllegalArgumentException", "port out of range");
        if (!create_unconnected_ssl(vm, factory, &out->l) || dvm_exception(vm)) return true;
        struct ssl_socket *socket = socket_payload(vm, out->l);
        if (!socket || !socket_connect_address(vm, socket, args[0].l, args[1].i)) return true;
        return true;
    }
    if (is_call(method, sig, "createSocket", "(Ljava/net/Socket;Ljava/lang/String;IZ)Ljava/net/Socket;") && nargs == 4) {
        struct socket_state *plain;
        const char *host;
        if (!string_argument(vm, args[1].l, &host, "TLS peer host")) return true;
        if (args[2].i < 0 || args[2].i > 65535) return fail_message(vm, "java/lang/IllegalArgumentException", "port out of range");
        plain = socket_runtime_retain(vm, args[0].l);
        if (!plain) return socket_error(vm, "plain Socket is not connected");
        if (!new_ssl_socket(vm, &factory->config, 0, plain, args[3].i != 0, &out->l)) {
            socket_state_release(plain); return true;
        }
        struct ssl_socket *socket = socket_payload(vm, out->l);
        if (!set_socket_host(vm, socket, args[1].l)) return true;
        return true;
    }
    if (is_call(method, sig, "createSocket", "(Ljava/lang/String;ILjava/net/InetAddress;I)Ljava/net/Socket;") && nargs == 4)
        return fail_message(vm, "java/net/SocketException", "local bind is unsupported by the socket runtime");
    if (is_call(method, sig, "createSocket", "(Ljava/net/InetAddress;ILjava/net/InetAddress;I)Ljava/net/Socket;") && nargs == 4)
        return fail_message(vm, "java/net/SocketException", "local bind is unsupported by the socket runtime");
    return false;
}

static bool context_call(struct dvm *vm, struct context_state *context,
                         const char *method, const char *sig,
                         const union dvm_value *args, int nargs,
                         union dvm_value *out)
{
    if (is_call(method, sig, "init", "([Ljavax/net/ssl/KeyManager;[Ljavax/net/ssl/TrustManager;Ljava/security/SecureRandom;)V") && nargs == 3) {
        struct trust_config replacement;
        dvm_ref managers = args[1].l;
        if (args[0].l && dvm_array_length(vm, args[0].l))
            return fail_message(vm, "java/security/KeyManagementException", "client key managers are unsupported");
        if (!managers) trust_config_init_system(&replacement, vm);
        else {
            struct dvm_object *array = dvm__obj(vm, managers);
            if (!array || array->kind != DVM_OBJ_ARRAY || !array->length)
                return fail_message(vm, "java/security/KeyManagementException", "no trust manager supplied");
            dvm_ref chosen = 0;
            dvm_ref *items = array->data;
            for (uint32_t i = 0; i < array->length; ++i) {
                if (items[i] && class_assignable(vm, items[i], "javax/net/ssl/X509TrustManager")) { chosen = items[i]; break; }
            }
            if (!chosen || !trust_config_from_manager(vm, chosen, &replacement)) return true;
        }
        trust_config_clear(&context->config);
        context->config = replacement;
        context->initialized = true;
        return true;
    }
    if (is_call(method, sig, "getProtocol", "()Ljava/lang/String;") && nargs == 0) { out->l = dvm_new_string(vm, context->protocol ? context->protocol : "TLS"); return true; }
    if (is_call(method, sig, "getSocketFactory", "()Ljavax/net/ssl/SSLSocketFactory;") && nargs == 0) {
        struct factory_state *factory;
        dvm_ref object;
        if (!context->initialized) return fail_message(vm, "java/lang/IllegalStateException", "SSLContext is not initialized");
        factory = calloc(1, sizeof(*factory));
        if (!factory || !trust_config_copy(vm, &factory->config, &context->config)) { free(factory); return true; }
        object = dvm_new_object(vm, dvm_find_class_or_external(vm, "javax/net/ssl/SSLSocketFactory"));
        if (!object || !attach_payload(vm, object, factory, ssl_factory_drop)) { ssl_factory_drop(factory); return fail_message(vm, "java/lang/OutOfMemoryError", "SSLSocketFactory"); }
        out->l = object;
        return true;
    }
    if (is_call(method, sig, "getClientSessionContext", "()Ljavax/net/ssl/SSLSessionContext;") && nargs == 0)
        return make_session_context(vm, out);
    if (is_call(method, sig, "getServerSessionContext", "()Ljavax/net/ssl/SSLSessionContext;") && nargs == 0)
        return make_session_context(vm, out);
    return false;
}

static bool trust_factory_call(struct dvm *vm, dvm_ref self,
                               const char *method, const char *sig,
                               const union dvm_value *args, int nargs,
                               union dvm_value *out)
{
    struct factory_state *state = object_payload(vm, self, ssl_factory_drop);
    (void)state;
    if (is_call(method, sig, "getDefaultAlgorithm", "()Ljava/lang/String;") && nargs == 0) {
        out->l = dvm_new_string(vm, "Default"); return true;
    }
    if (is_call(method, sig, "getAlgorithm", "()Ljava/lang/String;") && nargs == 0) {
        out->l = dvm_new_string(vm, "Default"); return true;
    }
    return false;
}

static bool ssl_context_static_call(struct dvm *vm, const char *method,
                                    const char *sig, const union dvm_value *args,
                                    int nargs, union dvm_value *out)
{
    if (is_call(method, sig, "getInstance", "(Ljava/lang/String;)Ljavax/net/ssl/SSLContext;") && nargs == 1) {
        const char *algorithm;
        struct context_state *state;
        dvm_ref object;
        if (!string_argument(vm, args[0].l, &algorithm, "SSLContext algorithm")) return true;
        if (strcmp(algorithm, "TLS") && strcmp(algorithm, "TLSv1.2") && strcmp(algorithm, "TLSv1.3"))
            return failf(vm, "java/security/NoSuchAlgorithmException", "unsupported SSLContext algorithm %s", algorithm);
        state = calloc(1, sizeof(*state));
        if (!state) return fail_message(vm, "java/lang/OutOfMemoryError", "SSLContext");
        state->protocol = strdup(algorithm);
        trust_config_init_system(&state->config, vm);
        object = dvm_new_object(vm, dvm_find_class_or_external(vm, "javax/net/ssl/SSLContext"));
        if (!object || !attach_payload(vm, object, state, ssl_context_drop)) { ssl_context_drop(state); return fail_message(vm, "java/lang/OutOfMemoryError", "SSLContext"); }
        out->l = object;
        return true;
    }
    if (is_call(method, sig, "getInstance", "(Ljava/lang/String;Ljava/lang/String;)Ljavax/net/ssl/SSLContext;") && nargs == 2) {
        if (!args[1].l) return fail_message(vm, "java/lang/NullPointerException", "provider");
        return fail_message(vm, "java/security/NoSuchProviderException", "provider is unsupported");
    }
    if (is_call(method, sig, "getDefault", "()Ljavax/net/ssl/SSLContext;") && nargs == 0) {
        if (!default_context_ref) {
            union dvm_value create = {0};
            if (!ssl_context_static_call(vm, "getInstance", "(Ljava/lang/String;)Ljavax/net/ssl/SSLContext;",
                                         (union dvm_value[]){{.l=dvm_new_string(vm, "TLS")}}, 1, &create)) return true;
            struct context_state *state = context_payload(vm, create.l);
            if (!state) return ssl_error(vm, "default SSLContext payload");
            state->initialized = true;
            default_context_ref = create.l;
            dvm_pin(vm, default_context_ref);
        }
        out->l = default_context_ref;
        return true;
    }
    if (is_call(method, sig, "setDefault", "(Ljavax/net/ssl/SSLContext;)V") && nargs == 1) {
        struct context_state *state = context_payload(vm, args[0].l);
        if (!state || !state->initialized) return fail_message(vm, "java/lang/IllegalArgumentException", "invalid SSLContext");
        if (default_context_ref) dvm_unpin(vm, default_context_ref);
        default_context_ref = args[0].l;
        dvm_pin(vm, default_context_ref);
        return true;
    }
    return false;
}

static bool factory_static_call(struct dvm *vm, const char *method,
                                const char *sig, const union dvm_value *args,
                                int nargs, union dvm_value *out)
{
    (void)args;
    if (is_call(method, sig, "getDefault", "()Ljavax/net/ssl/SSLSocketFactory;") && nargs == 0) {
        if (!default_factory_ref) {
            struct factory_state *state = calloc(1, sizeof(*state));
            dvm_ref object;
            if (!state || !trust_config_copy(vm, &state->config, &(struct trust_config){.system = true, .vm = vm})) {
                free(state); return fail_message(vm, "java/lang/OutOfMemoryError", "default SSLSocketFactory");
            }
            object = dvm_new_object(vm, dvm_find_class_or_external(vm, "javax/net/ssl/SSLSocketFactory"));
            if (!object || !attach_payload(vm, object, state, ssl_factory_drop)) { ssl_factory_drop(state); return fail_message(vm, "java/lang/OutOfMemoryError", "default SSLSocketFactory"); }
            default_factory_ref = object;
            dvm_pin(vm, default_factory_ref);
        }
        out->l = default_factory_ref;
        return true;
    }
    return false;
}

static dvm_ref new_default_trust_manager(struct dvm *vm, const struct trust_config *config)
{
    struct trust_manager_state *state = calloc(1, sizeof(*state));
    dvm_ref object;
    if (!state || !trust_config_copy(vm, &state->config, config)) { free(state); return 0; }
    object = dvm_new_object(vm, dvm_find_class_or_external(vm, "javax/net/ssl/DefaultX509TrustManager"));
    if (!object || !attach_payload(vm, object, state, trust_manager_drop)) { trust_manager_drop(state); return 0; }
    return object;
}

static bool trust_manager_factory_static(struct dvm *vm, const char *method,
                                         const char *sig, const union dvm_value *args,
                                         int nargs, union dvm_value *out)
{
    if (is_call(method, sig, "getDefaultAlgorithm", "()Ljava/lang/String;") && nargs == 0) { out->l = dvm_new_string(vm, "Default"); return true; }
    if (is_call(method, sig, "getInstance", "(Ljava/lang/String;)Ljavax/net/ssl/TrustManagerFactory;") && nargs == 1) {
        const char *algorithm;
        struct factory_state *state;
        dvm_ref object;
        if (!string_argument(vm, args[0].l, &algorithm, "TrustManagerFactory algorithm")) return true;
        if (strcmp(algorithm, "Default")) return failf(vm, "java/security/NoSuchAlgorithmException", "unsupported TrustManagerFactory algorithm %s", algorithm);
        state = calloc(1, sizeof(*state));
        if (!state) return fail_message(vm, "java/lang/OutOfMemoryError", "TrustManagerFactory");
        trust_config_init_system(&state->config, vm);
        object = dvm_new_object(vm, dvm_find_class_or_external(vm, "javax/net/ssl/TrustManagerFactory"));
        if (!object || !attach_payload(vm, object, state, ssl_factory_drop)) { ssl_factory_drop(state); return fail_message(vm, "java/lang/OutOfMemoryError", "TrustManagerFactory"); }
        out->l = object;
        return true;
    }
    return false;
}

static bool trust_manager_factory_instance(struct dvm *vm, struct factory_state *state,
                                           const char *method, const char *sig,
                                           const union dvm_value *args, int nargs,
                                           union dvm_value *out)
{
    if (is_call(method, sig, "init", "(Ljava/security/KeyStore;)V") && nargs == 1) {
        struct trust_config replacement;
        if (!trust_config_from_keystore(vm, args[0].l, &replacement)) return true;
        trust_config_clear(&state->config);
        state->config = replacement;
        return true;
    }
    if (is_call(method, sig, "init", "(Ljavax/net/ssl/ManagerFactoryParameters;)V") && nargs == 1)
        return fail_message(vm, "java/security/InvalidAlgorithmParameterException", "ManagerFactoryParameters unsupported");
    if (is_call(method, sig, "getTrustManagers", "()[Ljavax/net/ssl/TrustManager;") && nargs == 0) {
        dvm_ref manager = new_default_trust_manager(vm, &state->config);
        dvm_ref array;
        if (!manager) return true;
        array = dvm_new_array(vm, 'L', "Ljavax/net/ssl/TrustManager;", 1);
        if (!array) return fail_message(vm, "java/lang/OutOfMemoryError", "TrustManager array");
        ((dvm_ref *)dvm_array_data(vm, array))[0] = manager;
        out->l = array;
        return true;
    }
    if (is_call(method, sig, "getAlgorithm", "()Ljava/lang/String;") && nargs == 0) { out->l = dvm_new_string(vm, "Default"); return true; }
    return false;
}

static void declare_class(struct dvm *vm, const char *name, const char *super,
                          const char *const *interfaces, int interface_count,
                          bool is_interface)
{
    struct dvm_class *klass = dvm_find_class_or_external(vm, name);
    struct dvm_class *parent = super ? dvm_find_class_or_external(vm, super) : NULL;
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

void ssl_runtime_vm_start(struct dvm *vm)
{
    static const char *const closeable[] = {"java/io/Closeable", "java/lang/AutoCloseable"};
    static const char *const trust[] = {"javax/net/ssl/TrustManager"};
    static const char *const x509[] = {"javax/net/ssl/X509TrustManager"};
    active_vm = vm;
    default_context_ref = 0;
    default_factory_ref = 0;
    exception_hierarchy_vm_start(vm);
    declare_class(vm, "javax/net/ssl/TrustManager", "java/lang/Object", NULL, 0, true);
    declare_class(vm, "javax/net/ssl/X509TrustManager", "java/lang/Object", trust, 1, true);
    declare_class(vm, "javax/net/ssl/TrustManagerFactorySpi", "java/lang/Object", NULL, 0, false);
    declare_class(vm, "javax/net/ssl/TrustManagerFactory", "java/lang/Object", NULL, 0, false);
    declare_class(vm, "javax/net/ssl/SSLContextSpi", "java/lang/Object", NULL, 0, false);
    declare_class(vm, "javax/net/ssl/SSLContext", "java/lang/Object", NULL, 0, false);
    declare_class(vm, "javax/net/ssl/SSLSocketFactory", "javax/net/SocketFactory", NULL, 0, false);
    declare_class(vm, "javax/net/ssl/SSLSocket", "java/net/Socket", closeable, 2, false);
    declare_class(vm, "javax/net/ssl/SSLSession", "java/lang/Object", NULL, 0, true);
    declare_class(vm, "javax/net/ssl/SSLSessionContext", "java/lang/Object", NULL, 0, true);
    declare_class(vm, "javax/net/ssl/DefaultX509TrustManager", "java/lang/Object", x509, 1, false);
    declare_class(vm, "javax/net/ssl/SSLSocketInputStream", "java/io/InputStream", closeable, 2, false);
    declare_class(vm, "javax/net/ssl/SSLSocketOutputStream", "java/io/OutputStream", closeable, 2, false);
}

void ssl_runtime_cleanup(void)
{
    if (active_vm) {
        if (default_factory_ref) dvm_unpin(active_vm, default_factory_ref);
        if (default_context_ref) dvm_unpin(active_vm, default_context_ref);
    }
    default_factory_ref = 0;
    default_context_ref = 0;
    active_vm = NULL;
}

bool ssl_runtime_call(struct dvm *vm, const char *class_name, const char *method,
                      const char *sig, dvm_ref self, const union dvm_value *args,
                      int nargs, union dvm_value *out)
{
    struct ssl_stream *stream;
    struct ssl_socket *socket;
    struct ssl_session *session;
    struct ssl_session_context *session_context;
    struct trust_manager_state *trust_manager;
    struct context_state *context;
    struct factory_state *factory;
    if (vm != active_vm) return false;
    if (self) {
        if ((stream = stream_payload(vm, self)))
            return stream->input ? stream_read_call(vm, stream, method, sig, args, nargs, out) :
                                   stream_write_call(vm, stream, method, sig, args, nargs, out);
        if ((socket = socket_payload(vm, self)))
            return ssl_socket_call(vm, socket, method, sig, args, nargs, out);
        if ((session = session_payload(vm, self)))
            return session_call(vm, session, method, sig, args, nargs, out);
        if ((session_context = object_payload(vm, self, session_context_drop)))
            return session_context_call(vm, session_context, method, sig, args, nargs, out);
        if ((trust_manager = trust_manager_payload(vm, self)))
            return trust_manager_call(vm, trust_manager, method, sig, args, nargs, out);
        if ((context = context_payload(vm, self)))
            return context_call(vm, context, method, sig, args, nargs, out);
        if ((factory = factory_payload(vm, self))) {
            if (!strcmp(class_name, "javax/net/ssl/TrustManagerFactory"))
                return trust_manager_factory_instance(vm, factory, method, sig, args, nargs, out);
            return factory_call(vm, factory, method, sig, args, nargs, out);
        }
    }
    if (!strcmp(class_name, "javax/net/ssl/TrustManagerFactory"))
        return trust_manager_factory_static(vm, method, sig, args, nargs, out);
    if (!strcmp(class_name, "javax/net/ssl/SSLContext"))
        return ssl_context_static_call(vm, method, sig, args, nargs, out);
    if (!strcmp(class_name, "javax/net/ssl/SSLSocketFactory"))
        return factory_static_call(vm, method, sig, args, nargs, out);
    if (!strcmp(class_name, "javax/net/ssl/SSLContext")) return false;
    return false;
}

bool ssl_runtime_selftest(struct dvm *vm)
{
    const char *const *protocols = NULL;
    const char *const *ciphers = NULL;
    size_t protocol_count;
    size_t cipher_count;
    bool classes;
    bool hostname_cases = certificate_hostname_match(fixture_valid, sizeof fixture_valid, "local.test") &&
        certificate_hostname_match(fixture_valid, sizeof fixture_valid, "LOCAL.TEST") &&
        !certificate_hostname_match(fixture_valid, sizeof fixture_valid, "wrong.test") &&
        !certificate_hostname_match(fixture_wrong_host, sizeof fixture_wrong_host, "local.test") &&
        !certificate_hostname_match(fixture_valid, sizeof fixture_valid - 1, "local.test") &&
        !certificate_hostname_match(fixture_valid, sizeof fixture_valid, "127.0.0.1");
    if (vm != active_vm) return false;
    protocol_count = tls_client_supported_protocols(&protocols);
    cipher_count = tls_client_supported_cipher_suites(&ciphers);
    classes = dvm_find_class_or_external(vm, "javax/net/ssl/SSLContext") != NULL &&
              dvm_find_class_or_external(vm, "javax/net/ssl/SSLSocket") != NULL &&
              dvm_find_class_or_external(vm, "javax/net/ssl/X509TrustManager") != NULL;
    sdk_trace_fprintf(stderr, "SSL_RUNTIME_SELFTEST %s classes=%d protocols=%zu ciphers=%zu trust_callback=required hostname_cases=%d client_keys=fail_closed\n",
            classes && protocol_count && cipher_count && hostname_cases ? "PASS" : "FAIL",
            classes, protocol_count, cipher_count, hostname_cases);
    return classes && protocol_count && cipher_count && hostname_cases;
}
