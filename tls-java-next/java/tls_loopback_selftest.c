/*
 * Controlled Java TLS loopback selftest.
 *
 * This is the first real Java-level TLS handshake check: a guest
 * SSLContext/TrustManagerFactory/SSLSocketFactory/SSLSocket chain connects to a
 * loopback port that the host forwards to a local test TLS server.  The trust
 * anchors come from a loaded guest KeyStore holding a test-only CA; the server
 * certificate names "local.test", which the platform chain verifier must match
 * through the real hostname check.  Nothing here fabricates a success: every
 * step is a real API call against the DVM runtimes and the real native client.
 *
 * A second context loaded with an unrelated CA must fail the same handshake.
 * That negative case proves the trust decision actually runs instead of
 * accepting every peer.
 *
 * The whole test is compiled in only when LUNARIA_TLS_LOOPBACK_PORT is defined;
 * normal candidates print SKIP so the default self-test surface is unchanged.
 * The TCP connection itself still uses 127.0.0.1, so the existing loopback
 * network policy is not relaxed.
 */
#include "tls_loopback_selftest.h"
#include "ssl_runtime.h"
#include "cert_runtime.h"
#include "socket_runtime.h"
#include "tls_loopback_fixtures.h"
#include "trust.h"
#include "fixture_der.h"
#include <stdio.h>
#include <string.h>

extern int sdk_trace_fprintf(FILE *, const char *, ...);
extern void dvm_describe_exception(struct dvm *vm, dvm_ref exc, char *buf, size_t sz);

#define TLS_LB_CONNECT_HOST "127.0.0.1" /* loopback only; policy unchanged */
#define TLS_LB_SERVER_NAME "local.test" /* certificate DNS SAN */
#define TLS_LB_BANNER "LUNARIA_TLS_BANNER\n"
#define TLS_LB_REQUEST "LUNARIA_LOOPBACK_REQUEST\n"
#define TLS_LB_ECHO_PREFIX "ECHO:"

#ifdef LUNARIA_TLS_LOOPBACK_PORT

static const char *tls_lb_last_check = "start";

static bool tls_lb_ssl_call(struct dvm *vm, const char *cls, const char *method,
                            const char *sig, dvm_ref self,
                            const union dvm_value *args, int nargs,
                            union dvm_value *out)
{
    union dvm_value local = {0};
    if (!out) out = &local;
    return ssl_runtime_call(vm, cls, method, sig, self, args, nargs, out);
}

static bool tls_lb_cert_call(struct dvm *vm, const char *cls, const char *method,
                             const char *sig, dvm_ref self,
                             const union dvm_value *args, int nargs,
                             union dvm_value *out)
{
    union dvm_value local = {0};
    if (!out) out = &local;
    return cert_runtime_call(vm, cls, method, sig, self, args, nargs, out);
}

static bool tls_lb_socket_call(struct dvm *vm, const char *cls, const char *method,
                               const char *sig, dvm_ref self,
                               const union dvm_value *args, int nargs,
                               union dvm_value *out)
{
    union dvm_value local = {0};
    if (!out) out = &local;
    return socket_runtime_call(vm, cls, method, sig, self, args, nargs, out);
}

/* Build one initialized SSLContext whose only trust anchors are the given CA
 * DER.  Returns 0 with a pending exception on failure.  Step names are kept in
 * tls_lb_last_check for the failure line. */
