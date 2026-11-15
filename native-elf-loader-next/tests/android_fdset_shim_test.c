#include "../android_fdset_shim.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    struct { uint64_t before; fd_set set; uint64_t after; } guarded;
    guarded.before=UINT64_C(0x123456789abcdef0);
    guarded.after=UINT64_C(0xfedcba9876543210);
#define wrapped guarded.set
    unsigned char *bytes=(unsigned char *)&wrapped;
    if (argc > 1) {
        FD_ZERO(&wrapped);
        if (!strcmp(argv[1], "negative")) __FD_SET_chk(-1,&wrapped,sizeof(wrapped));
        if (!strcmp(argv[1], "high")) __FD_SET_chk(FD_SETSIZE,&wrapped,sizeof(wrapped));
        if (!strcmp(argv[1], "short")) __FD_SET_chk(0,&wrapped,sizeof(wrapped)-1);
        if (!strcmp(argv[1], "isset-short")) (void)__FD_ISSET_chk(0,&wrapped,sizeof(wrapped)-1);
        return 0;
    }
    memset(bytes,0xA5,sizeof(wrapped));
    FD_ZERO(&wrapped);
    __FD_SET_chk(0,&wrapped,sizeof(wrapped));
    __FD_SET_chk(FD_SETSIZE-1,&wrapped,sizeof(wrapped));
    if (!__FD_ISSET_chk(0,&wrapped,sizeof(wrapped)) ||
        !__FD_ISSET_chk(FD_SETSIZE-1,&wrapped,sizeof(wrapped)) ||
        __FD_ISSET_chk(1,&wrapped,sizeof(wrapped))) return 1;
    if(guarded.before!=UINT64_C(0x123456789abcdef0)||guarded.after!=UINT64_C(0xfedcba9876543210))return 2;
    puts("FDSET_SHIM_TEST PASS boundary_0_1023=1 canary=1");
    return 0;
}
