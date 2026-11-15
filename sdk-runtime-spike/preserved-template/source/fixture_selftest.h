#ifndef TLS_TRUST_FIXTURE_SELFTEST_H
#define TLS_TRUST_FIXTURE_SELFTEST_H

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*TlsTrustLog)(const char *line, void *context);
/* Returns 0 only when all controlled positive/negative cases meet expectations.
 * Returns -2 when API 26 symbols are unavailable; never counts that as a pass.
 */
int tls_trust_selftest(TlsTrustLog log, void *context);

#ifdef __cplusplus
}
#endif
#endif
