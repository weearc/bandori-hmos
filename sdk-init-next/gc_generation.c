#include "gc_generation.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <limits.h>
#define GEN_PAGE_SLOTS 4096u
#define GEN_PAGES (DVM_GEN_SLOT_CAPACITY/GEN_PAGE_SLOTS)
#ifndef DVM_GEN_TICKET_LIMIT
#define DVM_GEN_TICKET_LIMIT UINT32_MAX
#endif
_Static_assert(DVM_GEN_TICKET_LIMIT>0 && DVM_GEN_TICKET_LIMIT<=UINT32_MAX,"nonzero 32-bit ticket domain");
_Static_assert(DVM_GEN_SLOT_CAPACITY==4194304u,"same capacity as ID directory");
struct gen_page {_Atomic uint32_t tickets[GEN_PAGE_SLOTS];};
struct gen_shadow {uint32_t capacity;_Atomic(struct gen_page *) pages[GEN_PAGES];};
static _Atomic uint64_t gen_next_ticket=1;
static _Atomic uint64_t issued,reuse_bumps,check_ok,check_stale,overflow;
static enum gen_result issue(uint32_t *out){
 uint64_t next=atomic_load_explicit(&gen_next_ticket,memory_order_relaxed);
 for(;;){
  if(next>DVM_GEN_TICKET_LIMIT){atomic_fetch_add_explicit(&overflow,1,memory_order_relaxed);return GEN_TICKET_EXHAUSTED;}
  if(atomic_compare_exchange_weak_explicit(&gen_next_ticket,&next,next+1,memory_order_relaxed,memory_order_relaxed)){
   *out=(uint32_t)next;atomic_fetch_add_explicit(&issued,1,memory_order_relaxed);return GEN_SUCCESS;
  }
 }
}
enum gen_result gen_shadow_create(uint32_t capacity,struct gen_shadow **out){
 if(!out)return GEN_INVALID_ARGUMENT;
 *out=NULL;
 if(!capacity)return GEN_INVALID_ARGUMENT;
 if(capacity>DVM_GEN_SLOT_CAPACITY){atomic_fetch_add_explicit(&overflow,1,memory_order_relaxed);return GEN_SLOT_CAPACITY;}
 struct gen_shadow *t=calloc(1,sizeof *t);if(!t)return GEN_ALLOCATION_FAILED;
 t->capacity=capacity;for(unsigned p=0;p<GEN_PAGES;p++)atomic_init(&t->pages[p],NULL);
 *out=t;return GEN_SUCCESS;
}
void gen_shadow_destroy(struct gen_shadow *t){
 if(!t)return;
 for(unsigned p=0;p<GEN_PAGES;p++)free(atomic_load_explicit(&t->pages[p],memory_order_relaxed));
 free(t);
}
enum gen_result gen_shadow_publish(struct gen_shadow *t,uint32_t slot,uint32_t *out){
 if(out)*out=0;
 if(!t||!out||!slot)return GEN_INVALID_ARGUMENT;
 if(slot>t->capacity){atomic_fetch_add_explicit(&overflow,1,memory_order_relaxed);return GEN_SLOT_CAPACITY;}
 uint32_t index=slot-1,p=index/GEN_PAGE_SLOTS,offset=index%GEN_PAGE_SLOTS;
 struct gen_page *page=atomic_load_explicit(&t->pages[p],memory_order_acquire),*prepared=NULL;
 if(!page){
  prepared=calloc(1,sizeof *prepared);if(!prepared)return GEN_ALLOCATION_FAILED;
  for(unsigned i=0;i<GEN_PAGE_SLOTS;i++)atomic_init(&prepared->tickets[i],0);
  page=prepared;
 }
 uint32_t ticket;enum gen_result result=issue(&ticket);
 if(result!=GEN_SUCCESS){free(prepared);return result;}
 uint32_t previous=atomic_load_explicit(&page->tickets[offset],memory_order_relaxed);
 atomic_store_explicit(&page->tickets[offset],ticket,memory_order_release);
 if(prepared)atomic_store_explicit(&t->pages[p],page,memory_order_release);
 if(previous)atomic_fetch_add_explicit(&reuse_bumps,1,memory_order_relaxed);
 *out=ticket;return GEN_SUCCESS;
}
uint32_t gen_ticket_of(const struct gen_shadow *t,uint32_t slot){
 if(!t||!slot||slot>t->capacity)return 0;
 uint32_t i=slot-1;struct gen_page *p=atomic_load_explicit(&t->pages[i/GEN_PAGE_SLOTS],memory_order_acquire);
 return p?atomic_load_explicit(&p->tickets[i%GEN_PAGE_SLOTS],memory_order_acquire):0;
}
bool gen_check(const struct gen_shadow *t,uint32_t slot,uint32_t expected){
 bool ok=expected&&gen_ticket_of(t,slot)==expected;
 atomic_fetch_add_explicit(ok?&check_ok:&check_stale,1,memory_order_relaxed);return ok;
}
void gen_snapshot(struct gen_counters *out){
 if(!out)return;
 out->gen_issued=atomic_load_explicit(&issued,memory_order_relaxed);
 out->gen_reuse_bumps=atomic_load_explicit(&reuse_bumps,memory_order_relaxed);
 out->gen_check_ok=atomic_load_explicit(&check_ok,memory_order_relaxed);
 out->gen_check_stale=atomic_load_explicit(&check_stale,memory_order_relaxed);
 out->gen_overflow=atomic_load_explicit(&overflow,memory_order_relaxed);
}
