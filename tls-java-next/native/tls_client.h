/* Native TLS client session over an already connected TCP descriptor,
 * backed by source-built Mbed TLS 3.6.7.
 *
 * Independent of the DVM: callers own the interpreter lock and must release
 * it around blocking calls. Trust is never decided by Mbed TLS's anchor-less
 * X.509 result; it is decided only by the caller's trust callback, which runs
 * synchronously inside the handshake before it can complete. Hostname
 * verification is not performed here (it belongs to the Java layer). */
#ifndef TLS_JAVA_NEXT_TLS_CLIENT_H
#define TLS_JAVA_NEXT_TLS_CLIENT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct tls_client tls_client;

/* One peer certificate in DER. */
typedef struct {
    const unsigned char *der;
    size_t length;
} tls_cert_view;

/* Called on the handshaking thread after the server Certificate message is
 * parsed and before the handshake can complete. `chain` is the complete peer
 * list exactly as sent (leaf first); views are valid only during the call.
 * `auth_type` uses JSSE/Conscrypt names: "ECDHE_RSA", "ECDHE_ECDSA", "RSA",
 * or "GENERIC" for TLS 1.3. Return 0 to accept. Any other value rejects: the
 * handshake fails with TLS_CLIENT_ERR_TRUST_REJECTED and no application data
 * is ever exposed. */
typedef int (*tls_trust_fn)(void *context, const tls_cert_view *chain,
                            size_t count, const char *auth_type);

enum {
    TLS_CLIENT_OK = 0,
    TLS_CLIENT_ERR_ARGUMENT = -1,
    TLS_CLIENT_ERR_MEMORY = -2,
    TLS_CLIENT_ERR_STATE = -3,          /* wrong call order or already closed */
    TLS_CLIENT_ERR_IO = -4,             /* socket error, see tls_client_last_errno */
    TLS_CLIENT_ERR_TIMEOUT = -5,
    TLS_CLIENT_ERR_PEER_CLOSED = -6,    /* TCP closed during handshake/write */
    TLS_CLIENT_ERR_PROTOCOL = -7,       /* alert or protocol failure, see last_mbedtls_error */
    TLS_CLIENT_ERR_TRUST_REJECTED = -8,
    TLS_CLIENT_ERR_CANCELLED = -9,      /* tls_client_cancel was called */
    TLS_CLIENT_ERR_INIT = -10           /* RNG/PSA/threading initialisation failure */
};

typedef struct {
    const char *server_name;          /* SNI; NULL sends none (IP literal peer) */
    const char *const *alpn;          /* NULL-terminated list, or NULL for no ALPN */
    const char *const *protocols;     /* NULL-terminated Java names ("TLSv1.2", "TLSv1.3"), NULL = defaults */
    const char *const *cipher_suites; /* NULL-terminated Java/IANA names, NULL = defaults */
    tls_trust_fn trust;               /* required */
    void *trust_context;
} tls_client_config;

/* Static, library-owned name lists. Return their length. */
size_t tls_client_supported_protocols(const char *const **names);
size_t tls_client_default_protocols(const char *const **names);
size_t tls_client_supported_cipher_suites(const char *const **names);
size_t tls_client_default_cipher_suites(const char *const **names);

/* Does not take ownership of fd and never closes it. The library may switch
 * fd to non-blocking mode and uses poll() for timeouts. Unknown protocol or
 * suite names, or an empty resulting set, fail with TLS_CLIENT_ERR_ARGUMENT. */
int tls_client_create(int fd, const tls_client_config *config, tls_client **out);
/* timeout_ms <= 0 waits without a deadline (still cancellable). */
int tls_client_handshake(tls_client *client, int timeout_ms);
/* >0 bytes read; 0 orderly end of stream; <0 error code. Calls that touch
 * the same TLS client are serialized for the full operation, including I/O
 * waits; cancellation wakes the current operation. */
long tls_client_read(tls_client *client, unsigned char *buffer, size_t length, int timeout_ms);
/* Writes every byte or fails; returns length or <0. */
long tls_client_write(tls_client *client, const unsigned char *buffer, size_t length, int timeout_ms);
/* Decrypted bytes readable without blocking. */
size_t tls_client_pending(tls_client *client);
/* Best-effort close_notify with bounded wait. Never closes fd. */
int tls_client_close_notify(tls_client *client, int timeout_ms);
/* Async-signal-unsafe but thread-safe: wakes blocked calls on other threads,
 * which return TLS_CLIENT_ERR_CANCELLED; later calls fail the same way. */
void tls_client_cancel(tls_client *client);
/* Caller guarantees no other thread is still inside a call on client. */
void tls_client_free(tls_client *client);

/* Valid after a successful handshake until free. */
const char *tls_client_protocol(const tls_client *client);      /* "TLSv1.2" / "TLSv1.3" */
const char *tls_client_cipher_suite(const tls_client *client);  /* Java/IANA name */
const char *tls_client_alpn_selected(const tls_client *client); /* NULL when none */
/* Copies up to max views; returns the total chain length. */
size_t tls_client_peer_chain(const tls_client *client, tls_cert_view *out, size_t max);

int tls_client_last_mbedtls_error(const tls_client *client);
int tls_client_last_errno(const tls_client *client);
const char *tls_client_error_text(int code);

#ifdef __cplusplus
}
#endif
#endif
