#ifndef SOCKET_COUNTERS_H
#define SOCKET_COUNTERS_H
#include <stdint.h>
#include <stddef.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/types.h>
struct addrinfo;
/* Tokens prevent calls spanning an off/on transition entering the new session. */
struct socket_counter_stamp { uint64_t ns, session; };
struct socket_counter_stamp socket_counter_begin(void);
void socket_counter_end(struct socket_counter_stamp, const char *, int, int, int64_t, int);
int socket_counter_socket(int, int, int);
int socket_counter_connect(int, const struct sockaddr *, socklen_t);
int socket_counter_getsockopt(int, int, int, void *, socklen_t *);
int socket_counter_getaddrinfo(const char *, const char *, const struct addrinfo *, struct addrinfo **);
ssize_t socket_counter_send(int, const void *, size_t, int);
ssize_t socket_counter_recv(int, void *, size_t, int);
ssize_t socket_counter_sendto(int, const void *, size_t, int, const struct sockaddr *, socklen_t);
ssize_t socket_counter_recvfrom(int, void *, size_t, int, struct sockaddr *, socklen_t *);
ssize_t socket_counter_sendmsg(int, const struct msghdr *, int);
ssize_t socket_counter_recvmsg(int, struct msghdr *, int);
ssize_t socket_counter_read(int, void *, size_t);
ssize_t socket_counter_write(int, const void *, size_t);
ssize_t socket_counter_readv(int, const struct iovec *, int);
ssize_t socket_counter_writev(int, const struct iovec *, int);
/* Used by connect implementations whose completion includes poll/timeout. */
void socket_counter_connect_result(struct socket_counter_stamp, int fd, int error);
#endif