static dvm_ref tls_lb_context_from_ca(struct dvm *vm,
                                      const unsigned char *ca_der, size_t ca_len)
{
    dvm_ref certificate = 0, store = 0, tmf = 0, context = 0, managers = 0;
    union dvm_value args[3] = {{0}}, out = {0};

    tls_lb_last_check = "trust_store_certificate";
    certificate = cert_runtime_new_x509(vm, ca_der, ca_len);
    if (!certificate || dvm_exception(vm)) return 0;

    tls_lb_last_check = "trust_store_get_instance";
    args[0].l = dvm_new_string(vm, "PKCS12");
    out.l = 0;
    if (!args[0].l ||
        !tls_lb_cert_call(vm, "java/security/KeyStore", "getInstance",
                          "(Ljava/lang/String;)Ljava/security/KeyStore;",
                          0, args, 1, &out) || dvm_exception(vm) || !out.l)
        return 0;
    store = out.l;

    tls_lb_last_check = "trust_store_load";
    memset(args, 0, sizeof(args));
    out.l = 0;
    if (!tls_lb_cert_call(vm, "java/security/KeyStore", "load",
                          "(Ljava/io/InputStream;[C)V", store, args, 2, &out) ||
        dvm_exception(vm))
        return 0;

    tls_lb_last_check = "trust_store_set_ca";
    args[0].l = dvm_new_string(vm, "loopback-test-ca");
    args[1].l = certificate;
    out.l = 0;
    if (!args[0].l ||
        !tls_lb_cert_call(vm, "java/security/KeyStore", "setCertificateEntry",
                          "(Ljava/lang/String;Ljava/security/cert/Certificate;)V",
                          store, args, 2, &out) || dvm_exception(vm))
        return 0;
    {
        union dvm_value probe = {0};
        if (tls_lb_cert_call(vm, "java/security/KeyStore", "size", "()I", store,
                             NULL, 0, &probe) && !dvm_exception(vm))
            sdk_trace_fprintf(stderr, "TLS_LOOPBACK_KEYSTORE size=%d\n", probe.i);
        if (dvm_exception(vm)) dvm_clear_exception(vm);
        probe.i = 0;
        if (tls_lb_cert_call(vm, "java/security/KeyStore", "containsAlias",
                             "(Ljava/lang/String;)Z", store, args, 1, &probe) &&
            !dvm_exception(vm))
            sdk_trace_fprintf(stderr, "TLS_LOOPBACK_KEYSTORE contains_loopback_ca=%d\n",
                              probe.i);
        if (dvm_exception(vm)) dvm_clear_exception(vm);
    }

    tls_lb_last_check = "tmf_get_instance";
    memset(args, 0, sizeof(args));
    out.l = 0;
    args[0].l = dvm_new_string(vm, "Default");
    if (!args[0].l ||
        !tls_lb_ssl_call(vm, "javax/net/ssl/TrustManagerFactory", "getInstance",
                         "(Ljava/lang/String;)Ljavax/net/ssl/TrustManagerFactory;",
                         0, args, 1, &out) || dvm_exception(vm) || !out.l)
        return 0;
    tmf = out.l;

    tls_lb_last_check = "tmf_init";
    out.l = 0;
    memset(args, 0, sizeof(args));
    args[0].l = store;
    if (!tls_lb_ssl_call(vm, "javax/net/ssl/TrustManagerFactory", "init",
                         "(Ljava/security/KeyStore;)V", tmf, args, 1, &out) ||
        dvm_exception(vm))
        return 0;

    tls_lb_last_check = "tmf_get_trust_managers";
    out.l = 0;
    if (!tls_lb_ssl_call(vm, "javax/net/ssl/TrustManagerFactory", "getTrustManagers",
                         "()[Ljavax/net/ssl/TrustManager;", tmf, NULL, 0, &out) ||
        dvm_exception(vm) || !out.l)
        return 0;
    managers = out.l;

    tls_lb_last_check = "context_get_instance";
    memset(args, 0, sizeof(args));
    out.l = 0;
    args[0].l = dvm_new_string(vm, "TLS");
    if (!args[0].l ||
        !tls_lb_ssl_call(vm, "javax/net/ssl/SSLContext", "getInstance",
                         "(Ljava/lang/String;)Ljavax/net/ssl/SSLContext;",
                         0, args, 1, &out) || dvm_exception(vm) || !out.l)
        return 0;
    context = out.l;

    tls_lb_last_check = "context_init";
    memset(args, 0, sizeof(args));
    out.l = 0;
    args[0].l = 0; /* no client key managers */
    args[1].l = managers;
    args[2].l = 0;
    if (!tls_lb_ssl_call(vm, "javax/net/ssl/SSLContext", "init",
                         "([Ljavax/net/ssl/KeyManager;[Ljavax/net/ssl/TrustManager;Ljava/security/SecureRandom;)V",
                         context, args, 3, &out) || dvm_exception(vm))
        return 0;
    return context;
}

/* Connect a plain loopback TCP socket; TLS then wraps it with the server name
 * taken from the certificate, so hostname verification exercises the DNS SAN
 * path while the transport stays on loopback. */
