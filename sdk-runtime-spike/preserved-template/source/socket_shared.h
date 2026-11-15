#pragma once
#include "dvm/dvm_internal.h"
/* Shared plain-socket state for layered sockets (javax.net.ssl over an
 * existing java.net.Socket). Every function requires the interpreter lock. */
struct socket_state;

/* Retains the state of a connected runtime java.net.Socket, else NULL. */
struct socket_state *socket_runtime_retain(struct dvm *, dvm_ref socket);
/* Retains an unconnected runtime socket for a connect in progress. */
struct socket_state *socket_runtime_retain_for_connect(struct dvm *, dvm_ref socket);
void socket_state_release(struct socket_state *);
/* Descriptor for one blocking call made with the lock released, or -1 when
 * closed. Always pair a non-negative result with socket_state_io_end. */
int socket_state_io_begin(struct socket_state *);
void socket_state_io_end(struct socket_state *);
/* Shuts the descriptor down at once, waking blocked calls; the descriptor
 * itself is closed when the last blocked call has returned. */
void socket_state_close(struct socket_state *);
bool socket_state_closed(const struct socket_state *);
bool socket_state_connected(const struct socket_state *);
int socket_state_timeout(const struct socket_state *);
void socket_state_set_timeout(struct socket_state *, int timeout_ms);
int socket_state_port(const struct socket_state *);
/* Remote InetAddress as a new guest object (0 with pending exception on failure). */
dvm_ref socket_state_remote_address(struct dvm *, struct socket_state *);
int socket_state_local_port(const struct socket_state *);
/* Non-loopback connections are refused unless explicitly enabled. */
void socket_runtime_set_external(bool allowed);
bool socket_runtime_external_allowed(void);
