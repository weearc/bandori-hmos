#define _GNU_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#include "android_abi_probe.h"
#include "android_semaphore_shim.h"
#include "android_signal_shim.h"

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

extern int sdk_trace_fprintf(FILE *, const char *, ...);

static int check(int condition, int number)
{
    return condition ? 0 : number;
}

int android_abi_probe_run(void)
{
    struct { uint8_t before[8]; uint8_t guest[16]; uint8_t after[8]; } sem_mem;
    struct { uint8_t before[8]; uint8_t guest[8]; uint8_t after[8]; } set_mem;
    struct { uint8_t before[8]; uint8_t guest[8]; uint8_t after[8]; } old_mem;
    struct { uint8_t before[8]; uint8_t guest[32]; uint8_t after[8]; } action_mem;
    uint8_t sem_copy[16], set_copy[8], action_copy[32], canary[8];
    int initialized = 0, rc = 0, value = -1, saved_errno,sem_passed=0;
    struct { int64_t sec, nsec; } timeout = { 0, 0 };
    int e;

    memset(&sem_mem, 0xA5, sizeof sem_mem);
    memset(&set_mem, 0x5A, sizeof set_mem);
    memset(&old_mem, 0x6B, sizeof old_mem);
    memset(&action_mem, 0x3C, sizeof action_mem);
    memcpy(sem_copy, sem_mem.guest, sizeof sem_copy);
    memcpy(set_copy, set_mem.guest, sizeof set_copy);
    memcpy(action_copy, action_mem.guest, sizeof action_copy);
    memcpy(canary, sem_mem.before, sizeof canary);
    if ((e = check(android_sem_init(sem_mem.guest, 0, 0) == 0, 1))) { rc=e; goto done; }
    initialized = 1;
    errno = 0;
    if ((e = check(android_sem_trywait(sem_mem.guest) == -1 && errno == EAGAIN, 2))) { rc=e; goto done; }
    if ((e = check(android_sem_post(sem_mem.guest) == 0, 3))) { rc=e; goto done; }
    if ((e = check(android_sem_wait(sem_mem.guest) == 0, 4))) { rc=e; goto done; }
    if ((e = check(android_sem_getvalue(sem_mem.guest, &value) == 0 && value == 0, 5))) { rc=e; goto done; }
    if ((e = check(android_sem_timedwait(sem_mem.guest, &timeout) == -1 && errno == ETIMEDOUT, 6))) { rc=e; goto done; }
    if ((e = check(android_sem_init(sem_mem.guest, 1, 0) == -1 && errno == ENOSYS, 7))) { rc=e; goto done; }
    if ((e = check(android_sem_destroy(sem_mem.guest) == 0, 8))) { rc=e; goto done; }
    initialized = 0;
    if ((e = check(memcmp(sem_copy, sem_mem.guest, sizeof sem_copy)==0 && memcmp(canary,sem_mem.before,8)==0 && memcmp(canary,sem_mem.after,8)==0, 9))) { rc=e; goto done; }
    sem_passed=1;

    errno = EDOM;
    if ((e = check(android_signal_sigemptyset(set_mem.guest)==0, 10))) { rc=e; goto done; }
    if ((e = check(android_signal_sigaddset(set_mem.guest, SIGUSR1)==0, 11))) { rc=e; goto done; }
    uint64_t bits;memcpy(&bits,set_mem.guest,sizeof bits);
    if(bits!=(UINT64_C(1)<<(SIGUSR1-1))){rc=16;goto done;}
    if ((e = check(memcmp(set_mem.before, (uint8_t[8]){[0 ... 7]=0x5A}, 8)==0 && memcmp(set_mem.after,(uint8_t[8]){[0 ... 7]=0x5A},8)==0,12))) { rc=e; goto done; }
    saved_errno=errno;
    if ((e = check(android_signal_pthread_sigmask(0, NULL, old_mem.guest)==0 && errno==saved_errno,13))) { rc=e; goto done; }
    if(memcmp(old_mem.before,(uint8_t[8]){[0 ... 7]=0x6B},8)||memcmp(old_mem.after,(uint8_t[8]){[0 ... 7]=0x6B},8)){rc=17;goto done;}
    if ((e = check(android_signal_sigaction(SIGUSR1,NULL,action_mem.guest)==0,14))) { rc=e; goto done; }
    if ((e = check(memcmp(action_mem.before,(uint8_t[8]){[0 ... 7]=0x3C},8)==0 && memcmp(action_mem.after,(uint8_t[8]){[0 ... 7]=0x3C},8)==0,15))) { rc=e; goto done; }
    (void)set_copy; (void)action_copy;
done:
    if (initialized) android_sem_destroy(sem_mem.guest);
    sdk_trace_fprintf(stderr,"ANDROID_ABI_PROBE %s check=%d sem=%d signal=%d invoked_guest_engine=0 signal_disposition_changed=0\n",rc?"FAIL":"PASS",rc,sem_passed,!rc);
    return rc;
}
