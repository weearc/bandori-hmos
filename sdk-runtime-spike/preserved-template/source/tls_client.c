#include "tls_client.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#include "mbedtls/ssl_ciphersuites.h"
#include "mbedtls/x509.h"
#include "mbedtls/x509_crt.h"
#include "psa/crypto.h"

#define TLS_MAX_CERTIFICATES 128U
#define TLS_MAX_ALPN_PROTOCOLS 128U
#define TLS_MAX_ALPN_LENGTH 255U

static const char *const supported_protocols[] = {
    "TLSv1.2",
    "TLSv1.3",
    NULL
};

/* These are the Java/IANA spellings accepted by this wrapper. */
static const char *const supported_cipher_suites[] = {
    "TLS_AES_128_GCM_SHA256",
    "TLS_AES_256_GCM_SHA384",
    "TLS_CHACHA20_POLY1305_SHA256",
    "TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256",
    "TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256",
    "TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384",
    "TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384",
    "TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256",
    "TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256",
    "TLS_DHE_RSA_WITH_AES_128_GCM_SHA256",
    "TLS_DHE_RSA_WITH_AES_256_GCM_SHA384",
    NULL
};

typedef struct {
    const char *java_name;
    const char *mbedtls_name;
} cipher_name_map;

static const cipher_name_map cipher_names[] = {
    { "TLS_AES_128_GCM_SHA256", "TLS1-3-AES-128-GCM-SHA256" },
    { "TLS_AES_256_GCM_SHA384", "TLS1-3-AES-256-GCM-SHA384" },
    { "TLS_CHACHA20_POLY1305_SHA256", "TLS1-3-CHACHA20-POLY1305-SHA256" },
    { "TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256", "TLS-ECDHE-ECDSA-WITH-AES-128-GCM-SHA256" },
    { "TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256", "TLS-ECDHE-RSA-WITH-AES-128-GCM-SHA256" },
    { "TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384", "TLS-ECDHE-ECDSA-WITH-AES-256-GCM-SHA384" },
    { "TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384", "TLS-ECDHE-RSA-WITH-AES-256-GCM-SHA384" },
    { "TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256", "TLS-ECDHE-ECDSA-WITH-CHACHA20-POLY1305-SHA256" },
    { "TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256", "TLS-ECDHE-RSA-WITH-CHACHA20-POLY1305-SHA256" },
    { "TLS_DHE_RSA_WITH_AES_128_GCM_SHA256", "TLS-DHE-RSA-WITH-AES-128-GCM-SHA256" },
    { "TLS_DHE_RSA_WITH_AES_256_GCM_SHA384", "TLS-DHE-RSA-WITH-AES-256-GCM-SHA384" }
};

struct tls_client {
    int fd;
    int cancel_read_fd;
    int cancel_write_fd;
    int original_fd_flags;
    int restore_fd_flags;

    pthread_mutex_t operation_lock;
    int operation_lock_initialized;
    volatile int cancelled;
    volatile int handshake_done;
    volatile int failed;
    volatile int close_sent;

    int trust_called;
    int trust_rejected;
    int verify_memory_error;
    int last_mbedtls_error;
    int last_errno;

    tls_trust_fn trust;
    void *trust_context;

    mbedtls_ssl_config conf;
    mbedtls_ssl_context ssl;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    int conf_initialized;
    int ssl_initialized;
    int entropy_initialized;
    int drbg_initialized;

    int *cipher_ids;
    char **alpn_list;
    size_t alpn_count;

    tls_cert_view *peer_chain;
    size_t peer_chain_count;
};

static pthread_once_t psa_once = PTHREAD_ONCE_INIT;
static psa_status_t psa_init_status = PSA_ERROR_GENERIC_ERROR;

static void initialize_psa(void)
{
    psa_init_status = psa_crypto_init();
}

static int ensure_psa_initialized(void)
{
    if (pthread_once(&psa_once, initialize_psa) != 0) {
        return TLS_CLIENT_ERR_INIT;
    }
    return psa_init_status == PSA_SUCCESS ? TLS_CLIENT_OK : TLS_CLIENT_ERR_INIT;
}

static char *duplicate_string(const char *value)
{
    size_t length;
    char *copy;

    if (value == NULL) {
        return NULL;
    }
    length = strlen(value);
    copy = (char *) malloc(length + 1U);
    if (copy == NULL) {
        return NULL;
    }
    memcpy(copy, value, length + 1U);
    return copy;
}

static int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return -1;
    }
    if ((flags & O_NONBLOCK) == 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        return -1;
    }
    return flags;
}

static int64_t monotonic_millis(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return -1;
    }
    return (int64_t) now.tv_sec * 1000LL + now.tv_nsec / 1000000LL;
}

static int64_t make_deadline(int timeout_ms)
{
    int64_t now;
    if (timeout_ms <= 0) {
        return -1;
    }
    now = monotonic_millis();
    if (now < 0 || INT64_MAX - now < timeout_ms) {
        return INT64_MAX;
    }
    return now + timeout_ms;
}

