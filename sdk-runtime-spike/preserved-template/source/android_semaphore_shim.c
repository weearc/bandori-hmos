#define _POSIX_C_SOURCE 200809L
#include "android_semaphore_shim.h"
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#define SEM_SLOTS 512
/* Slot memory is stable. state=1 is live, state=2 is being initialized or
 * destroyed. The registry lock protects state, identity and active calls. */
typedef struct { void *guest; sem_t native; unsigned active; int state; } sem_slot;
static sem_slot slots[SEM_SLOTS];
static pthread_mutex_t registry=PTHREAD_MUTEX_INITIALIZER;
static sem_slot *lookup(void *guest)
{
    for(unsigned i=0;i<SEM_SLOTS;++i)if(slots[i].state && slots[i].guest==guest)return &slots[i];
    return NULL;
}
static sem_slot *pin(void *guest)
{
    if(!guest){errno=EINVAL;return NULL;}
    pthread_mutex_lock(&registry);sem_slot *s=lookup(guest);
    if(!s || s->state!=1){pthread_mutex_unlock(&registry);errno=EINVAL;return NULL;}
    ++s->active;pthread_mutex_unlock(&registry);return s;
}
static void unpin(sem_slot *s,int saved_errno)
{
    pthread_mutex_lock(&registry);--s->active;pthread_mutex_unlock(&registry);errno=saved_errno;
}
int android_sem_init(void *guest,int shared,unsigned value)
{
    if(!guest){errno=EINVAL;return -1;}
    if(shared){errno=ENOSYS;return -1;}
#ifdef SEM_VALUE_MAX
    if(value>(unsigned)SEM_VALUE_MAX){errno=EINVAL;return -1;}
#endif
    pthread_mutex_lock(&registry);
    if(lookup(guest)){pthread_mutex_unlock(&registry);errno=EBUSY;return -1;}
    sem_slot *s=NULL;for(unsigned i=0;i<SEM_SLOTS;++i)if(!slots[i].state){s=&slots[i];break;}
    if(!s){pthread_mutex_unlock(&registry);errno=ENOSPC;return -1;}
    s->state=2;s->guest=guest;s->active=0;pthread_mutex_unlock(&registry);
    int rc=sem_init(&s->native,0,value);int saved=errno;
    pthread_mutex_lock(&registry);if(rc){s->guest=NULL;s->state=0;}else s->state=1;pthread_mutex_unlock(&registry);errno=saved;return rc;
}
int android_sem_destroy(void *guest)
{
    if(!guest){errno=EINVAL;return -1;}
    pthread_mutex_lock(&registry);sem_slot *s=lookup(guest);
    if(!s || s->state!=1){pthread_mutex_unlock(&registry);errno=EINVAL;return -1;}
    if(s->active){pthread_mutex_unlock(&registry);errno=EBUSY;return -1;}
    s->state=2;pthread_mutex_unlock(&registry);
    int rc=sem_destroy(&s->native);int saved=errno;
    pthread_mutex_lock(&registry);if(rc)s->state=1;else{s->guest=NULL;s->state=0;}pthread_mutex_unlock(&registry);errno=saved;return rc;
}
int android_sem_post(void *guest){sem_slot*s=pin(guest);if(!s)return -1;int r=sem_post(&s->native),e=errno;unpin(s,e);return r;}
static void cancel_unpin(void *arg){unpin((sem_slot *)arg,errno);}
int android_sem_wait(void *guest)
{
    sem_slot *s=pin(guest);if(!s)return -1;int r,e;
    pthread_cleanup_push(cancel_unpin,s);
    r=sem_wait(&s->native);e=errno;
    pthread_cleanup_pop(0);
    unpin(s,e);return r;
}
int android_sem_trywait(void *guest){sem_slot*s=pin(guest);if(!s)return -1;int r=sem_trywait(&s->native),e=errno;unpin(s,e);return r;}
int android_sem_getvalue(void *guest,int *value)
{
    if(!value){errno=EINVAL;return -1;}sem_slot*s=pin(guest);if(!s)return -1;
    int r=sem_getvalue(&s->native,value),e=errno;unpin(s,e);return r;
}
int android_sem_timedwait(void *guest,const void *timeout)
{
    if(!timeout){errno=EINVAL;return -1;}
    int64_t values[2];memcpy(values,timeout,sizeof(values));
    if(values[1]<0 || values[1]>=1000000000){errno=EINVAL;return -1;}
    struct timespec ts;ts.tv_sec=(time_t)values[0];ts.tv_nsec=(long)values[1];
    if((int64_t)ts.tv_sec!=values[0]){errno=EOVERFLOW;return -1;}
    sem_slot *s=pin(guest);if(!s)return -1;int r,e;
    pthread_cleanup_push(cancel_unpin,s);
    r=sem_timedwait(&s->native,&ts);e=errno;
    pthread_cleanup_pop(0);
    unpin(s,e);return r;
}
