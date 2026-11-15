#include "fixture_selftest.h"
#include "fixture_der.h"
#include "trust.h"

#include <stdio.h>

typedef struct {
    const char *name;
    const unsigned char *peer;
    size_t peer_size;
    const unsigned char *ca;
    size_t ca_size;
    const char *hostname;
    int should_succeed;
} Case;

int tls_trust_selftest(TlsTrustLog log, void *context)
{
    if (!tls_trust_available()) {
        if (log) log("trust: API26 chain verifier unavailable (dlopen/dlsym)", context);
        return TLS_TRUST_UNAVAILABLE;
    }
    const Case cases[] = {
        {"valid_explicit_ca", fixture_valid, sizeof fixture_valid,
         fixture_test_root, sizeof fixture_test_root, "local.test", 1},
        {"wrong_hostname", fixture_wrong_host, sizeof fixture_wrong_host,
         fixture_test_root, sizeof fixture_test_root, "local.test", 0},
        {"wrong_ca", fixture_wrong_ca, sizeof fixture_wrong_ca,
         fixture_test_root, sizeof fixture_test_root, "local.test", 0},
        {"wrong_root", fixture_valid, sizeof fixture_valid,
         fixture_wrong_root, sizeof fixture_wrong_root, "local.test", 0},
        {"expired", fixture_expired, sizeof fixture_expired,
         fixture_test_root, sizeof fixture_test_root, "local.test", 0},
        {"system_untrusted", fixture_valid, sizeof fixture_valid,
         NULL, 0, "local.test", 0},
    };
    int failures = 0;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        const Case *item = &cases[i];
        TlsTrustCert peer = {TLS_TRUST_DER, item->peer, item->peer_size};
        TlsTrustCert ca = {TLS_TRUST_DER, item->ca, item->ca_size};
        TlsTrustChain chain = {0};
        int32_t code = tls_trust_verify(&peer, 1, item->ca ? &ca : NULL,
                                        item->hostname, &chain);
        int passed = item->should_succeed ? (code == 0 && chain.count > 0) : (code > 0);
        if (!passed) ++failures;
        if (log) {
            char line[192];
            snprintf(line, sizeof line, "trust: %s code=%d chain_count=%zu %s",
                     item->name, code, chain.count, passed ? "PASS" : "FAIL");
            log(line, context);
        }
        tls_trust_free_chain(&chain);
    }
    return failures;
}
