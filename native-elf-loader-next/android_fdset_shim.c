#include "android_fdset_shim.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#if UINTPTR_MAX != UINT64_MAX
#error "android_fdset_shim supports AArch64/64-bit targets only"
#endif
_Static_assert(FD_SETSIZE == 1024, "unexpected fd_set capacity");
_Static_assert(sizeof(unsigned long) == 8, "AArch64 unsigned long must be 64-bit");
_Static_assert(sizeof(fd_set) == 128, "fd_set layout differs from bionic AArch64");

static void validate_fdset(const char *op, int fd, fd_set *set, size_t set_size)
{
    if (fd < 0) {
        fprintf(stderr, "FORTIFY: %s: file descriptor < 0\n", op);
        abort();
    }
    if (fd >= FD_SETSIZE) {
        fprintf(stderr, "FORTIFY: %s: file descriptor >= FD_SETSIZE\n", op);
        abort();
    }
    if (set_size < sizeof(fd_set)) {
        fprintf(stderr, "FORTIFY: %s: set is too small\n", op);
        abort();
    }
    if (set == NULL) {
        fprintf(stderr, "FORTIFY: %s: null fd_set\n", op);
        abort();
    }
}

void __FD_SET_chk(int fd, fd_set *set, size_t set_size)
{
    validate_fdset("FD_SET", fd, set, set_size);
    FD_SET(fd, set);
}

int __FD_ISSET_chk(int fd, fd_set *set, size_t set_size)
{
    validate_fdset("FD_ISSET", fd, set, set_size);
    return FD_ISSET(fd, set);
}