static int wait_timeout(int64_t deadline)
{
    int64_t now;
    int64_t remaining;

    if (deadline < 0) {
        return -1;
    }
    now = monotonic_millis();
    if (now < 0) {
        return 0;
    }
    remaining = deadline - now;
    if (remaining <= 0) {
        return 0;
    }
    if (remaining > INT_MAX) {
        return INT_MAX;
    }
    return (int) remaining;
}

static int operation_cancelled(const tls_client *client)
{
    return __atomic_load_n(&client->cancelled, __ATOMIC_ACQUIRE) != 0;
}

static int wait_for_io(tls_client *client, short events, int64_t deadline)
{
    struct pollfd descriptors[2];

    for (;;) {
        int result;
        int timeout;

        if (operation_cancelled(client)) {
            return TLS_CLIENT_ERR_CANCELLED;
        }
        timeout = wait_timeout(deadline);
        if (deadline >= 0 && timeout == 0) {
            client->last_errno = ETIMEDOUT;
            return TLS_CLIENT_ERR_TIMEOUT;
        }

        descriptors[0].fd = client->fd;
        descriptors[0].events = events;
        descriptors[0].revents = 0;
        descriptors[1].fd = client->cancel_read_fd;
        descriptors[1].events = POLLIN;
        descriptors[1].revents = 0;
        result = poll(descriptors, 2, timeout);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            client->last_errno = errno;
            return TLS_CLIENT_ERR_IO;
        }
        if (descriptors[1].revents != 0) {
            return TLS_CLIENT_ERR_CANCELLED;
        }
        if (descriptors[0].revents & POLLNVAL) {
            client->last_errno = EBADF;
            return TLS_CLIENT_ERR_IO;
        }
        if (descriptors[0].revents != 0) {
            return TLS_CLIENT_OK;
        }
        if (result == 0) {
            client->last_errno = ETIMEDOUT;
            return TLS_CLIENT_ERR_TIMEOUT;
        }
    }
}

static int native_recv(void *context, unsigned char *buffer, size_t length)
{
    tls_client *client = (tls_client *) context;
    ssize_t result;

    result = read(client->fd, buffer, length);
    if (result >= 0) {
        return result > INT_MAX ? INT_MAX : (int) result;
    }
    client->last_errno = errno;
    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
        return MBEDTLS_ERR_SSL_WANT_READ;
    }
    if (errno == EPIPE || errno == ECONNRESET || errno == ENOTCONN) {
        return MBEDTLS_ERR_NET_CONN_RESET;
    }
    return MBEDTLS_ERR_NET_RECV_FAILED;
}

static int native_send(void *context, const unsigned char *buffer, size_t length)
{
    tls_client *client = (tls_client *) context;
    ssize_t result;

    result = write(client->fd, buffer, length);
    if (result >= 0) {
        return result > INT_MAX ? INT_MAX : (int) result;
    }
    client->last_errno = errno;
    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
        return MBEDTLS_ERR_SSL_WANT_WRITE;
    }
    if (errno == EPIPE || errno == ECONNRESET || errno == ENOTCONN) {
        return MBEDTLS_ERR_NET_CONN_RESET;
    }
    return MBEDTLS_ERR_NET_SEND_FAILED;
}

static const char *auth_type_for_client(const tls_client *client)
{
    const char *version = mbedtls_ssl_get_version(&client->ssl);
    const char *suite = mbedtls_ssl_get_ciphersuite(&client->ssl);

    if ((version != NULL && strstr(version, "1.3") != NULL) ||
        (suite != NULL &&
         (strstr(suite, "TLS-AES-") == suite ||
          strstr(suite, "TLS-CHACHA20-") == suite))) {
        return "GENERIC";
    }
    if (suite != NULL && strstr(suite, "ECDHE-ECDSA") != NULL) {
        return "ECDHE_ECDSA";
    }
    if (suite != NULL && strstr(suite, "ECDHE-RSA") != NULL) {
        return "ECDHE_RSA";
    }
    if (suite != NULL && strstr(suite, "DHE-RSA") != NULL) {
        return "DHE_RSA";
    }
    if (suite != NULL && strstr(suite, "RSA") != NULL) {
        return "RSA";
    }
    return "GENERIC";
}

static void free_peer_chain(tls_client *client)
{
    size_t index;
    if (client->peer_chain == NULL) {
        client->peer_chain_count = 0;
        return;
    }
    for (index = 0; index < client->peer_chain_count; ++index) {
        free((void *) client->peer_chain[index].der);
    }
    free(client->peer_chain);
    client->peer_chain = NULL;
    client->peer_chain_count = 0;
}

static size_t certificate_count(const mbedtls_x509_crt *certificate)
{
    size_t count = 0;
    while (certificate != NULL && count < TLS_MAX_CERTIFICATES) {
        ++count;
        certificate = certificate->next;
    }
    return certificate == NULL ? count : 0;
}

