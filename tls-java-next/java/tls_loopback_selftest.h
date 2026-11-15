#pragma once
#include "dvm/dvm_internal.h"

/* Controlled Java TLS loopback self-test.
 *
 * When built with -DLUNARIA_TLS_LOOPBACK_PORT=<port>, runs a real guest
 * SSLContext/TrustManagerFactory/SSLSocketFactory/SSLSocket handshake against
 * 127.0.0.1:<port> (expected to be forwarded to a host-local test TLS server),
 * verifies a banner read + request/echo exchange, and requires an unrelated CA
 * to be rejected with javax/net/ssl/SSLException.  Without the define it logs
 * SKIP and returns true so normal candidates keep their existing surface. */
bool tls_loopback_selftest(struct dvm *vm);
