#define _GNU_SOURCE
#include "native_fault_probe.h"
#include <signal.h>
#include <ucontext.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <dlfcn.h>

#if defined(__aarch64__)
static const int signals[] = {SIGSEGV,SIGBUS,SIGILL,SIGABRT};
static struct sigaction previous[4];
static int logfd=-1;
static int started;
static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
static void putstr(char *b, unsigned *n, const char *s) { while(*s)b[(*n)++]=*s++; }
static void puthex(char *b, unsigned *n, uint64_t x) {
    static const char digits[]="0123456789abcdef";
    putstr(b,n,"0x"); for(int i=15;i>=0;--i)b[(*n)++]=digits[(x>>(i*4))&15];
}
static void putdec(char *b, unsigned *n, uint64_t v) {
    char t[20]; unsigned k=0;
    if(!v)t[k++]='0';
    while(v){t[k++]=(char)('0'+(unsigned)(v%10u));v/=10u;}
    while(k)b[(*n)++]=t[--k];
}
#define BT_MAX_FRAMES 8u
#define BT_SPAN_BYTES (8ull*1024ull*1024ull)
/* Read [fp] and [fp+8] without faulting the signal thread when possible.
   process_vm_readv on self reports EFAULT for bad memory instead of raising a
   nested SIGSEGV; a direct volatile load is used only when the syscall itself
   is unavailable (ENOSYS/EPERM), and only after the caller's range checks. */
static int bt_read_pair(uint64_t fp,uint64_t *prev,uint64_t *ret) {
#if defined(SYS_process_vm_readv)
    struct iovec local[2],remote[2];
    local[0].iov_base=(void *)prev; local[0].iov_len=sizeof(*prev);
    local[1].iov_base=(void *)ret;  local[1].iov_len=sizeof(*ret);
    remote[0].iov_base=(void *)(uintptr_t)fp; remote[0].iov_len=sizeof(uint64_t);
    remote[1].iov_base=(void *)(uintptr_t)(fp+8); remote[1].iov_len=sizeof(uint64_t);
    long r=syscall(SYS_process_vm_readv,syscall(SYS_getpid),local,2UL,remote,2UL,0UL);
    if(r==(long)(2*sizeof(uint64_t)))return 0;
    if(r>=0)return -1;                         /* partial read: stop, do not guess */
    if(errno!=ENOSYS&&errno!=EPERM)return -1;  /* EFAULT etc: never blind-read */
#endif
    { volatile const uint64_t *p=(volatile const uint64_t *)(uintptr_t)fp;
      *prev=p[0]; *ret=p[1]; }
    return 0;
}
static void put_bt_frame(char *b,unsigned *n,unsigned idx,uint64_t pc) {
    putstr(b,n," bt"); b[(*n)++]=(char)('0'+(idx%10u)); b[(*n)++]='='; puthex(b,n,pc);
}
/* Conservative x29 chain: bounded frame count, stack-window and alignment
   checks, non-zero/equal rejection, and one direction per chain. Any violation
   stops immediately; already recovered frames are still emitted. */
static void put_bt(char *b,unsigned *n,uint64_t sp,uint64_t fp) {
    const char *stop=NULL; unsigned frames=0; int dir=0;
    uint64_t limit=sp+BT_SPAN_BYTES;
    if(limit<sp)limit=~0ull;
    if(fp==0)stop="zero";
    else if(sp==0||(fp&7u)!=0||fp<sp||fp>=limit)stop="range";
    while(!stop&&frames<BT_MAX_FRAMES) {
        uint64_t prev=0,ret=0;
        if(bt_read_pair(fp,&prev,&ret)!=0){stop="read";break;}
        if(ret==0){stop="zero";break;}
        put_bt_frame(b,n,frames,ret); ++frames;
        if(prev==0){stop="zero";break;}
        if((prev&7u)!=0||prev<sp||prev>=limit){stop="range";break;}
        if(prev==fp){stop="zero";break;}
        int nd=(prev>fp)?1:-1;
        if(dir==0)dir=nd; else if(nd!=dir){stop="order";break;}
        fp=prev;
    }
    if(stop){putstr(b,n," bt_stop=");putstr(b,n,stop);}
}
/* Read a bounded range without faulting the signal thread. process_vm_readv
   reports EFAULT for unmapped pages; the direct fallback is used only when the
   syscall itself is unavailable. The interrupted stack above SP must be mapped,
   but the LR may be stale, so its code bytes are read through the same guard. */
