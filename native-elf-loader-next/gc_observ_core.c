#include "gc_observ_core.h"
#include <stdatomic.h>
#if defined(fopen) || defined(write) || defined(malloc) || defined(fsync) || defined(vdprintf)
#error GC_OBSERV_CORE_MUST_NOT_USE_INTERPOSITION_HEADERS
#endif
_Static_assert(ATOMIC_LLONG_LOCK_FREE==2,"64-bit counters must be lock-free");
_Static_assert(ATOMIC_INT_LOCK_FREE==2,"ring indices must be lock-free");
struct gco_ring { _Atomic unsigned head,tail;struct gco_row rows[GCO_RING_CAP]; };
static struct gco_ring rings[GCO_PRODUCERS];
static _Atomic unsigned enabled,producers;
static _Atomic uint64_t counts[GCO_STAGES][8],reasons[GCO_REASONS],dropped;
/* Thread-pointer identity uses a compiler intrinsic, never a TLS resolver.
 * Bounded open addressing; owners are never cleared. Reused thread pointers
 * can reuse a ring only after the old thread has terminated. No address export. */
_Static_assert(ATOMIC_POINTER_LOCK_FREE==2,"owner identities must be lock-free");
static _Atomic uintptr_t owners[GCO_PRODUCERS];
void gco_emit(unsigned stage,uint64_t seq,int action,int a,int b,int c,uint64_t begin,uint64_t end){
 if(!atomic_load_explicit(&enabled,memory_order_relaxed)||stage>=GCO_STAGES)return;
 unsigned bucket=(unsigned)(action&255);if(bucket>6)bucket=7;
 atomic_fetch_add_explicit(&counts[stage][bucket],1,memory_order_relaxed);
 uintptr_t key=(uintptr_t)__builtin_thread_pointer();
 unsigned id=0;
 for(unsigned n=0;n<GCO_PRODUCERS;n++){
  unsigned slot=(unsigned)((key>>4)+n)%GCO_PRODUCERS;
  uintptr_t owner=atomic_load_explicit(&owners[slot],memory_order_relaxed);
  if(owner==key){id=slot+1;break;}
  if(!owner){
   uintptr_t empty=0;
   if(atomic_compare_exchange_strong_explicit(&owners[slot],&empty,key,memory_order_relaxed,memory_order_relaxed)){
    atomic_fetch_add_explicit(&producers,1,memory_order_relaxed);id=slot+1;break;
   }
   if(empty==key){id=slot+1;break;}
  }
 }
 if(!id){atomic_fetch_add_explicit(&dropped,1,memory_order_relaxed);return;}
 struct gco_ring *q=&rings[id-1];
 unsigned h=atomic_load_explicit(&q->head,memory_order_relaxed);
 unsigned t=atomic_load_explicit(&q->tail,memory_order_acquire);
 if((unsigned)(h-t)>=GCO_RING_CAP){atomic_fetch_add_explicit(&dropped,1,memory_order_relaxed);return;}
 struct gco_row *r=&q->rows[h%GCO_RING_CAP];
 r->seq=seq;r->begin=begin;r->end=end;r->action=action;
 r->a=a;r->b=b;r->c=c;r->stage=stage;r->producer=id-1;
 atomic_store_explicit(&q->head,h+1,memory_order_release);
}
void gco_contract_failure(unsigned reason){
 if(reason<GCO_REASONS)atomic_fetch_add_explicit(&reasons[reason],1,memory_order_relaxed);
 gco_emit(GCO_CONTRACT,0,-1,(int)reason,0,0,0,0);
}
void gco_set_enabled(unsigned on){atomic_store_explicit(&enabled,on==1,memory_order_relaxed);}
int gco_take(unsigned producer_index,struct gco_row *out){
 if(producer_index>=GCO_PRODUCERS||!out)return 0;
 struct gco_ring *q=&rings[producer_index];
 unsigned t=atomic_load_explicit(&q->tail,memory_order_relaxed);
 unsigned h=atomic_load_explicit(&q->head,memory_order_acquire);
 if(t==h)return 0;
 *out=q->rows[t%GCO_RING_CAP];
 atomic_store_explicit(&q->tail,t+1,memory_order_release);return 1;
}
void gco_read_snapshot(struct gco_snapshot *out){
 for(unsigned i=0;i<GCO_STAGES;i++)for(unsigned j=0;j<8;j++)out->counts[i][j]=atomic_load_explicit(&counts[i][j],memory_order_relaxed);
 for(unsigned i=0;i<GCO_REASONS;i++)out->reasons[i]=atomic_load_explicit(&reasons[i],memory_order_relaxed);
 out->dropped=atomic_load_explicit(&dropped,memory_order_relaxed);
 out->enabled=atomic_load_explicit(&enabled,memory_order_relaxed);
 out->producers=atomic_load_explicit(&producers,memory_order_relaxed);
}