static dvm_ref tls_lb_plain_socket(struct dvm *vm, int port)
{
    dvm_ref socket;
    union dvm_value args[2] = {{0}}, out = {0};
    socket = dvm_new_object(vm, dvm_find_class_or_external(vm, "java/net/Socket"));
    if (!socket) return 0;
    args[0].l = dvm_new_string(vm, TLS_LB_CONNECT_HOST);
    args[1].i = port;
    if (!args[0].l ||
        !tls_lb_socket_call(vm, "java/net/Socket", "<init>",
                            "(Ljava/lang/String;I)V", socket, args, 2, &out) ||
        dvm_exception(vm))
        return 0;
    args[0].i = 5000;
    if (!tls_lb_socket_call(vm, "java/net/Socket", "setSoTimeout", "(I)V",
                            socket, args, 1, &out) || dvm_exception(vm))
        return 0;
    return socket;
}

static dvm_ref tls_lb_ssl_socket(struct dvm *vm, dvm_ref context,
                                 dvm_ref plain_socket, int port,
                                 const char *server_name)
{
    dvm_ref factory, ssl;
    union dvm_value args[4] = {{0}}, out = {0};
    tls_lb_last_check = "factory_get";
    out.l = 0;
    if (!tls_lb_ssl_call(vm, "javax/net/ssl/SSLContext", "getSocketFactory",
                         "()Ljavax/net/ssl/SSLSocketFactory;", context, NULL, 0,
                         &out) || dvm_exception(vm) || !out.l)
        return 0;
    factory = out.l;
    tls_lb_last_check = "ssl_socket_create";
    out.l = 0;
    memset(args, 0, sizeof(args));
    args[0].l = plain_socket;
    args[1].l = dvm_new_string(vm, server_name);
    args[2].i = port;
    args[3].i = 1; /* autoClose the plain socket */
    if (!args[1].l ||
        !tls_lb_ssl_call(vm, "javax/net/ssl/SSLSocketFactory", "createSocket",
                         "(Ljava/net/Socket;Ljava/lang/String;IZ)Ljava/net/Socket;",
                         factory, args, 4, &out) || dvm_exception(vm) || !out.l)
        return 0;
    ssl = out.l;
    return ssl;
}

/* Read exactly want bytes from the TLS input stream.  Returns the byte count
 * actually collected, or -1 with a pending exception. */
static int tls_lb_read_exact(struct dvm *vm, dvm_ref stream, unsigned char *into,
                             size_t want)
{
    size_t got = 0;
    dvm_ref buffer = dvm_new_array(vm, 'B', "B", (uint32_t)want);
    if (!buffer) return -1;
    while (got < want) {
        union dvm_value args[3] = {{0}}, out = {0};
        size_t remaining = want - got;
        args[0].l = buffer;
        args[1].i = 0;
        args[2].i = (int)remaining;
        if (!tls_lb_ssl_call(vm, "javax/net/ssl/SSLSocketInputStream", "read",
                             "([BII)I", stream, args, 3, &out)) {
            if (!dvm_exception(vm))
                dvm__throw(vm, "java/io/IOException", "TLS read returned no handler");
            goto fail;
        }
        if (dvm_exception(vm)) goto fail;
        if (out.i <= 0) {
            dvm__throw(vm, "java/io/IOException", "TLS stream closed before payload");
            goto fail;
        }
        if ((size_t)out.i > remaining) {
            dvm__throw(vm, "java/io/IOException", "TLS read overflow");
            goto fail;
        }
        memcpy(into + got, dvm_array_data(vm, buffer), (size_t)out.i);
        got += (size_t)out.i;
    }
    dvm__discard_temporary(vm, buffer);
    return (int)got;
fail:
    dvm__discard_temporary(vm, buffer);
    return -1;
}

static bool tls_lb_write_all(struct dvm *vm, dvm_ref stream, const unsigned char *from,
                             size_t length)
{
    union dvm_value args[1] = {{0}}, out = {0};
    dvm_ref buffer = dvm_new_array(vm, 'B', "B", (uint32_t)length);
    if (!buffer) return false;
    memcpy(dvm_array_data(vm, buffer), from, length);
    args[0].l = buffer;
    if (!tls_lb_ssl_call(vm, "javax/net/ssl/SSLSocketOutputStream", "write",
                         "([B)V", stream, args, 1, &out) || dvm_exception(vm)) {
        dvm__discard_temporary(vm, buffer);
        return false;
    }
    if (!tls_lb_ssl_call(vm, "javax/net/ssl/SSLSocketOutputStream", "flush",
                         "()V", stream, NULL, 0, &out) || dvm_exception(vm)) {
        dvm__discard_temporary(vm, buffer);
        return false;
    }
    dvm__discard_temporary(vm, buffer);
    return true;
}

