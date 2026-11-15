#include "trust.h"

#include <stdio.h>
#include <stdlib.h>

static int read_der(const char *path, TlsTrustCert *cert)
{
    FILE *file = fopen(path, "rb");
    if (!file) return 0;
    if (fseek(file, 0, SEEK_END) || (long)(cert->size = (size_t)ftell(file)) <= 0 ||
        fseek(file, 0, SEEK_SET)) { fclose(file); return 0; }
    uint8_t *data = malloc(cert->size);
    if (!data) { fclose(file); return 0; }
    if (fread(data, 1, cert->size, file) != cert->size) {
        free(data);
        fclose(file);
        return 0;
    }
    fclose(file);
    cert->data = data;
    cert->encoding = TLS_TRUST_DER;
    return 1;
}

int main(int argc, char **argv)
{
    if (argc != 4) {
        fprintf(stderr, "usage: trust-selftest PEER.der CA.der|- HOSTNAME\n");
        return 2;
    }
    TlsTrustCert peer = {0}, ca = {0};
    if (!read_der(argv[1], &peer)) return 2;
    if (argv[2][0] != '-' || argv[2][1] != '\0') {
        if (!read_der(argv[2], &ca)) { free((void *)peer.data); return 2; }
    }
    TlsTrustChain chain = {0};
    int32_t result = tls_trust_verify(&peer, 1, ca.data ? &ca : NULL,
                                      argv[3], &chain);
    printf("available=%d result=%d sorted_count=%zu\n",
           tls_trust_available(), result, chain.count);
    tls_trust_free_chain(&chain);
    free((void *)peer.data);
    free((void *)ca.data);
    return result == 0 ? 0 : 1;
}