static int capture_peer_chain(tls_client *client, mbedtls_x509_crt *certificate)
{
    tls_cert_view *views;
    size_t count;
    size_t index;
    mbedtls_x509_crt *current;

    count = certificate_count(certificate);
    if (count == 0) {
        return TLS_CLIENT_ERR_PROTOCOL;
    }
    views = (tls_cert_view *) calloc(count, sizeof(*views));
    if (views == NULL) {
        return TLS_CLIENT_ERR_MEMORY;
    }
    current = certificate;
    for (index = 0; index < count; ++index, current = current->next) {
        if (current->raw.p == NULL || current->raw.len == 0) {
            free(views);
            return TLS_CLIENT_ERR_PROTOCOL;
        }
        views[index].der = current->raw.p;
        views[index].length = current->raw.len;
    }

    free_peer_chain(client);
    client->peer_chain = (tls_cert_view *) calloc(count, sizeof(*client->peer_chain));
    if (client->peer_chain == NULL) {
        free(views);
        return TLS_CLIENT_ERR_MEMORY;
    }
    for (index = 0; index < count; ++index) {
        unsigned char *copy = (unsigned char *) malloc(views[index].length);
        if (copy == NULL) {
            client->peer_chain_count = index;
            free_peer_chain(client);
            free(views);
            return TLS_CLIENT_ERR_MEMORY;
        }
        memcpy(copy, views[index].der, views[index].length);
        client->peer_chain[index].der = copy;
        client->peer_chain[index].length = views[index].length;
    }
    client->peer_chain_count = count;
    free(views);
    return TLS_CLIENT_OK;
}

static int verify_peer(void *context, mbedtls_x509_crt *certificate,
                       int depth, uint32_t *flags)
{
    tls_client *client = (tls_client *) context;
    const mbedtls_x509_crt *peer_chain;
    tls_cert_view *views = NULL;
    size_t count;
    size_t index;
    const mbedtls_x509_crt *current;
    int callback_result;

    if (flags == NULL) {
        return MBEDTLS_ERR_X509_FATAL_ERROR;
    }
    /* Java/TrustManager owns trust anchors and policy. */
    *flags = 0;
    if (depth != 0 || client->trust_called) {
        return 0;
    }
    client->trust_called = 1;

    /* At depth zero, certificate is the leaf and retains the parsed peer
     * chain through next. The public get_peer_cert accessor reads ssl->session,
     * which is not published until the initial handshake finishes; calling it
     * here rejected every initial handshake before invoking the trust policy.
     * Validate and copy the callback-owned chain while it is still alive. */
    peer_chain = certificate;
    count = certificate_count(peer_chain);
    if (count == 0) {
        client->trust_rejected = 1;
        return MBEDTLS_ERR_X509_FATAL_ERROR;
    }
    views = (tls_cert_view *) calloc(count, sizeof(*views));
    if (views == NULL) {
        client->verify_memory_error = 1;
        return MBEDTLS_ERR_X509_FATAL_ERROR;
    }
    current = peer_chain;
    for (index = 0; index < count; ++index, current = current->next) {
        if (current->raw.p == NULL || current->raw.len == 0) {
            free(views);
            client->trust_rejected = 1;
            return MBEDTLS_ERR_X509_FATAL_ERROR;
        }
        views[index].der = current->raw.p;
        views[index].length = current->raw.len;
    }

    callback_result = client->trust(client->trust_context, views, count,
                                    auth_type_for_client(client));
    if (callback_result != 0) {
        client->trust_rejected = 1;
        free(views);
        return MBEDTLS_ERR_X509_FATAL_ERROR;
    }
    free(views);
    if (capture_peer_chain(client, (mbedtls_x509_crt *) peer_chain) != TLS_CLIENT_OK) {
        client->verify_memory_error = 1;
        return MBEDTLS_ERR_X509_FATAL_ERROR;
    }
    return 0;
}

static int suite_id_from_java_name(const char *name)
{
    size_t index;

    if (name == NULL || *name == '\0') {
        return 0;
    }
    for (index = 0; index < sizeof(cipher_names) / sizeof(cipher_names[0]); ++index) {
        if (strcmp(name, cipher_names[index].java_name) == 0) {
            return mbedtls_ssl_get_ciphersuite_id(cipher_names[index].mbedtls_name);
        }
    }
    return 0;
}

static const char *java_cipher_name(const char *mbedtls_name)
{
    size_t index;

    if (mbedtls_name == NULL) {
        return NULL;
    }
    for (index = 0; index < sizeof(cipher_names) / sizeof(cipher_names[0]); ++index) {
        if (strcmp(mbedtls_name, cipher_names[index].mbedtls_name) == 0) {
            return cipher_names[index].java_name;
        }
    }
    return mbedtls_name;
}

