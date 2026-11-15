#ifndef NATIVE_SCRATCH_H
#define NATIVE_SCRATCH_H
/* C-only temporary storage. Never use for guest objects, JNI loans or pointers
 * returned to a caller. Each translation unit owns independent per-thread
 * slots; nested live acquisitions occupy distinct slots. */
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#define NATIVE_SCRATCH_SLOTS 8
#define NATIVE_SCRATCH_LIMIT 65536u
struct native_scratch_slot {void *data;size_t capacity;int active;};
struct native_scratch_state {struct native_scratch_slot slot[NATIVE_SCRATCH_SLOTS];};
static pthread_key_t native_scratch_key;
static pthread_once_t native_scratch_once=PTHREAD_ONCE_INIT;
static int native_scratch_ready;
static void native_scratch_destroy(void *p){struct native_scratch_state *s=p;if(s){for(unsigned i=0;i<NATIVE_SCRATCH_SLOTS;i++)free(s->slot[i].data);free(s);}}
static void native_scratch_init(void){native_scratch_ready=pthread_key_create(&native_scratch_key,native_scratch_destroy)==0;}
static void *native_scratch_acquire(size_t bytes){
 if(!bytes)return NULL;
 pthread_once(&native_scratch_once,native_scratch_init);
 if(!native_scratch_ready||bytes>NATIVE_SCRATCH_LIMIT)return calloc(1,bytes);
 struct native_scratch_state *s=pthread_getspecific(native_scratch_key);
 if(!s){s=calloc(1,sizeof *s);if(!s)return NULL;if(pthread_setspecific(native_scratch_key,s)){free(s);return calloc(1,bytes);}}
 for(unsigned i=0;i<NATIVE_SCRATCH_SLOTS;i++)if(!s->slot[i].active){
  struct native_scratch_slot *slot=&s->slot[i];
  if(slot->capacity<bytes){void *p=realloc(slot->data,bytes);if(!p)return NULL;slot->data=p;slot->capacity=bytes;}
  slot->active=1;memset(slot->data,0,bytes);return slot->data;
 }
 return calloc(1,bytes); /* bounded cache; deep recursion retains old semantics */
}
static void native_scratch_release(void *p){
 if(!p)return;
 struct native_scratch_state *s=native_scratch_ready?pthread_getspecific(native_scratch_key):NULL;
 if(s)for(unsigned i=0;i<NATIVE_SCRATCH_SLOTS;i++)if(s->slot[i].data==p){s->slot[i].active=0;memset(p,0,s->slot[i].capacity);return;}
 free(p);
}
#endif