static int safe_read_range(uint64_t addr, void *dst, size_t len) {
#if defined(SYS_process_vm_readv)
    struct iovec local[1], remote[1];
    local[0].iov_base = dst; local[0].iov_len = len;
    remote[0].iov_base = (void *)(uintptr_t)addr; remote[0].iov_len = len;
    long r = syscall(SYS_process_vm_readv, syscall(SYS_getpid), local, 1UL, remote, 1UL, 0UL);
    if (r == (long)len) return 0;
    if (r >= 0) return -1;
    if (errno != ENOSYS && errno != EPERM) return -1;
#endif
    { volatile const unsigned char *p = (volatile const unsigned char *)(uintptr_t)addr;
      unsigned char *d = (unsigned char *)dst;
      for (size_t i = 0; i < len; ++i) d[i] = p[i]; }
    return 0;
}
#define STACK_DUMP_QWORDS 64u
/* Raw caller-frame window: saved return addresses survive here even when the
   x29 chain is unusable (Unity code is built without frame pointers). */
static void put_stack_dump(char *b, unsigned *n, uint64_t sp) {
    static const char digits[] = "0123456789abcdef";
    uint64_t w[STACK_DUMP_QWORDS];
    putstr(b,n,"stack sp="); puthex(b,n,sp);
    if (!sp) { putstr(b,n," read=no-sp\n"); return; }
    if (safe_read_range(sp, w, sizeof w) != 0) { putstr(b,n," read=fail\n"); return; }
    putstr(b,n," words=");
    for (unsigned i = 0; i < STACK_DUMP_QWORDS; ++i) {
        if (i) b[(*n)++] = ' ';
        for (int k = 15; k >= 0; --k) b[(*n)++] = digits[(w[i] >> (k * 4)) & 15];
    }
    b[(*n)++] = '\n';
}
/* Bytes around LR let the host distinguish a real call return from a stale or
   clobbered x30 value without re-deriving the runtime mapping. */
static void put_code_dump(char *b, unsigned *n, uint64_t lr) {
    static const char digits[] = "0123456789abcdef";
    unsigned char c[32];
    uint64_t start = lr >= 16 ? lr - 16 : 0;
    start &= ~3ull;
    putstr(b,n,"code lr="); puthex(b,n,lr);
    if (!start || safe_read_range(start, c, sizeof c) != 0) { putstr(b,n," read=fail\n"); return; }
    putstr(b,n," bytes=");
    for (unsigned i = 0; i < sizeof c; ++i) {
        b[(*n)++] = digits[c[i] >> 4];
        b[(*n)++] = digits[c[i] & 15];
    }
    b[(*n)++] = '\n';
}
#define OBJECT_DUMP_BYTES 0x40u
/* x19 is callee-saved and, in the PlayerPrefs log-formatter frame, holds the
   log context pointer (the helper copies x0 there).  Dump the object and the
   qword at +0x10 (its message/data pointer) so the empty-string source is
   visible without a live debugger.  Unmapped addresses fail closed. */