static int copy_alpn(tls_client *client, const char *const *source)
{
    size_t count = 0;
    size_t index;

    if (source == NULL) {
        return TLS_CLIENT_OK;
    }
    while (source[count] != NULL) {
        size_t length = strlen(source[count]);
        if (++count > TLS_MAX_ALPN_PROTOCOLS || length == 0 || length > TLS_MAX_ALPN_LENGTH) {
            return TLS_CLIENT_ERR_ARGUMENT;
        }
    }
    if (count == 0) {
        return TLS_CLIENT_ERR_ARGUMENT;
    }
    client->alpn_list = (char **) calloc(count + 1U, sizeof(*client->alpn_list));
    if (client->alpn_list == NULL) {
        return TLS_CLIENT_ERR_MEMORY;
    }
    for (index = 0; index < count; ++index) {
        client->alpn_list[index] = duplicate_string(source[index]);
        if (client->alpn_list[index] == NULL) {
            return TLS_CLIENT_ERR_MEMORY;
        }
    }
    client->alpn_count = count;
    return TLS_CLIENT_OK;
}

static void free_alpn(tls_client *client)
{
    size_t index;
    if (client->alpn_list == NULL) {
        return;
    }
    for (index = 0; index < client->alpn_count; ++index) {
        free(client->alpn_list[index]);
    }
    free(client->alpn_list);
    client->alpn_list = NULL;
    client->alpn_count = 0;
}

static int configure_protocols(mbedtls_ssl_config *conf,
                               const char *const *protocols)
{
    int has12 = 0;
    int has13 = 0;
    size_t index;

    if (protocols == NULL) {
        has12 = 1;
        has13 = 1;
    } else {
        for (index = 0; protocols[index] != NULL; ++index) {
            if (strcmp(protocols[index], "TLSv1.2") == 0) {
                has12 = 1;
            } else if (strcmp(protocols[index], "TLSv1.3") == 0) {
                has13 = 1;
            } else {
                return TLS_CLIENT_ERR_ARGUMENT;
            }
        }
        if (!has12 && !has13) {
            return TLS_CLIENT_ERR_ARGUMENT;
        }
    }
    if (has12 && has13) {
        mbedtls_ssl_conf_min_tls_version(conf, MBEDTLS_SSL_VERSION_TLS1_2);
        mbedtls_ssl_conf_max_tls_version(conf, MBEDTLS_SSL_VERSION_TLS1_3);
    } else if (has12) {
        mbedtls_ssl_conf_min_tls_version(conf, MBEDTLS_SSL_VERSION_TLS1_2);
        mbedtls_ssl_conf_max_tls_version(conf, MBEDTLS_SSL_VERSION_TLS1_2);
    } else {
        mbedtls_ssl_conf_min_tls_version(conf, MBEDTLS_SSL_VERSION_TLS1_3);
        mbedtls_ssl_conf_max_tls_version(conf, MBEDTLS_SSL_VERSION_TLS1_3);
    }
    return TLS_CLIENT_OK;
}

static int configure_cipher_suites(tls_client *client,
                                   const char *const *names)
{
    size_t count = 0;
    size_t index;

    if (names == NULL) {
        count = sizeof(supported_cipher_suites) / sizeof(supported_cipher_suites[0]) - 1U;
    } else {
        while (names[count] != NULL) {
            if (++count > 256U) {
                return TLS_CLIENT_ERR_ARGUMENT;
            }
        }
        if (count == 0) {
            return TLS_CLIENT_ERR_ARGUMENT;
        }
    }
    client->cipher_ids = (int *) calloc(count + 1U, sizeof(*client->cipher_ids));
    if (client->cipher_ids == NULL) {
        return TLS_CLIENT_ERR_MEMORY;
    }
    for (index = 0; index < count; ++index) {
        const char *name = names == NULL ? supported_cipher_suites[index] : names[index];
        client->cipher_ids[index] = suite_id_from_java_name(name);
        if (client->cipher_ids[index] == 0) {
            return TLS_CLIENT_ERR_ARGUMENT;
        }
    }
    client->cipher_ids[count] = 0;
    mbedtls_ssl_conf_ciphersuites(&client->conf, client->cipher_ids);
    return TLS_CLIENT_OK;
}

static int map_mbedtls_error(tls_client *client, int error, int handshake)
{
    client->last_mbedtls_error = error;
    if (operation_cancelled(client)) {
        return TLS_CLIENT_ERR_CANCELLED;
    }
    if (client->trust_rejected) {
        return TLS_CLIENT_ERR_TRUST_REJECTED;
    }
    if (client->verify_memory_error) {
        return TLS_CLIENT_ERR_MEMORY;
    }
    if (error == MBEDTLS_ERR_SSL_TIMEOUT) {
        client->last_errno = ETIMEDOUT;
        return TLS_CLIENT_ERR_TIMEOUT;
    }
    if (error == MBEDTLS_ERR_SSL_CONN_EOF ||
        error == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY ||
        error == MBEDTLS_ERR_NET_CONN_RESET) {
        return handshake ? TLS_CLIENT_ERR_PEER_CLOSED : TLS_CLIENT_ERR_PEER_CLOSED;
    }
    if (error == MBEDTLS_ERR_NET_RECV_FAILED || error == MBEDTLS_ERR_NET_SEND_FAILED) {
        return TLS_CLIENT_ERR_IO;
    }
    return handshake ? TLS_CLIENT_ERR_PROTOCOL : TLS_CLIENT_ERR_PROTOCOL;
}

