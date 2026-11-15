#ifndef TLS_TRUST_NEXT_H
#define TLS_TRUST_NEXT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TLS_TRUST_PEM = 0,
    TLS_TRUST_DER = 1
} TlsTrustEncoding;

typedef struct {
    TlsTrustEncoding encoding;
    const uint8_t *data;
    size_t size;
} TlsTrustCert;

typedef struct {
    uint8_t *data;
    size_t size;
    TlsTrustEncoding encoding;
} TlsTrustOwnedCert;

typedef struct {
    TlsTrustOwnedCert *certs;
    size_t count;
} TlsTrustChain;

/* Negative values are local errors; positive values are unchanged NetStack errors. */
enum {
    TLS_TRUST_BAD_ARGUMENT = -1,
    TLS_TRUST_UNAVAILABLE = -2,
    TLS_TRUST_NO_MEMORY = -3,
    TLS_TRUST_BAD_PLATFORM_OUTPUT = -4
};

/* ca == NULL selects platform system anchors. Otherwise ca is one explicit CA.
 * hostname must be the actual requested DNS name/IP, never an inferred name.
 * A zero return guarantees a nonempty owned sorted chain; release with free_chain.
 */
int32_t tls_trust_verify(const TlsTrustCert *peer, size_t peer_count,
                         const TlsTrustCert *ca, const char *hostname,
                         TlsTrustChain *verified);
void tls_trust_free_chain(TlsTrustChain *chain);
int tls_trust_available(void);

#ifdef __cplusplus
}
#endif
#endif