static void put_object_dump(char *b, unsigned *n, uint64_t obj) {
    static const char digits[] = "0123456789abcdef";
    unsigned char c[OBJECT_DUMP_BYTES];
    putstr(b,n,"object x19="); puthex(b,n,obj);
    if (!obj || safe_read_range(obj, c, sizeof c) != 0) {
        putstr(b,n," read=fail\n"); return;
    }
    putstr(b,n," bytes=");
    for (unsigned i = 0; i < sizeof c; ++i) {
        b[(*n)++] = digits[c[i] >> 4];
        b[(*n)++] = digits[c[i] & 15];
    }
    b[(*n)++] = '\n';
    uint64_t p = 0;
    for (int i = 0; i < 8; ++i)
        p |= (uint64_t)c[0x10 + i] << (i * 8);
    unsigned char c2[OBJECT_DUMP_BYTES];
    putstr(b,n,"object_data="); puthex(b,n,p);
    if (p && safe_read_range(p, c2, sizeof c2) == 0) {
        putstr(b,n," bytes=");
        for (unsigned i = 0; i < sizeof c2; ++i) {
            b[(*n)++] = digits[c2[i] >> 4];
            b[(*n)++] = digits[c2[i] & 15];
        }
    } else {
        putstr(b,n," read=fail");
    }
    b[(*n)++] = '\n';
}
static void fault_handler(int sig,siginfo_t *info,void *context) {
    char b[4096]; unsigned n=0; ucontext_t *u=(ucontext_t *)context;
    putstr(b,&n,"fatal signal=");puthex(b,&n,(uint64_t)(unsigned)sig);
    putstr(b,&n," code=");puthex(b,&n,(uint64_t)(int64_t)(info?info->si_code:0));
    putstr(b,&n," addr=");puthex(b,&n,(uint64_t)(uintptr_t)(info?info->si_addr:NULL));
    putstr(b,&n," pc=");puthex(b,&n,u?u->uc_mcontext.pc:0);
    putstr(b,&n," sp=");puthex(b,&n,u?u->uc_mcontext.sp:0);
    putstr(b,&n," lr=");puthex(b,&n,u?u->uc_mcontext.regs[30]:0);
    putstr(b,&n," x0=");puthex(b,&n,u?u->uc_mcontext.regs[0]:0);
    putstr(b,&n," x1=");puthex(b,&n,u?u->uc_mcontext.regs[1]:0);
    putstr(b,&n," x2=");puthex(b,&n,u?u->uc_mcontext.regs[2]:0);
    putstr(b,&n," x3=");puthex(b,&n,u?u->uc_mcontext.regs[3]:0);
    putstr(b,&n," x21=");puthex(b,&n,u?u->uc_mcontext.regs[21]:0);
    putstr(b,&n," x24=");puthex(b,&n,u?u->uc_mcontext.regs[24]:0);
    putstr(b,&n," x29=");puthex(b,&n,u?u->uc_mcontext.regs[29]:0);
    putstr(b,&n," x4=");puthex(b,&n,u?u->uc_mcontext.regs[4]:0);
    putstr(b,&n," x5=");puthex(b,&n,u?u->uc_mcontext.regs[5]:0);
    putstr(b,&n," x19=");puthex(b,&n,u?u->uc_mcontext.regs[19]:0);
    putstr(b,&n," x20=");puthex(b,&n,u?u->uc_mcontext.regs[20]:0);
    putstr(b,&n," x22=");puthex(b,&n,u?u->uc_mcontext.regs[22]:0);
    putstr(b,&n," x23=");puthex(b,&n,u?u->uc_mcontext.regs[23]:0);
    putstr(b,&n," x25=");puthex(b,&n,u?u->uc_mcontext.regs[25]:0);
    putstr(b,&n," x26=");puthex(b,&n,u?u->uc_mcontext.regs[26]:0);
    putstr(b,&n," x27=");puthex(b,&n,u?u->uc_mcontext.regs[27]:0);
    putstr(b,&n," x28=");puthex(b,&n,u?u->uc_mcontext.regs[28]:0);
    /* Thread name is deliberately omitted: pthread_getname_np is not
       async-signal-safe, so only the kernel tid is recorded. */
    putstr(b,&n," tid=");putdec(b,&n,(uint64_t)(long)syscall(SYS_gettid));
    put_bt(b,&n,u?u->uc_mcontext.sp:0,u?u->uc_mcontext.regs[29]:0);
    b[n++]='\n'; if(logfd>=0)(void)write(logfd,b,n);
    n=0;
    put_stack_dump(b,&n,u?u->uc_mcontext.sp:0);
    if(logfd>=0)(void)write(logfd,b,n);
    n=0;
    put_code_dump(b,&n,u?u->uc_mcontext.regs[30]:0);
    if(logfd>=0)(void)write(logfd,b,n);
    n=0;
    put_object_dump(b,&n,u?u->uc_mcontext.regs[19]:0);
    if(logfd>=0)(void)write(logfd,b,n);
    unsigned i=0; while(i<4 && signals[i]!=sig)++i;
    if(i<4) {
        struct sigaction old=previous[i];
        (void)sigaction(sig,&old,NULL);
        if(old.sa_handler!=SIG_DFL && old.sa_handler!=SIG_IGN) {
            if(old.sa_flags&SA_SIGINFO) old.sa_sigaction(sig,info,context);
            else old.sa_handler(sig);
            return;
        }
    }
    /* Default disposition retains the operating system fatal behavior. */
    struct sigaction def={0}; def.sa_handler=SIG_DFL; sigemptyset(&def.sa_mask);
    (void)sigaction(sig,&def,NULL); (void)raise(sig);
}
bool native_fault_probe_begin(const char *path,const char *candidate) {
    if(!path||!candidate)return false;
    pthread_mutex_lock(&mutex);
    /* Idempotent: an early installer (login/trace entry) may arm the probe
     * before the UnityPlayer diagnostic does; both observe ready=1. */
    if(started){pthread_mutex_unlock(&mutex);return true;}
    int fd=open(path,O_WRONLY|O_CREAT|O_APPEND|O_CLOEXEC,0600);
    if(fd<0){pthread_mutex_unlock(&mutex);return false;}
    char line[512]; int n=snprintf(line,sizeof line,"candidate=%.*s pid=%ld tid=%ld\n",300,candidate,(long)getpid(),(long)syscall(SYS_gettid));
    if(n<0 || (size_t)n>=sizeof line || write(fd,line,(size_t)n)!=n){close(fd);pthread_mutex_unlock(&mutex);return false;}
    Dl_info self={0}, libc={0};
    void *copy_symbol=dlsym(RTLD_DEFAULT,"strncpy");
    (void)dladdr((void *)&native_fault_probe_begin,&self);
    if(copy_symbol)(void)dladdr(copy_symbol,&libc);
    n=snprintf(line,sizeof line,"module probe=%p base=%p path=%.150s strncpy=%p base=%p path=%.150s\n",(void *)&native_fault_probe_begin,self.dli_fbase,self.dli_fname?self.dli_fname:"?",copy_symbol,libc.dli_fbase,libc.dli_fname?libc.dli_fname:"?");
    if(n<0 || (size_t)n>=sizeof line || write(fd,line,(size_t)n)!=n){close(fd);pthread_mutex_unlock(&mutex);return false;}
    for(unsigned i=0;i<4;i++) if(sigaction(signals[i],NULL,&previous[i])!=0 || previous[i].sa_handler==SIG_IGN){close(fd);pthread_mutex_unlock(&mutex);return false;}
    logfd=fd; started=1;
    struct sigaction action={0}; action.sa_sigaction=fault_handler; action.sa_flags=SA_SIGINFO; sigemptyset(&action.sa_mask);
    unsigned i=0; for(;i<4;i++) if(sigaction(signals[i],&action,NULL)!=0)break;
    if(i<4) { while(i>0){--i;(void)sigaction(signals[i],&previous[i],NULL);} pthread_mutex_unlock(&mutex);return false; }
    pthread_mutex_unlock(&mutex);return true;
}
void native_fault_probe_end(void) {
    pthread_mutex_lock(&mutex);
    if(started) for(unsigned i=0;i<4;i++) { struct sigaction current; if(sigaction(signals[i],NULL,&current)==0 && (current.sa_flags&SA_SIGINFO) && current.sa_sigaction==fault_handler) (void)sigaction(signals[i],&previous[i],NULL); }
    /* Keep fd and state pinned: an already-dispatched handler can still write. */
    pthread_mutex_unlock(&mutex);
}
#else
bool native_fault_probe_begin(const char *path,const char *candidate) {(void)path;(void)candidate;return false;}
void native_fault_probe_end(void) {}
#endif