/* Direct platform trust diagnosis: run the same NetStack verifier that the
 * handshake callback uses, for the loopback server certificate and for the
 * preserved fixture pair.  A passing fixture line proves the platform path is
 * live; a non-zero loopback code attributes the rejection to the test chain. */
static void tls_lb_platform_probe(void)
{
    TlsTrustCert peer, ca;
    TlsTrustChain chain = {0};
    int32_t code;
    static const struct {
        const char *name;
        const unsigned char *peer;
        size_t peer_len;
    } cases[] = {
        {"loopback_server", lunaria_loopback_server_der, sizeof lunaria_loopback_server_der},
        {"fixture_valid", fixture_valid, sizeof fixture_valid},
    };
    sdk_trace_fprintf(stderr, "TLS_LOOPBACK_TRUST available=%d\n", tls_trust_available());
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        peer.encoding = TLS_TRUST_DER;
        peer.data = cases[i].peer;
        peer.size = cases[i].peer_len;
        if (!strcmp(cases[i].name, "loopback_server")) {
            ca.encoding = TLS_TRUST_DER;
            ca.data = lunaria_loopback_test_ca_der;
            ca.size = sizeof lunaria_loopback_test_ca_der;
        } else {
            ca.encoding = TLS_TRUST_DER;
            ca.data = fixture_test_root;
            ca.size = sizeof fixture_test_root;
        }
        memset(&chain, 0, sizeof chain);
        code = tls_trust_verify(&peer, 1, &ca, TLS_LB_SERVER_NAME, &chain);
        sdk_trace_fprintf(stderr, "TLS_LOOPBACK_TRUST case=%s code=%d chain_count=%zu\n",
                          cases[i].name, code, chain.count);
        tls_trust_free_chain(&chain);
    }
}

static const char *tls_lb_exception_class(struct dvm *vm)
{
    dvm_ref pending = dvm_exception(vm);
    struct dvm_class *klass;
    if (!pending) return "none";
    klass = dvm_object_class(vm, pending);
    return klass && klass->name ? klass->name : "unknown";
}

static const char *tls_lb_exception_text(struct dvm *vm)
{
    static char description[512];
    dvm_ref pending = dvm_exception(vm);
    if (!pending) return "none";
    description[0] = '\0';
    dvm_describe_exception(vm, pending, description, sizeof description);
    return description[0] ? description : tls_lb_exception_class(vm);
}

static void tls_lb_close_socket(struct dvm *vm, dvm_ref socket)
{
    union dvm_value out = {0};
    if (!socket) return;
    if (!tls_lb_ssl_call(vm, "javax/net/ssl/SSLSocket", "close", "()V",
                         socket, NULL, 0, &out)) {
        if (dvm_exception(vm)) dvm_clear_exception(vm);
        (void)tls_lb_socket_call(vm, "java/net/Socket", "close", "()V",
                                 socket, NULL, 0, &out);
    }
    if (dvm_exception(vm)) dvm_clear_exception(vm);
}

