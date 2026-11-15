#include "trust.h"

#include <dlfcn.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <network/netstack/net_ssl/net_ssl_c_type.h>

typedef uint32_t (*VerifyFn)(const struct NetStack_CertBlob *, size_t,
                             const struct NetStack_CertBlob *, const char *,
                             struct NetStack_CertBlob **, size_t *);
typedef void (*FreeFn)(struct NetStack_CertBlob *, size_t);

static int load_api(void **library, VerifyFn *verify, FreeFn *release)
{
    *library = dlopen("libnet_ssl.so", RTLD_NOW | RTLD_LOCAL);
    if (!*library) return 0;
    *verify = (VerifyFn)dlsym(*library, "OH_NetStack_CreateAndVerifySortedCertChain");
    *release = (FreeFn)dlsym(*library, "OH_NetStack_FreeCertChain");
    if (*verify && *release) return 1;
    dlclose(*library);
    *library = NULL;
    return 0;
}

int tls_trust_available(void)
{
    void *library = NULL;
    VerifyFn verify = NULL;
    FreeFn release = NULL;
    int found = load_api(&library, &verify, &release);
    if (library) dlclose(library);
    return found;
}

void tls_trust_free_chain(TlsTrustChain *chain)
{
    if (!chain) return;
    for (size_t i = 0; i < chain->count; ++i) free(chain->certs[i].data);
    free(chain->certs);
    chain->certs = NULL;
    chain->count = 0;
}

static int valid_cert(const TlsTrustCert *cert)
{
    return cert && cert->data && cert->size && cert->size <= UINT32_MAX &&
           (cert->encoding == TLS_TRUST_DER || cert->encoding == TLS_TRUST_PEM);
}

int32_t tls_trust_verify(const TlsTrustCert *peer, size_t peer_count,
                         const TlsTrustCert *ca, const char *hostname,
                         TlsTrustChain *verified)
{
    if (!verified) return TLS_TRUST_BAD_ARGUMENT;
    verified->certs = NULL;
    verified->count = 0;
    if (!peer || !peer_count || peer_count > 32 || !hostname || !*hostname ||
        strlen(hostname) > 253 || (ca && !valid_cert(ca))) return TLS_TRUST_BAD_ARGUMENT;
    for (size_t i = 0; i < peer_count; ++i)
        if (!valid_cert(&peer[i])) return TLS_TRUST_BAD_ARGUMENT;

    void *library = NULL;
    VerifyFn verify = NULL;
    FreeFn release = NULL;
    if (!load_api(&library, &verify, &release)) return TLS_TRUST_UNAVAILABLE;

    struct NetStack_CertBlob blobs[32];
    for (size_t i = 0; i < peer_count; ++i) {
        blobs[i].type = (enum NetStack_CertType)peer[i].encoding;
        blobs[i].size = (uint32_t)peer[i].size;
        blobs[i].data = (uint8_t *)peer[i].data;
    }
    struct NetStack_CertBlob ca_blob;
    if (ca) {
        ca_blob.type = (enum NetStack_CertType)ca->encoding;
        ca_blob.size = (uint32_t)ca->size;
        ca_blob.data = (uint8_t *)ca->data;
    }
    struct NetStack_CertBlob *sorted = NULL;
    size_t count = 0;
    uint32_t status = verify(blobs, peer_count, ca ? &ca_blob : NULL,
                             hostname, &sorted, &count);
    if (status) {
        if (sorted) release(sorted, count);
        dlclose(library);
        return (int32_t)status;
    }
    if (!sorted || !count || count > 64) {
        if (sorted) release(sorted, count);
        dlclose(library);
        return TLS_TRUST_BAD_PLATFORM_OUTPUT;
    }
    TlsTrustOwnedCert *copy = calloc(count, sizeof(*copy));
    if (!copy) {
        release(sorted, count);
        dlclose(library);
        return TLS_TRUST_NO_MEMORY;
    }
    int32_t result = 0;
    for (size_t i = 0; i < count; ++i) {
        if (!sorted[i].data || !sorted[i].size ||
            (sorted[i].type != NETSTACK_CERT_TYPE_DER && sorted[i].type != NETSTACK_CERT_TYPE_PEM)) {
            result = TLS_TRUST_BAD_PLATFORM_OUTPUT;
            break;
        }
        copy[i].data = malloc(sorted[i].size);
        if (!copy[i].data) { result = TLS_TRUST_NO_MEMORY; break; }
        memcpy(copy[i].data, sorted[i].data, sorted[i].size);
        copy[i].size = sorted[i].size;
        copy[i].encoding = (TlsTrustEncoding)sorted[i].type;
    }
    release(sorted, count);
    dlclose(library);
    if (result) {
        TlsTrustChain failed = {copy, count};
        tls_trust_free_chain(&failed);
        return result;
    }
    verified->certs = copy;
    verified->count = count;
    return 0;
}
