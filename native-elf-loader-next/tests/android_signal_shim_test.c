#define _POSIX_C_SOURCE 200809L
#include "../android_signal_shim.h"
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct { uint32_t flags,pad; uint64_t handler,mask,restorer; } guest_action;
typedef struct { uint64_t before,value,after; } guarded_set;
typedef struct { uint64_t before; guest_action value; uint64_t after; } guarded_action;
static volatile sig_atomic_t received;
static void handler(int sig){if(sig==SIGUSR1)++received;}
#define CHECK(c) do {if(!(c)){fprintf(stderr,"signal test failed at line %d errno=%d\n",__LINE__,errno);goto cleanup;}} while(0)
int main(void)
{
    int ok=0;sigset_t saved_mask;struct sigaction saved_action;
    if(pthread_sigmask(SIG_SETMASK,NULL,&saved_mask) || sigaction(SIGUSR1,NULL,&saved_action))return 1;
    guarded_set set={UINT64_C(0x1122334455667788),0,UINT64_C(0x8877665544332211)};
    guarded_set old=set; guarded_action old_action={0};
    old_action.before=UINT64_C(0x1234567890abcdef);old_action.after=UINT64_C(0xfedcba0987654321);
    CHECK(android_signal_sigemptyset(&set.value)==0);
    CHECK(android_signal_sigaddset(&set.value,SIGUSR1)==0);
    CHECK(set.value==(UINT64_C(1)<<(SIGUSR1-1)));
    CHECK(set.before==UINT64_C(0x1122334455667788)&&set.after==UINT64_C(0x8877665544332211));
    errno=EDOM;CHECK(android_signal_pthread_sigmask(0,&set.value,&old.value)==0 && errno==EDOM);
    CHECK(old.before==UINT64_C(0x1122334455667788)&&old.after==UINT64_C(0x8877665544332211));
    guest_action act={0};void (*fn)(int)=handler;uintptr_t p=0;memcpy(&p,&fn,sizeof(p));act.handler=(uint64_t)p;
    act.flags=UINT32_C(0x10000000);act.mask=set.value;
    CHECK(android_signal_sigaction(SIGUSR1,&act,&old_action.value)==0);
    CHECK(old_action.before==UINT64_C(0x1234567890abcdef)&&old_action.after==UINT64_C(0xfedcba0987654321));
    guest_action query={0};CHECK(android_signal_sigaction(SIGUSR1,NULL,&query)==0);
    CHECK(query.handler==act.handler && (query.flags&UINT32_C(0x10000000)) && (query.mask&set.value));
    CHECK(raise(SIGUSR1)==0 && received==0);
    CHECK(android_signal_pthread_sigmask(1,&set.value,NULL)==0 && received==1);
    guest_action unsupported=act;unsupported.flags=4;errno=0;
    CHECK(android_signal_sigaction(SIGUSR1,&unsupported,NULL)==-1 && errno==ENOTSUP);
    guest_action after={0};CHECK(android_signal_sigaction(SIGUSR1,NULL,&after)==0 && after.handler==act.handler);
    errno=0;CHECK(android_signal_sigaddset(&set.value,65)==-1 && errno==EINVAL);
    errno=EDOM;CHECK(android_signal_pthread_sigmask(99,&set.value,NULL)==EINVAL && errno==EDOM);
    CHECK(android_signal_pthread_sigmask(99,NULL,&old.value)==0 && errno==EDOM);
    ok=1;
cleanup:
    sigaction(SIGUSR1,&saved_action,NULL);
    pthread_sigmask(SIG_SETMASK,&saved_mask,NULL);
    if(ok)puts("ANDROID_SIGNAL_SHIM_TEST PASS canaries=1 mask=1 handler=1 oldact=1 siginfo_rejected=1");
    return ok?0:1;
}
