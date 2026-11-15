#define _GNU_SOURCE
#include "wait_context.h"
#include <stdatomic.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#define WAIT_SLOTS 512
static struct {atomic_int tid,kind;atomic_uintptr_t pc,fp,condition,mutex;atomic_uint_fast64_t start,generation;} slots[WAIT_SLOTS];
static _Thread_local int index_=-1;
void perf_wait_enter(int kind,void*c,void*m,void*pc,void*fp){
 int saved=errno;
 int tid=(int)syscall(SYS_gettid);
 if(index_<0)for(unsigned i=0;i<WAIT_SLOTS;i++){int empty=0;if(atomic_compare_exchange_strong(&slots[i].tid,&empty,tid)){index_=(int)i;break;}}
 if(index_<0){errno=saved;return;}
 unsigned i=(unsigned)index_;atomic_fetch_add(&slots[i].generation,1);
 atomic_store(&slots[i].kind,0);atomic_store(&slots[i].pc,(uintptr_t)pc);atomic_store(&slots[i].fp,(uintptr_t)fp);
 atomic_store(&slots[i].condition,(uintptr_t)c);atomic_store(&slots[i].mutex,(uintptr_t)m);
 struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);atomic_store(&slots[i].start,(uint64_t)t.tv_sec*1000000000+t.tv_nsec);
 atomic_store(&slots[i].kind,kind);atomic_fetch_add(&slots[i].generation,1);
 errno=saved;
}
void perf_wait_leave(void){if(index_>=0){atomic_fetch_add(&slots[index_].generation,1);atomic_store(&slots[index_].kind,0);atomic_fetch_add(&slots[index_].generation,1);}}
int perf_wait_get(int tid,struct perf_wait_context*out){
 for(unsigned i=0;i<WAIT_SLOTS;i++)if(atomic_load(&slots[i].tid)==tid){
  uint64_t generation=atomic_load(&slots[i].generation);if(generation&1)return 0;
  *out=(struct perf_wait_context){.tid=tid,.kind=atomic_load(&slots[i].kind),.pc=atomic_load(&slots[i].pc),.fp=atomic_load(&slots[i].fp),.condition=atomic_load(&slots[i].condition),.mutex=atomic_load(&slots[i].mutex),.start_ns=atomic_load(&slots[i].start),.generation=generation};out->sp=out->fp;
  return out->kind&&generation==atomic_load(&slots[i].generation);
 }return 0;
}