static int check_client_state(tls_client *client, int require_handshake)
{
    if (client == NULL) {
        return TLS_CLIENT_ERR_ARGUMENT;
    }
    if (operation_cancelled(client)) {
        return TLS_CLIENT_ERR_CANCELLED;
    }
    if (__atomic_load_n(&client->failed, __ATOMIC_ACQUIRE)) {
        return client->trust_rejected ? TLS_CLIENT_ERR_TRUST_REJECTED : TLS_CLIENT_ERR_STATE;
    }
    if (require_handshake && !__atomic_load_n(&client->handshake_done, __ATOMIC_ACQUIRE)) {
        return TLS_CLIENT_ERR_STATE;
    }
    if (!require_handshake && __atomic_load_n(&client->handshake_done, __ATOMIC_ACQUIRE)) {
        return TLS_CLIENT_ERR_STATE;
    }
    if (__atomic_load_n(&client->close_sent, __ATOMIC_ACQUIRE)) {
        return TLS_CLIENT_ERR_STATE;
    }
    return TLS_CLIENT_OK;
}

static int wait_after_want(tls_client *client, int error, int64_t deadline)
{
    if (error == MBEDTLS_ERR_SSL_WANT_READ) {
        return wait_for_io(client, POLLIN, deadline);
    }
    if (error == MBEDTLS_ERR_SSL_WANT_WRITE) {
        return wait_for_io(client, POLLOUT, deadline);
    }
    return TLS_CLIENT_ERR_PROTOCOL;
}

size_t tls_client_supported_protocols(const char *const **names)
{
    if (names != NULL) {
        *names = supported_protocols;
    }
    return 2U;
}

size_t tls_client_default_protocols(const char *const **names)
{
    return tls_client_supported_protocols(names);
}

size_t tls_client_supported_cipher_suites(const char *const **names)
{
    if (names != NULL) {
        *names = supported_cipher_suites;
    }
    return sizeof(supported_cipher_suites) / sizeof(supported_cipher_suites[0]) - 1U;
}

size_t tls_client_default_cipher_suites(const char *const **names)
{
    return tls_client_supported_cipher_suites(names);
}

int tls_client_create(int fd, const tls_client_config *config, tls_client **out)
{
    tls_client *client;
    int result;
    int pipe_fds[2] = {-1, -1};
    int old_flags;

    if (fd < 0 || config == NULL || out == NULL || config->trust == NULL) {
        return TLS_CLIENT_ERR_ARGUMENT;
    }
    *out = NULL;
    result = ensure_psa_initialized();
    if (result != TLS_CLIENT_OK) {
        return result;
    }
    old_flags = set_nonblocking(fd);
    if (old_flags < 0) {
        return TLS_CLIENT_ERR_IO;
    }
    if (pipe(pipe_fds) != 0) {
        (void) fcntl(fd, F_SETFL, old_flags);
        return TLS_CLIENT_ERR_INIT;
    }
    if (set_nonblocking(pipe_fds[0]) < 0 || set_nonblocking(pipe_fds[1]) < 0) {
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        (void) fcntl(fd, F_SETFL, old_flags);
        return TLS_CLIENT_ERR_INIT;
    }

    client = (tls_client *) calloc(1, sizeof(*client));
    if (client == NULL) {
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        (void) fcntl(fd, F_SETFL, old_flags);
        return TLS_CLIENT_ERR_MEMORY;
    }
    client->fd = fd;
    client->cancel_read_fd = pipe_fds[0];
    client->cancel_write_fd = pipe_fds[1];
    client->original_fd_flags = old_flags;
    client->restore_fd_flags = 1;
    client->trust = config->trust;
    client->trust_context = config->trust_context;

    if (pthread_mutex_init(&client->operation_lock, NULL) != 0) {
        free(client);
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        (void) fcntl(fd, F_SETFL, old_flags);
        return TLS_CLIENT_ERR_INIT;
    }
    mbedtls_ssl_config_init(&client->conf);
    client->conf_initialized = 1;
    mbedtls_ssl_init(&client->ssl);
    client->ssl_initialized = 1;
    mbedtls_entropy_init(&client->entropy);
    client->entropy_initialized = 1;
    mbedtls_ctr_drbg_init(&client->drbg);
    client->drbg_initialized = 1;

    result = configure_protocols(&client->conf, config->protocols);
    if (result != TLS_CLIENT_OK) {
        goto fail;
    }
    result = mbedtls_ssl_config_defaults(&client->conf, MBEDTLS_SSL_IS_CLIENT,
                                         MBEDTLS_SSL_TRANSPORT_STREAM,
                                         MBEDTLS_SSL_PRESET_DEFAULT);
    if (result != 0) {
        client->last_mbedtls_error = result;
        result = TLS_CLIENT_ERR_INIT;
        goto fail;
    }
    /* Set the requested version range after defaults have initialized config. */
    result = configure_protocols(&client->conf, config->protocols);
    if (result != TLS_CLIENT_OK) {
        goto fail;
    }
    mbedtls_ssl_conf_authmode(&client->conf, MBEDTLS_SSL_VERIFY_OPTIONAL);
    mbedtls_ssl_conf_verify(&client->conf, verify_peer, client);
    mbedtls_ssl_conf_rng(&client->conf, mbedtls_ctr_drbg_random, &client->drbg);

    result = mbedtls_ctr_drbg_seed(&client->drbg, mbedtls_entropy_func,
                                   &client->entropy, NULL, 0);
    if (result != 0) {
        client->last_mbedtls_error = result;
        result = TLS_CLIENT_ERR_INIT;
        goto fail;
    }
    result = configure_cipher_suites(client, config->cipher_suites);
    if (result != TLS_CLIENT_OK) {
        goto fail;
    }
    result = copy_alpn(client, config->alpn);
    if (result != TLS_CLIENT_OK) {
        goto fail;
    }
    if (client->alpn_list != NULL) {
        result = mbedtls_ssl_conf_alpn_protocols(&client->conf,
                                                 (const char **) client->alpn_list);
        if (result != 0) {
            client->last_mbedtls_error = result;
            result = TLS_CLIENT_ERR_INIT;
            goto fail;
        }
    }
    result = mbedtls_ssl_setup(&client->ssl, &client->conf);
    if (result != 0) {
        client->last_mbedtls_error = result;
        result = TLS_CLIENT_ERR_INIT;
        goto fail;
    }
    if (config->server_name != NULL &&
        mbedtls_ssl_set_hostname(&client->ssl, config->server_name) != 0) {
        client->last_mbedtls_error = MBEDTLS_ERR_SSL_BAD_INPUT_DATA;
        result = TLS_CLIENT_ERR_ARGUMENT;
        goto fail;
    }
    mbedtls_ssl_set_bio(&client->ssl, client, native_send, native_recv, NULL);
    *out = client;
    return TLS_CLIENT_OK;

fail:
    free_alpn(client);
    free(client->cipher_ids);
    free_peer_chain(client);
    if (client->drbg_initialized) {
        mbedtls_ctr_drbg_free(&client->drbg);
    }
    if (client->entropy_initialized) {
        mbedtls_entropy_free(&client->entropy);
    }
    if (client->ssl_initialized) {
        mbedtls_ssl_free(&client->ssl);
    }
    if (client->conf_initialized) {
        mbedtls_ssl_config_free(&client->conf);
    }
    pthread_mutex_destroy(&client->operation_lock);
    close(client->cancel_read_fd);
    close(client->cancel_write_fd);
    if (client->restore_fd_flags) {
        (void) fcntl(client->fd, F_SETFL, client->original_fd_flags);
    }
    free(client);
    return result;
}

