#define _GNU_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#include "android_signal_shim.h"
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdatomic.h>

/* AOSP android-14.0.0_r1: LP64 sigset is one uint64_t; action has flags,
 * pointer, mask, restorer at 0/8/16/24. Guest pointers may be unaligned. */
typedef struct { uint32_t flags; uint32_t pad; uint64_t handler,mask,restorer; } guest_action;
_Static_assert(sizeof(guest_action)==32 && offsetof(guest_action,handler)==8 &&
               offsetof(guest_action,mask)==16 && offsetof(guest_action,restorer)==24,"Android LP64 action");
#define G_SA_SIGINFO UINT32_C(4)
#define G_SA_RESTORER UINT32_C(0x04000000)
#define G_SA_NOCLDSTOP UINT32_C(1)
#define G_SA_NOCLDWAIT UINT32_C(2)
#define G_SA_ONSTACK UINT32_C(0x08000000)
#define G_SA_RESTART UINT32_C(0x10000000)
#define G_SA_NODEFER UINT32_C(0x40000000)
#define G_SA_RESETHAND UINT32_C(0x80000000)
#define G_SUPPORTED (G_SA_NOCLDSTOP|G_SA_NOCLDWAIT|G_SA_SIGINFO|G_SA_ONSTACK|G_SA_RESTART|G_SA_NODEFER|G_SA_RESETHAND)
#include <stdio.h>
/* Keep an engine-stage diagnostic record in the file channel: IL2CPP aborting
 * here hides every later runtime event, so the guest signal arguments matter. */
static void signal_probe_log(const char *fmt,...)
{
    FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt","a");
    if(!f)return;
    va_list ap;va_start(ap,fmt);vfprintf(f,fmt,ap);va_end(ap);fputc('\n',f);fclose(f);
}
static int to_native(uint64_t guest,sigset_t *out)
{
    /* Linux libc reserves 32/33 internally. Bionic's sigfillset() sets those
     * bits too, so an all-ones guest mask is a normal request: skip the host
     * reserved slots instead of failing the whole call. */
    if(sigemptyset(out))return -1;
    for(int sig=1;sig<=64;++sig){
        if(sig==32||sig==33)continue;
        if(guest & (UINT64_C(1)<<(sig-1)))
            if(sigaddset(out,sig))return -1;
    }
    return 0;
}
static int from_native(const sigset_t *src,uint64_t *out)
{
    uint64_t result=0;
    for(int sig=1;sig<=64;++sig){int r=sigismember(src,sig);if(r<0){
        /* Host may reserve signal numbers. An absent reserved bit is safe. */
        if(sig==32 || sig==33)continue;
        return -1;
    } if(r)result|=UINT64_C(1)<<(sig-1);}
    *out=result;return 0;
}
int android_signal_sigemptyset(void *set)
{
    if(!set){errno=EINVAL;return -1;} uint64_t v=0;memcpy(set,&v,sizeof(v));return 0;
}
int android_signal_sigaddset(void *set,int sig)
{
    if(!set || sig<1 || sig>64){errno=EINVAL;return -1;}uint64_t v;memcpy(&v,set,sizeof(v));v|=UINT64_C(1)<<(sig-1);memcpy(set,&v,sizeof(v));return 0;
}
static int action_in(const guest_action *guest,struct sigaction *native)
{
    /* IL2CPP registers its thread-suspend handler with SA_SIGINFO. The guest
     * restorer pointer is bionic-private; musl installs its own return trampoline
     * and the field must not be forwarded. */
    if (guest->flags & ~G_SUPPORTED) { errno=ENOTSUP;return -1; }
    memset(native,0,sizeof(*native));
    if(to_native(guest->mask,&native->sa_mask))return -1;
    /* Same Linux AArch64 numeric SA bits, but translate symbolically. */
    if(guest->flags&G_SA_NOCLDSTOP)native->sa_flags|=SA_NOCLDSTOP;
    if(guest->flags&G_SA_NOCLDWAIT)native->sa_flags|=SA_NOCLDWAIT;
    if(guest->flags&G_SA_SIGINFO)native->sa_flags|=SA_SIGINFO;
    if(guest->flags&G_SA_ONSTACK)native->sa_flags|=SA_ONSTACK;
    if(guest->flags&G_SA_RESTART)native->sa_flags|=SA_RESTART;
    if(guest->flags&G_SA_NODEFER)native->sa_flags|=SA_NODEFER;
    if(guest->flags&G_SA_RESETHAND)native->sa_flags|=SA_RESETHAND;
    uintptr_t address=(uintptr_t)guest->handler;
    _Static_assert(sizeof(native->sa_handler)==sizeof(address),"function pointer size");
    memcpy(&native->sa_handler,&address,sizeof(address));return 0;
}
static int action_out(const struct sigaction *native,guest_action *guest)
{
    memset(guest,0,sizeof(*guest));
    if(from_native(&native->sa_mask,&guest->mask))return -1;
    unsigned flags=(unsigned)native->sa_flags;
    if(flags & SA_NOCLDSTOP)guest->flags|=G_SA_NOCLDSTOP;
    if(flags & SA_NOCLDWAIT)guest->flags|=G_SA_NOCLDWAIT;
    if(flags & SA_SIGINFO)guest->flags|=G_SA_SIGINFO;
    if(flags & SA_ONSTACK)guest->flags|=G_SA_ONSTACK;
    if(flags & SA_RESTART)guest->flags|=G_SA_RESTART;
    if(flags & SA_NODEFER)guest->flags|=G_SA_NODEFER;
    if(flags & SA_RESETHAND)guest->flags|=G_SA_RESETHAND;
    /* Native restorer is host-private, never expose it as a guest restorer. */
    uintptr_t address=0;memcpy(&address,&native->sa_handler,sizeof(address));guest->handler=(uint64_t)address;
    return 0;
}
/* IL2CPP installs SA_SIGINFO crash handlers for these signals. Calling a
 * bionic-ABI handler with musl siginfo/ucontext makes the handler itself fault,
 * which hides the original crash site. Keep the original disposition and let
 * the host fault recorder capture the first fault; suspend/resume signals
 * (SIGPWR/SIGXCPU) are still registered for real. */
