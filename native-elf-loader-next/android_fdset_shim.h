#ifndef ANDROID_FDSET_SHIM_H
#define ANDROID_FDSET_SHIM_H

#include <stddef.h>
#include <sys/select.h>

#ifdef __cplusplus
extern "C" {
#endif

void __FD_SET_chk(int fd, fd_set *set, size_t set_size);
int __FD_ISSET_chk(int fd, fd_set *set, size_t set_size);

#ifdef __cplusplus
}
#endif
#endif
