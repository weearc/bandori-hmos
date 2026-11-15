#ifndef CERT_OBJECTS_H
#define CERT_OBJECTS_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct cert_object cert_object;
/* Parse one complete DER X.509 certificate. NULL means invalid input or allocation failure. */
cert_object *cert_object_parse(const unsigned char *der, size_t length);
void cert_object_free(cert_object *cert);
/* Returned bytes are owned by cert and valid until cert_object_free. */
const unsigned char *cert_object_der(const cert_object *cert, size_t *length);
const unsigned char *cert_object_spki(const cert_object *cert, size_t *length);
/* Canonical key algorithm name such as RSA or EC, or NULL when unavailable. */
const char *cert_object_public_key_algorithm(const cert_object *cert);
/* 0 on success; -1 on invalid arguments or hash failure. */
int cert_sha256(const unsigned char *input, size_t length, unsigned char output[32]);
int cert_sha1(const unsigned char *input, size_t length, unsigned char output[20]);
#ifdef __cplusplus
}
#endif
#endif