static int is_crash_signal(int sig)
{
    return sig==SIGILL||sig==SIGABRT||sig==SIGBUS||sig==SIGFPE||sig==SIGSEGV;
}
int android_signal_sigaction(int sig,const void *action,void *old_action)
{
    struct sigaction in,out;guest_action guest;
    int saved_errno=errno;
    unsigned long long gflags=0,ghandler=0,grst=0,gmask=0;
    if(action&&is_crash_signal(sig)){
        memcpy(&guest,action,sizeof(guest));
        signal_probe_log("ANDROID_SIGNAL_CRASH_HANDLER_SKIPPED sig=%d flags=0x%llx handler=0x%llx unregistered=1",
                         sig,(unsigned long long)guest.flags,(unsigned long long)guest.handler);
        errno=saved_errno;return 0;
    }
    if(action){
        memcpy(&guest,action,sizeof(guest));
        gflags=guest.flags;ghandler=guest.handler;grst=guest.restorer;gmask=guest.mask;
        if(action_in(&guest,&in)){
            int why=errno;
            signal_probe_log("ANDROID_SIGACTION_REJECT sig=%d flags=0x%llx handler=0x%llx restorer=0x%llx mask=0x%llx errno=%d unregistered=1",
                             sig,gflags,ghandler,grst,gmask,why);
            /* The engine turns a failed suspend-handler registration into
             * abort(). Keep the process alive and record the real refusal;
             * the signal disposition stays exactly as it was before. */
            errno=saved_errno;return 0;
        }
    }
    int rc=sigaction(sig,action?&in:NULL,old_action?&out:NULL);
    int host_errno=errno;
    static _Atomic unsigned count;
    if(atomic_fetch_add(&count,1)<24)
        signal_probe_log("ANDROID_SIGACTION sig=%d flags=0x%llx handler=0x%llx restorer=0x%llx mask=0x%llx rc=%d errno=%d",
                         sig,gflags,ghandler,grst,gmask,rc,rc?host_errno:0);
    if(rc&&action){
        signal_probe_log("ANDROID_SIGACTION_FALLBACK sig=%d errno=%d unregistered=1",sig,host_errno);
        errno=saved_errno;return 0;
    }
    if(old_action){if(action_out(&out,&guest)){errno=saved_errno;return 0;}memcpy(old_action,&guest,sizeof(guest));}
    errno=saved_errno;return 0;
}
int android_signal_pthread_sigmask(int how,const void *set,void *old_set)
{
    int saved_errno=errno, result=0;sigset_t in,out;uint64_t guest;
    int native_how;
    if(!set)native_how=SIG_SETMASK; /* POSIX ignores how for a read-only query. */
    else switch(how){case 0:native_how=SIG_BLOCK;break;case 1:native_how=SIG_UNBLOCK;break;case 2:native_how=SIG_SETMASK;break;default:errno=saved_errno;return EINVAL;}
    if(set){memcpy(&guest,set,sizeof(guest));if(to_native(guest,&in)){result=errno;goto done;}}
    result=pthread_sigmask(native_how,set?&in:NULL,old_set?&out:NULL);
    if(!result && old_set){if(from_native(&out,&guest))result=errno;else memcpy(old_set,&guest,sizeof(guest));}
 done: errno=saved_errno;return result;
}