int tls_client_handshake(tls_client *client, int timeout_ms)
{
    int64_t deadline;
    int result;

    if (client == NULL) {
        return TLS_CLIENT_ERR_ARGUMENT;
    }
    deadline = make_deadline(timeout_ms);
    pthread_mutex_lock(&client->operation_lock);
    if (operation_cancelled(client)) {
        pthread_mutex_unlock(&client->operation_lock);
        return TLS_CLIENT_ERR_CANCELLED;
    }
    if (__atomic_load_n(&client->handshake_done, __ATOMIC_ACQUIRE)) {
        pthread_mutex_unlock(&client->operation_lock);
        return TLS_CLIENT_ERR_STATE;
    }
    if (__atomic_load_n(&client->failed, __ATOMIC_ACQUIRE)) {
        int state = client->trust_rejected ? TLS_CLIENT_ERR_TRUST_REJECTED : TLS_CLIENT_ERR_STATE;
        pthread_mutex_unlock(&client->operation_lock);
        return state;
    }
    for (;;) {
        int wait_result;

        if (operation_cancelled(client)) {
            pthread_mutex_unlock(&client->operation_lock);
            return TLS_CLIENT_ERR_CANCELLED;
        }
        result = mbedtls_ssl_handshake(&client->ssl);
        if (result == 0) {
            if (!client->trust_called || client->peer_chain_count == 0) {
                client->trust_rejected = 1;
                client->last_mbedtls_error = MBEDTLS_ERR_X509_FATAL_ERROR;
                __atomic_store_n(&client->failed, 1, __ATOMIC_RELEASE);
                pthread_mutex_unlock(&client->operation_lock);
                return TLS_CLIENT_ERR_TRUST_REJECTED;
            }
            __atomic_store_n(&client->handshake_done, 1, __ATOMIC_RELEASE);
            pthread_mutex_unlock(&client->operation_lock);
            return TLS_CLIENT_OK;
        }
        client->last_mbedtls_error = result;
        if (result == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) {
            continue;
        }
        if (result == MBEDTLS_ERR_SSL_WANT_READ || result == MBEDTLS_ERR_SSL_WANT_WRITE) {
            wait_result = wait_after_want(client, result, deadline);
            if (wait_result != TLS_CLIENT_OK) {
                pthread_mutex_unlock(&client->operation_lock);
                return wait_result;
            }
            continue;
        }
        result = map_mbedtls_error(client, result, 1);
        __atomic_store_n(&client->failed, 1, __ATOMIC_RELEASE);
        pthread_mutex_unlock(&client->operation_lock);
        return result;
    }
}

