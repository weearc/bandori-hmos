#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/version.h"
int tls_smoke(void) { mbedtls_ssl_context c; mbedtls_x509_crt x; mbedtls_ssl_init(&c); mbedtls_x509_crt_init(&x); mbedtls_x509_crt_free(&x); mbedtls_ssl_free(&c); return (int)mbedtls_version_get_number(); }