static bool tls_lb_run(struct dvm *vm)
{
    const int port = LUNARIA_TLS_LOOPBACK_PORT;
    unsigned scenarios_passed = 0;
    const char *fail = NULL;
    char saved_exception[512] = {0};
    dvm_ref context = 0, plain = 0, ssl = 0, input = 0, output = 0;
    dvm_ref bad_context = 0, bad_plain = 0, bad_ssl = 0;
    dvm_ref wrong_plain = 0, wrong_ssl = 0;
    union dvm_value out = {0};
    unsigned char banner[64];
    unsigned char echo[128];
    int got;

    tls_lb_platform_probe();
    context = tls_lb_context_from_ca(vm, lunaria_loopback_test_ca_der,
                                     sizeof lunaria_loopback_test_ca_der);
    if (!context || dvm_exception(vm)) { fail = "context_chain"; goto done; }

    tls_lb_last_check = "plain_socket_create";
    plain = tls_lb_plain_socket(vm, port);
    if (!plain || dvm_exception(vm)) { fail = "plain_socket_create"; goto done; }
    tls_lb_last_check = "ssl_socket_create";
    ssl = tls_lb_ssl_socket(vm, context, plain, port, TLS_LB_SERVER_NAME);
    if (!ssl || dvm_exception(vm)) { fail = "ssl_socket_create"; goto done; }
    tls_lb_last_check = "handshake";
    out.l = 0;
    if (!tls_lb_ssl_call(vm, "javax/net/ssl/SSLSocket", "startHandshake", "()V",
                         ssl, NULL, 0, &out) || dvm_exception(vm)) {
        fail = "handshake"; goto done;
    }
    tls_lb_last_check = "banner_read";
    out.l = 0;
    if (!tls_lb_ssl_call(vm, "javax/net/ssl/SSLSocket", "getInputStream",
                         "()Ljava/io/InputStream;", ssl, NULL, 0, &out) ||
        dvm_exception(vm) || !out.l) { fail = "get_input_stream"; goto done; }
    input = out.l;
    got = tls_lb_read_exact(vm, input, banner, sizeof(TLS_LB_BANNER) - 1);
    if (got != (int)(sizeof(TLS_LB_BANNER) - 1) ||
        memcmp(banner, TLS_LB_BANNER, sizeof(TLS_LB_BANNER) - 1) != 0) {
        fail = "banner_read"; goto done;
    }
    tls_lb_last_check = "request_write";
    out.l = 0;
    if (!tls_lb_ssl_call(vm, "javax/net/ssl/SSLSocket", "getOutputStream",
                         "()Ljava/io/OutputStream;", ssl, NULL, 0, &out) ||
        dvm_exception(vm) || !out.l) { fail = "get_output_stream"; goto done; }
    output = out.l;
    if (!tls_lb_write_all(vm, output, (const unsigned char *)TLS_LB_REQUEST,
                          sizeof(TLS_LB_REQUEST) - 1)) { fail = "request_write"; goto done; }
    tls_lb_last_check = "echo_read";
    got = tls_lb_read_exact(vm, input, echo, (sizeof(TLS_LB_ECHO_PREFIX) - 1) +
                            (sizeof(TLS_LB_REQUEST) - 1));
    if (got != (int)((sizeof(TLS_LB_ECHO_PREFIX) - 1) +
                     (sizeof(TLS_LB_REQUEST) - 1)) ||
        memcmp(echo, TLS_LB_ECHO_PREFIX, sizeof(TLS_LB_ECHO_PREFIX) - 1) != 0 ||
        memcmp(echo + sizeof(TLS_LB_ECHO_PREFIX) - 1, TLS_LB_REQUEST,
               sizeof(TLS_LB_REQUEST) - 1) != 0 || echo[got - 1] != '\n') {
        fail = "echo_read"; goto done;
    }
    tls_lb_last_check = "ssl_close";
    out.l = 0;
    if (!tls_lb_ssl_call(vm, "javax/net/ssl/SSLSocket", "close", "()V", ssl,
                         NULL, 0, &out) || dvm_exception(vm)) { fail = "ssl_close"; goto done; }
    ssl = 0;
    plain = 0;
    scenarios_passed++;

    /* An unrelated CA must fail specifically at certificate trust validation. */
    bad_context = tls_lb_context_from_ca(vm, lunaria_loopback_untrusted_ca_der,
                                         sizeof lunaria_loopback_untrusted_ca_der);
    if (!bad_context || dvm_exception(vm)) { fail = "untrusted_context_chain"; goto done; }
    tls_lb_last_check = "untrusted_plain_socket_create";
    bad_plain = tls_lb_plain_socket(vm, port);
    if (!bad_plain || dvm_exception(vm)) { fail = "untrusted_plain_socket_create"; goto done; }
    tls_lb_last_check = "untrusted_ssl_socket_create";
    bad_ssl = tls_lb_ssl_socket(vm, bad_context, bad_plain, port, TLS_LB_SERVER_NAME);
    if (!bad_ssl || dvm_exception(vm)) { fail = "untrusted_ssl_socket_create"; goto done; }
    tls_lb_last_check = "untrusted_handshake_rejected";
    out.l = 0;
    (void)tls_lb_ssl_call(vm, "javax/net/ssl/SSLSocket", "startHandshake", "()V",
                          bad_ssl, NULL, 0, &out);
    if (!dvm_exception(vm)) { fail = "untrusted_handshake_accepted"; goto done; }
    {
        const char *klass = tls_lb_exception_class(vm);
        const char *description = tls_lb_exception_text(vm);
        if ((strcmp(klass, "javax/net/ssl/SSLHandshakeException") != 0 &&
             strcmp(klass, "javax/net/ssl/SSLException") != 0) ||
            !strstr(description, "certificate trust verification failed")) {
            sdk_trace_fprintf(stderr, "TLS_LOOPBACK_CA wrong_exception=%s description=%s\n",
                              klass, description);
            fail = "untrusted_handshake_wrong_reason"; goto done;
        }
    }
    dvm_clear_exception(vm);
    tls_lb_close_socket(vm, bad_ssl);
    bad_ssl = 0;
    bad_plain = 0; /* autoClose closed the underlying socket */
    scenarios_passed++;

    /* A valid CA with a wrong peer name must fail only for hostname mismatch. */
    tls_lb_last_check = "hostname_plain_socket_create";
    wrong_plain = tls_lb_plain_socket(vm, port);
    if (!wrong_plain || dvm_exception(vm)) { fail = "hostname_plain_socket_create"; goto done; }
    tls_lb_last_check = "hostname_ssl_socket_create";
    wrong_ssl = tls_lb_ssl_socket(vm, context, wrong_plain, port, "wrong.test");
    if (!wrong_ssl || dvm_exception(vm)) { fail = "hostname_ssl_socket_create"; goto done; }
    tls_lb_last_check = "hostname_handshake_rejected";
    out.l = 0;
    (void)tls_lb_ssl_call(vm, "javax/net/ssl/SSLSocket", "startHandshake", "()V",
                          wrong_ssl, NULL, 0, &out);
    if (!dvm_exception(vm)) { fail = "hostname_handshake_accepted"; goto done; }
    {
        const char *klass = tls_lb_exception_class(vm);
        const char *description = tls_lb_exception_text(vm);
        if (strcmp(klass, "javax/net/ssl/SSLHandshakeException") != 0 ||
            !strstr(description, "hostname")) {
            sdk_trace_fprintf(stderr, "TLS_LOOPBACK_HOSTNAME wrong_exception=%s description=%s\n",
                              klass, description);
            fail = "hostname_handshake_wrong_reason"; goto done;
        }
    }
    dvm_clear_exception(vm);
    tls_lb_close_socket(vm, wrong_ssl);
    wrong_ssl = 0;
    wrong_plain = 0;
    scenarios_passed++;
    tls_lb_last_check = NULL;

    if (scenarios_passed != 3) { fail = "scenario_count"; goto done; }
    sdk_trace_fprintf(stderr,
        "TLS_LOOPBACK_SELFTEST PASS scenarios=%u binders=keystore host=%s port=%d "
        "banner=1 request_echo=1 ca_reject=1 hostname_reject=1 expected_scenarios=3\n",
        scenarios_passed, TLS_LB_SERVER_NAME, port);
    return true;

done:
    snprintf(saved_exception, sizeof saved_exception, "%s", tls_lb_exception_text(vm));
    if (dvm_exception(vm)) dvm_clear_exception(vm);
    tls_lb_close_socket(vm, ssl);
    tls_lb_close_socket(vm, plain);
    tls_lb_close_socket(vm, bad_ssl);
    tls_lb_close_socket(vm, bad_plain);
    tls_lb_close_socket(vm, wrong_ssl);
    tls_lb_close_socket(vm, wrong_plain);
    sdk_trace_fprintf(stderr,
        "TLS_LOOPBACK_SELFTEST FAIL check=%s scenarios=%u/3 exception=%s\n",
        fail ? fail : (tls_lb_last_check ? tls_lb_last_check : "unknown"),
        scenarios_passed, saved_exception);
    return false;
}

#endif /* LUNARIA_TLS_LOOPBACK_PORT */

bool tls_loopback_selftest(struct dvm *vm)
{
#ifdef LUNARIA_TLS_LOOPBACK_PORT
    if (!vm) return false;
    return tls_lb_run(vm);
#else
    (void)vm;
    sdk_trace_fprintf(stderr, "TLS_LOOPBACK_SELFTEST SKIP reason=not_configured\n");
    return true;
#endif
}