long tls_client_read(tls_client *client, unsigned char *buffer,
                     size_t length, int timeout_ms)
{
    int64_t deadline;
    int result;

    if (client == NULL || (buffer == NULL && length != 0)) {
        return TLS_CLIENT_ERR_ARGUMENT;
    }
    if (length == 0) {
        return 0;
    }
    deadline = make_deadline(timeout_ms);
    pthread_mutex_lock(&client->operation_lock);
    {
        int state = check_client_state(client, 1);
        if (state != TLS_CLIENT_OK) {
            pthread_mutex_unlock(&client->operation_lock);
            return state;
        }
    }
    for (;;) {
        int wait_result;

        if (operation_cancelled(client)) {
            pthread_mutex_unlock(&client->operation_lock);
            return TLS_CLIENT_ERR_CANCELLED;
        }
        result = mbedtls_ssl_read(&client->ssl, buffer, length);
        client->last_mbedtls_error = result < 0 ? result : 0;
        if (result > 0 || result == 0) {
            pthread_mutex_unlock(&client->operation_lock);
            return result;
        }
        if (result == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || result == MBEDTLS_ERR_SSL_CONN_EOF) {
            pthread_mutex_unlock(&client->operation_lock);
            return 0;
        }
        if (result == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) {
            continue;
        }
        if (result == MBEDTLS_ERR_SSL_WANT_READ || result == MBEDTLS_ERR_SSL_WANT_WRITE) {
            wait_result = wait_after_want(client, result, deadline);
            if (wait_result != TLS_CLIENT_OK) {
                pthread_mutex_unlock(&client->operation_lock);
                return wait_result;
            }
            continue;
        }
        result = map_mbedtls_error(client, result, 0);
        __atomic_store_n(&client->failed, 1, __ATOMIC_RELEASE);
        pthread_mutex_unlock(&client->operation_lock);
        return result;
    }
}

long tls_client_write(tls_client *client, const unsigned char *buffer,
                      size_t length, int timeout_ms)
{
    size_t offset = 0;
    int64_t deadline;

    if (client == NULL || (buffer == NULL && length != 0)) {
        return TLS_CLIENT_ERR_ARGUMENT;
    }
    if (length == 0) {
        return 0;
    }
    deadline = make_deadline(timeout_ms);
    pthread_mutex_lock(&client->operation_lock);
    {
        int state = check_client_state(client, 1);
        if (state != TLS_CLIENT_OK) {
            pthread_mutex_unlock(&client->operation_lock);
            return state;
        }
    }
    while (offset < length) {
        int result;
        int wait_result;

        if (operation_cancelled(client)) {
            pthread_mutex_unlock(&client->operation_lock);
            return TLS_CLIENT_ERR_CANCELLED;
        }
        result = mbedtls_ssl_write(&client->ssl, buffer + offset, length - offset);
        client->last_mbedtls_error = result < 0 ? result : 0;
        if (result > 0) {
            offset += (size_t) result;
            continue;
        }
        if (result == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) {
            continue;
        }
        if (result == MBEDTLS_ERR_SSL_WANT_READ || result == MBEDTLS_ERR_SSL_WANT_WRITE) {
            wait_result = wait_after_want(client, result, deadline);
            if (wait_result != TLS_CLIENT_OK) {
                pthread_mutex_unlock(&client->operation_lock);
                return wait_result;
            }
            continue;
        }
        if (result == 0) {
            result = MBEDTLS_ERR_SSL_CONN_EOF;
        }
        result = map_mbedtls_error(client, result, 0);
        __atomic_store_n(&client->failed, 1, __ATOMIC_RELEASE);
        pthread_mutex_unlock(&client->operation_lock);
        return result;
    }
    pthread_mutex_unlock(&client->operation_lock);
    return (long) length;
}

size_t tls_client_pending(tls_client *client)
{
    size_t pending;
    if (client == NULL) {
        return 0;
    }
    pthread_mutex_lock(&client->operation_lock);
    pending = mbedtls_ssl_get_bytes_avail(&client->ssl);
    pthread_mutex_unlock(&client->operation_lock);
    return pending;
}

