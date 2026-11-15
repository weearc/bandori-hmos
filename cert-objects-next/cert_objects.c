#include "cert_objects.h"
#include <stdlib.h>
#include <string.h>
#include "mbedtls/x509_crt.h"
#include "mbedtls/pk.h"
#include "mbedtls/sha256.h"
#include "mbedtls/sha1.h"

struct cert_object { mbedtls_x509_crt parsed; };

cert_object *cert_object_parse(const unsigned char *der, size_t length) {
    if (!der || !length) return NULL;
    cert_object *cert = (cert_object *)calloc(1, sizeof(*cert));
    if (!cert) return NULL;
    mbedtls_x509_crt_init(&cert->parsed);
    if (mbedtls_x509_crt_parse_der(&cert->parsed, der, length) != 0 ||
        cert->parsed.raw.len != length || !cert->parsed.pk_raw.p || !cert->parsed.pk_raw.len) {
        cert_object_free(cert);
        return NULL;
    }
    return cert;
}

void cert_object_free(cert_object *cert) {
    if (!cert) return;
    mbedtls_x509_crt_free(&cert->parsed);
    free(cert);
}

const unsigned char *cert_object_der(const cert_object *cert, size_t *length) {
    if (length) *length = cert ? cert->parsed.raw.len : 0;
    return cert ? cert->parsed.raw.p : NULL;
}

const unsigned char *cert_object_spki(const cert_object *cert, size_t *length) {
    if (length) *length = cert ? cert->parsed.pk_raw.len : 0;
    return cert ? cert->parsed.pk_raw.p : NULL;
}

const char *cert_object_public_key_algorithm(const cert_object *cert) {
    mbedtls_pk_type_t type;
    if (!cert) return NULL;
    type = mbedtls_pk_get_type(&cert->parsed.pk);
    if (type == MBEDTLS_PK_RSA) return "RSA";
    if (type == MBEDTLS_PK_ECKEY || type == MBEDTLS_PK_ECDSA ||
        type == MBEDTLS_PK_ECKEY_DH) return "EC";
    return NULL;
}

int cert_sha256(const unsigned char *input, size_t length, unsigned char output[32]) {
    if (!output || (!input && length)) return -1;
    return mbedtls_sha256(input ? input : (const unsigned char *)"", length, output, 0) == 0 ? 0 : -1;
}

int cert_sha1(const unsigned char *input, size_t length, unsigned char output[20]) {
    if (!output || (!input && length)) return -1;
    return mbedtls_sha1(input ? input : (const unsigned char *)"", length, output) == 0 ? 0 : -1;
}
