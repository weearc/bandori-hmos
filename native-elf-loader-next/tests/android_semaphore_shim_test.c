#define _POSIX_C_SOURCE 200809L
#include "../android_semaphore_shim.h"
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

typedef struct {uint64_t before;unsigned char guest[16];uint64_t after;} guarded;
typedef struct {void *sem;atomic_int started;int result;} worker_data;
static void *waiter(void *arg){worker_data*d=arg;atomic_store(&d->started,1);d->result=android_sem_wait(d->sem);return NULL;}
#define CHECK(x) do{if(!(x)){fprintf(stderr,"semaphore test failed line=%d errno=%d\n",__LINE__,errno);return 1;}}while(0)
int main(void)
{
    guarded g={UINT64_C(0x123456789abcdef0),{0},UINT64_C(0xfedcba9876543210)};
    memset(g.guest,0xa5,sizeof(g.guest));unsigned char original[16];memcpy(original,g.guest,16);
    errno=0;CHECK(android_sem_init(g.guest,1,0)==-1 && errno==ENOSYS);
    CHECK(android_sem_init(g.guest,0,0)==0);
    errno=0;CHECK(android_sem_init(g.guest,0,0)==-1 && errno==EBUSY);
    int value=-1;CHECK(android_sem_getvalue(g.guest,&value)==0 && value==0);
    errno=0;CHECK(android_sem_trywait(g.guest)==-1 && errno==EAGAIN);
    struct timespec now;CHECK(clock_gettime(CLOCK_REALTIME,&now)==0);
    int64_t past[2]={(int64_t)now.tv_sec-1,(int64_t)now.tv_nsec};
    errno=0;CHECK(android_sem_timedwait(g.guest,past)==-1 && errno==ETIMEDOUT);
    worker_data d={.sem=g.guest,.result=-9};atomic_init(&d.started,0);pthread_t t;
    CHECK(pthread_create(&t,NULL,waiter,&d)==0);
    while(!atomic_load(&d.started)){struct timespec delay={0,1000000};nanosleep(&delay,NULL);}
    /* Allow the worker to enter the native blocking wait before destroy. */
    struct timespec delay={0,100000000};nanosleep(&delay,NULL);
    errno=0;int destroy_result=android_sem_destroy(g.guest),destroy_errno=errno;
    if(destroy_result==0){android_sem_init(g.guest,0,0);android_sem_post(g.guest);pthread_join(t,NULL);fprintf(stderr,"worker was not pinned before destroy\n");return 1;}
    CHECK(destroy_result==-1 && destroy_errno==EBUSY);
    CHECK(android_sem_post(g.guest)==0);CHECK(pthread_join(t,NULL)==0 && d.result==0);
    CHECK(android_sem_destroy(g.guest)==0);
    errno=0;CHECK(android_sem_post(g.guest)==-1 && errno==EINVAL);
    CHECK(android_sem_init(g.guest,0,1)==0);CHECK(android_sem_trywait(g.guest)==0);
    CHECK(android_sem_destroy(g.guest)==0);
    CHECK(g.before==UINT64_C(0x123456789abcdef0)&&g.after==UINT64_C(0xfedcba9876543210)&&!memcmp(g.guest,original,16));
    puts("ANDROID_SEMAPHORE_SHIM_TEST PASS canary=1 wait_post=1 timeout=1 destroy_busy=1 reinit=1");return 0;
}