int tls_client_close_notify(tls_client *client, int timeout_ms)
{
    int64_t deadline;

    if (client == NULL) {
        return TLS_CLIENT_ERR_ARGUMENT;
    }
    deadline = make_deadline(timeout_ms);
    pthread_mutex_lock(&client->operation_lock);
    {
        int state = check_client_state(client, 1);
        if (state != TLS_CLIENT_OK) {
            pthread_mutex_unlock(&client->operation_lock);
            return state;
        }
    }
    for (;;) {
        int result;
        int wait_result;

        if (operation_cancelled(client)) {
            pthread_mutex_unlock(&client->operation_lock);
            return TLS_CLIENT_ERR_CANCELLED;
        }
        result = mbedtls_ssl_close_notify(&client->ssl);
        client->last_mbedtls_error = result;
        if (result == 0) {
            __atomic_store_n(&client->close_sent, 1, __ATOMIC_RELEASE);
            pthread_mutex_unlock(&client->operation_lock);
            return TLS_CLIENT_OK;
        }
        if (result == MBEDTLS_ERR_SSL_WANT_READ || result == MBEDTLS_ERR_SSL_WANT_WRITE) {
            wait_result = wait_after_want(client, result, deadline);
            if (wait_result != TLS_CLIENT_OK) {
                pthread_mutex_unlock(&client->operation_lock);
                return wait_result;
            }
            continue;
        }
        result = map_mbedtls_error(client, result, 0);
        pthread_mutex_unlock(&client->operation_lock);
        return result;
    }
}

void tls_client_cancel(tls_client *client)
{
    unsigned char marker = 1;
    if (client == NULL) {
        return;
    }
    __atomic_store_n(&client->cancelled, 1, __ATOMIC_RELEASE);
    (void) write(client->cancel_write_fd, &marker, sizeof(marker));
}

void tls_client_free(tls_client *client)
{
    if (client == NULL) {
        return;
    }
    free_peer_chain(client);
    free_alpn(client);
    free(client->cipher_ids);
    if (client->drbg_initialized) {
        mbedtls_ctr_drbg_free(&client->drbg);
    }
    if (client->entropy_initialized) {
        mbedtls_entropy_free(&client->entropy);
    }
    if (client->ssl_initialized) {
        mbedtls_ssl_free(&client->ssl);
    }
    if (client->conf_initialized) {
        mbedtls_ssl_config_free(&client->conf);
    }
    pthread_mutex_destroy(&client->operation_lock);
    close(client->cancel_read_fd);
    close(client->cancel_write_fd);
    if (client->restore_fd_flags) {
        (void) fcntl(client->fd, F_SETFL, client->original_fd_flags);
    }
    free(client);
}

const char *tls_client_protocol(const tls_client *client)
{
    if (client == NULL || !__atomic_load_n(&client->handshake_done, __ATOMIC_ACQUIRE)) {
        return NULL;
    }
    return mbedtls_ssl_get_version(&client->ssl);
}

const char *tls_client_cipher_suite(const tls_client *client)
{
    static _Thread_local char converted[128];
    const char *name;
    size_t index;
    size_t length;

    if (client == NULL || !__atomic_load_n(&client->handshake_done, __ATOMIC_ACQUIRE)) {
        return NULL;
    }
    name = mbedtls_ssl_get_ciphersuite(&client->ssl);
    if (name == NULL) {
        return NULL;
    }
    length = strlen(name);
    if (length >= sizeof(converted)) {
        return name;
    }
    for (index = 0; index <= length; ++index) {
        converted[index] = name[index] == '-' ? '_' : name[index];
    }
    return converted;
}

const char *tls_client_alpn_selected(const tls_client *client)
{
    if (client == NULL || !__atomic_load_n(&client->handshake_done, __ATOMIC_ACQUIRE)) {
        return NULL;
    }
    return mbedtls_ssl_get_alpn_protocol(&client->ssl);
}

size_t tls_client_peer_chain(const tls_client *client,
                             tls_cert_view *out, size_t max)
{
    size_t count;
    size_t copy_count;

    if (client == NULL) {
        return 0;
    }
    count = client->peer_chain_count;
    if (out == NULL || max == 0) {
        return count;
    }
    copy_count = count < max ? count : max;
    memcpy(out, client->peer_chain, copy_count * sizeof(*out));
    return count;
}

int tls_client_last_mbedtls_error(const tls_client *client)
{
    return client == NULL ? 0 : client->last_mbedtls_error;
}

int tls_client_last_errno(const tls_client *client)
{
    return client == NULL ? 0 : client->last_errno;
}

const char *tls_client_error_text(int code)
{
    switch (code) {
    case TLS_CLIENT_OK:
        return "ok";
    case TLS_CLIENT_ERR_ARGUMENT:
        return "invalid argument";
    case TLS_CLIENT_ERR_MEMORY:
        return "out of memory";
    case TLS_CLIENT_ERR_STATE:
        return "invalid client state";
    case TLS_CLIENT_ERR_IO:
        return "I/O error";
    case TLS_CLIENT_ERR_TIMEOUT:
        return "operation timed out";
    case TLS_CLIENT_ERR_PEER_CLOSED:
        return "peer closed the connection";
    case TLS_CLIENT_ERR_PROTOCOL:
        return "TLS protocol error";
    case TLS_CLIENT_ERR_TRUST_REJECTED:
        return "peer certificate rejected";
    case TLS_CLIENT_ERR_CANCELLED:
        return "operation cancelled";
    case TLS_CLIENT_ERR_INIT:
        return "TLS initialization failed";
    default:
        return "unknown TLS error";
    }
}
