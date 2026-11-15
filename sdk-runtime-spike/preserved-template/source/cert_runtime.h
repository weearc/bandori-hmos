/* Guest java.security certificate, key, principal, digest and KeyStore objects
 * backed by real DER parsed with Mbed TLS. Unknown algorithms, encodings and
 * methods fail explicitly; nothing is synthesized from absent data. */
#pragma once
#include "dvm/dvm_internal.h"

bool cert_runtime_call(struct dvm *, const char *, const char *, const char *, dvm_ref,
                       const union dvm_value *, int, union dvm_value *);
void cert_runtime_vm_start(struct dvm *);
bool cert_runtime_selftest(struct dvm *);

/* New guest X509Certificate holding a private copy of one complete DER
 * certificate. Returns 0 with a pending CertificateException (or
 * OutOfMemoryError) on failure. */
dvm_ref cert_runtime_new_x509(struct dvm *, const unsigned char *der, size_t length);
/* New guest X509Certificate[] in the given order. 0 with pending exception on failure. */
dvm_ref cert_runtime_new_x509_array(struct dvm *, const unsigned char *const *der,
                                    const size_t *length, size_t count);
/* DER owned by a runtime certificate object, or NULL when ref is not one. */
const unsigned char *cert_runtime_der(struct dvm *, dvm_ref certificate, size_t *length);
/* Trusted-certificate entries of a loaded runtime KeyStore, in alias order.
 * Views stay valid while the KeyStore is alive and unmodified. Returns the
 * total entry count, or -1 when ref is not a loaded runtime KeyStore. */
long cert_runtime_keystore_trusted(struct dvm *, dvm_ref keystore,
                                   const unsigned char **der, size_t *length, size_t max);
