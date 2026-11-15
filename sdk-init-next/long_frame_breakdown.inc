/* Fixed storage, no allocation/IO/blocking locks in any producer. Export only
 * after capture stops. A busy slot is dropped and made visible, never waited. */
#include <errno.h>
#include <sys/syscall.h>
#define LONG_BLOCKS 131072u
struct long_block {atomic_uint busy;uint64_t sequence,start,end,tid;unsigned kind;};
static struct long_block long_blocks[LONG_BLOCKS];
static atomic_uint_fast64_t long_sequence,long_capture_first,long_dropped;
static atomic_uint long_inflight;
static _Thread_local unsigned long_depth[PERF_BLOCK_COUNT];
static _Thread_local uint64_t long_tid;
static uint64_t long_thread(void){
 if(!long_tid)long_tid=(uint64_t)syscall(SYS_gettid);return long_tid;
}
void perf_block_record(unsigned kind,uint64_t start,uint64_t end){
 if(!perf_play_enabled()||!start||end<start||kind==0||kind>=PERF_BLOCK_COUNT)return;
 int saved=errno;uint64_t seq=atomic_fetch_add_explicit(&long_sequence,1,memory_order_relaxed)+1;
 struct long_block *b=&long_blocks[(seq-1)%LONG_BLOCKS];
 if(atomic_exchange_explicit(&b->busy,1,memory_order_acquire)){
  atomic_fetch_add_explicit(&long_dropped,1,memory_order_relaxed);errno=saved;return;
 }
 b->sequence=seq;b->start=start;b->end=end;b->tid=long_thread();b->kind=kind;
 atomic_store_explicit(&b->busy,0,memory_order_release);errno=saved;
}
struct perf_block_scope perf_block_begin(unsigned kind){
 int saved=errno;
 struct perf_block_scope s={0,0};
 if(!perf_play_enabled()||kind==0||kind>=PERF_BLOCK_COUNT)return s;
 s.kind=kind;if(long_depth[kind]++==0){s.start=perf_now();atomic_fetch_add_explicit(&long_inflight,1,memory_order_relaxed);}errno=saved;return s;
}
void perf_block_finish(struct perf_block_scope *s){
 int saved=errno;
 if(!s->kind)return;
 if(long_depth[s->kind])--long_depth[s->kind];
 if(s->start){uint64_t end=perf_now();perf_block_record(s->kind,s->start,end);atomic_fetch_sub_explicit(&long_inflight,1,memory_order_relaxed);}
 s->kind=0;s->start=0;errno=saved;
}
struct long_totals {uint64_t same[PERF_BLOCK_COUNT],other[PERF_BLOCK_COUNT],lost;};
static void long_overlap(struct long_totals *t,unsigned kind,uint64_t a,uint64_t b,uint64_t tid,uint64_t start,uint64_t end,uint64_t frame_tid){
 uint64_t lo=a>start?a:start,hi=b<end?b:end;if(hi<=lo||kind==0||kind>=PERF_BLOCK_COUNT)return;
 if(tid==frame_tid)t->same[kind]+=hi-lo;else t->other[kind]+=hi-lo;
}
static struct long_totals long_sum(uint64_t start,uint64_t end,uint64_t tid,uint64_t last){
 struct long_totals t={0};uint64_t first=atomic_load(&long_capture_first)+1;
 if(last>=LONG_BLOCKS&&first<last-LONG_BLOCKS+1){first=last-LONG_BLOCKS+1;t.lost=1;}
 for(uint64_t n=first;n<=last;++n){struct long_block *b=&long_blocks[(n-1)%LONG_BLOCKS];
  if(atomic_exchange_explicit(&b->busy,1,memory_order_acquire)){++t.lost;continue;}
  if(b->sequence==n)long_overlap(&t,b->kind,b->start,b->end,b->tid,start,end,tid);else ++t.lost;
  atomic_store_explicit(&b->busy,0,memory_order_release);
 }return t;
}
