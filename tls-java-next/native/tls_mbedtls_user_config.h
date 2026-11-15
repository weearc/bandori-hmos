#ifndef TLS_MBEDTLS_USER_CONFIG_H
#define TLS_MBEDTLS_USER_CONFIG_H

/* Keep the upstream configuration and only add the platform threading backend. */
#define MBEDTLS_THREADING_C
#define MBEDTLS_THREADING_PTHREAD

#endif /* TLS_MBEDTLS_USER_CONFIG_H */
