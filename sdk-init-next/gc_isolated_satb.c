#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "gc_isolated_satb.h"
#include "gc_runtime.h"
#include "page_geometry.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdalign.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>

struct gcs_object {
 _Atomic uint32_t flags;
 _Atomic(struct gcs_object *) brooks;
 uint64_t birth;
 struct gcs_heap *owner;
 const struct dvm_gc_layout *layout;
 size_t nslots;
 _Atomic uintptr_t slots[];
};
struct gcs_block {
 size_t offset,bytes;
 uint32_t generation,borrows,pins;
 bool retired,keep;
 size_t next_free;
};
struct gcs_heap {
 pthread_mutex_t lock;
 pthread_cond_t wake;
 pthread_t worker,mutator;
 unsigned char *arena;
 size_t bytes,used,capacity,count;
 gcs_object **objects,**stack,**satb;
 size_t top,qhead,qcount,qcap;
 uint64_t sequence,cutoff;
 bool active,held,started,joined,stop,ack;
 int error;
 size_t marked,scanned,barriers,queued,new_skipped,duplicates,remarks,overflows;
 bool distinct_thread;
 bool page_backed;
 size_t page_size,free_head;
 struct gcs_block *blocks;
 struct gcs_evac_stats evac;
};
static int fail_locked(gcs_heap *h,int error){
 if(!h->error)h->error=error;
 pthread_cond_broadcast(&h->wake);return h->error;
}
static bool valid(gcs_heap *h,gcs_object *o){
 /* Exact table lookup before dereference; scalar-looking pointer bits are
  * never accepted merely because they fall inside the arena. */
 if(!o)return false;
 for(size_t i=0;i<h->count;i++)if(h->objects[i]==o)return true;
 return false;
}
static gcs_object *resolve_locked(gcs_heap *h,gcs_object *o){
 for(size_t n=0;o&&n<=h->count;n++){
  if(!valid(h,o))return NULL;
  gcs_object *next=atomic_load_explicit(&o->brooks,memory_order_acquire);
  if(next==o)return o;
  o=next;
 }
 return NULL;
}
static bool ref_slot(const gcs_object *o,size_t slot){
 if(slot>=o->nslots)return false;
 for(size_t i=0;i<o->layout->instance_count;i++)
  if(o->layout->instance_slots[i]==slot)return true;
 return false;
}
static int offer_locked(gcs_heap *h,gcs_object *o){
 if(!o)return GCS_OK;
 o=resolve_locked(h,o);if(!o)return fail_locked(h,GCS_INVALID);
 if(o->birth>h->cutoff){h->new_skipped++;return GCS_OK;}
 uint32_t old=atomic_fetch_or_explicit(&o->flags,DVM_GC_MARKED,memory_order_acq_rel);
 if(old&DVM_GC_MARKED){h->duplicates++;return GCS_OK;}
 if(h->top==h->capacity)return fail_locked(h,GCS_OOM);
 h->stack[h->top++]=o;h->marked++;pthread_cond_signal(&h->wake);return GCS_OK;
}
static void *marker(void *arg){
 gcs_heap *h=arg;pthread_mutex_lock(&h->lock);
 h->distinct_thread=!pthread_equal(pthread_self(),h->mutator);
 h->ack=true;pthread_cond_broadcast(&h->wake);
 while(!h->stop&&!h->error){
  if(h->held){pthread_cond_wait(&h->wake,&h->lock);continue;}
  if(h->qcount){
   gcs_object *old=h->satb[h->qhead];
   h->qhead=(h->qhead+1)%h->qcap;h->qcount--;
   offer_locked(h,old);continue;
  }
  if(h->top){
   gcs_object *o=h->stack[--h->top];h->scanned++;
   /* Same precise instance_slots representation as GC1, never a scan of
    * pointer-shaped primitives. Mutator stores and scanning use this lock;
    * object fields and mark bits are also C11 atomics. */
   for(size_t i=0;i<o->layout->instance_count&&!h->error;i++){
    uint32_t slot=o->layout->instance_slots[i];
    offer_locked(h,(gcs_object *)atomic_load_explicit(&o->slots[slot],memory_order_acquire));
   }
   pthread_mutex_unlock(&h->lock);
   pthread_mutex_lock(&h->lock);
   continue;
  }
  pthread_cond_broadcast(&h->wake);
  pthread_cond_wait(&h->wake,&h->lock);
 }
 pthread_cond_broadcast(&h->wake);pthread_mutex_unlock(&h->lock);return NULL;
}
gcs_heap *gcs_create(size_t bytes,size_t cap,size_t qcap){
 if(!bytes||!cap||!qcap||cap>SIZE_MAX/sizeof(gcs_object*)||qcap>SIZE_MAX/sizeof(gcs_object*))return NULL;
 gcs_heap *h=calloc(1,sizeof *h);if(!h)return NULL;
 h->arena=malloc(bytes);h->objects=calloc(cap,sizeof *h->objects);
 h->stack=calloc(cap,sizeof *h->stack);h->satb=calloc(qcap,sizeof *h->satb);
 if(!h->arena||!h->objects||!h->stack||!h->satb)goto bad;
 if(pthread_mutex_init(&h->lock,NULL))goto bad;
 if(pthread_cond_init(&h->wake,NULL)){pthread_mutex_destroy(&h->lock);goto bad;}
 h->bytes=bytes;h->capacity=cap;h->qcap=qcap;return h;
bad: free(h->arena);free(h->objects);free(h->stack);free(h->satb);free(h);return NULL;
}
void gcs_destroy(gcs_heap *h){
 if(!h)return;
 if(h->started&&!h->joined){
  pthread_mutex_lock(&h->lock);h->stop=true;pthread_cond_broadcast(&h->wake);pthread_mutex_unlock(&h->lock);
  pthread_join(h->worker,NULL);
 }
 pthread_cond_destroy(&h->wake);pthread_mutex_destroy(&h->lock);
 if(h->page_backed)munmap(h->arena,h->bytes);else free(h->arena);
 free(h->blocks);free(h->objects);free(h->stack);free(h->satb);free(h);
}
gcs_object *gcs_alloc(gcs_heap *h,const struct dvm_gc_layout *layout,size_t slots){
 if(!h||!layout||(layout->instance_count&&!layout->instance_slots))return NULL;
 for(size_t i=0;i<layout->instance_count;i++)if(layout->instance_slots[i]>=slots)return NULL;
 if(slots>(SIZE_MAX-sizeof(gcs_object))/sizeof(_Atomic uintptr_t))return NULL;
 size_t size=sizeof(gcs_object)+slots*sizeof(_Atomic uintptr_t),alignment=alignof(max_align_t);
 if(size>SIZE_MAX-(alignment-1))return NULL;
 size=(size+alignment-1)/alignment*alignment;
 pthread_mutex_lock(&h->lock);
 size_t index=h->count,offset=h->used;
 if(h->page_backed){
  if(size>SIZE_MAX-(h->page_size-1)){fail_locked(h,GCS_OOM);pthread_mutex_unlock(&h->lock);return NULL;}
  size=(size+h->page_size-1)/h->page_size*h->page_size;
  size_t *link=&h->free_head;
  while(*link!=SIZE_MAX){size_t i=*link;
   if(h->blocks[i].bytes>=size){index=i;offset=h->blocks[i].offset;size=h->blocks[i].bytes;*link=h->blocks[i].next_free;h->evac.reuse_hits++;break;}
   link=&h->blocks[i].next_free;
  }
 }
 if(h->error||(index==h->count&&(h->count==h->capacity||size>h->bytes-h->used))){fail_locked(h,GCS_OOM);pthread_mutex_unlock(&h->lock);return NULL;}
 if(h->page_backed){
  if(mprotect(h->arena+offset,size,PROT_READ|PROT_WRITE)){fail_locked(h,GCS_INVALID);pthread_mutex_unlock(&h->lock);return NULL;}
  memset(h->arena+offset,0,size); /* Touch backing; later MADV_DONTNEED is real. */
  struct gcs_block *block=&h->blocks[index];block->offset=offset;block->bytes=size;
  block->generation++;if(!block->generation)block->generation++;
  block->retired=block->keep=false;block->borrows=block->pins=0;
  h->evac.committed_bytes+=size;
 }
 gcs_object *o=(gcs_object *)(h->arena+offset);
 if(index==h->count){h->used+=size;h->count++;}
 atomic_init(&o->flags,0);atomic_init(&o->brooks,o);o->birth=++h->sequence;o->owner=h;o->layout=layout;o->nslots=slots;
 for(size_t i=0;i<slots;i++)atomic_init(&o->slots[i],0);
 h->objects[index]=o;pthread_mutex_unlock(&h->lock);return o;
}
int gcs_put_ref(gcs_heap *h,gcs_object *o,size_t slot,gcs_object *value){
 if(!h)return GCS_INVALID;
 pthread_mutex_lock(&h->lock);
 if(h->error){int error=h->error;pthread_mutex_unlock(&h->lock);return error;}
 bool had_value=value!=NULL;o=resolve_locked(h,o);if(value)value=resolve_locked(h,value);
 if(had_value&&!value){int error=fail_locked(h,GCS_INVALID);pthread_mutex_unlock(&h->lock);return error;}
 if(!valid(h,o)||!ref_slot(o,slot)||(value&&!valid(h,value))){int error=fail_locked(h,GCS_INVALID);pthread_mutex_unlock(&h->lock);return error;}
 gcs_object *old=(gcs_object *)atomic_load_explicit(&o->slots[slot],memory_order_acquire);
 /* SATB PRE barrier: enqueue the deleted snapshot edge before any store.
  * On overflow, reject the write and invalidate the cycle explicitly. */
 if(h->active&&old&&old->birth<=h->cutoff){
  h->barriers++;
  if(h->qcount==h->qcap){h->overflows++;int error=fail_locked(h,GCS_SATB_OVERFLOW);pthread_mutex_unlock(&h->lock);return error;}
  h->satb[(h->qhead+h->qcount)%h->qcap]=old;h->qcount++;h->queued++;
 }
 atomic_store_explicit(&o->slots[slot],(uintptr_t)value,memory_order_release);
 pthread_cond_signal(&h->wake);pthread_mutex_unlock(&h->lock);return GCS_OK;
}
int gcs_put_scalar(gcs_heap *h,gcs_object *o,size_t slot,uintptr_t value){
 if(!h)return GCS_INVALID;pthread_mutex_lock(&h->lock);
 int result=GCS_OK;
 o=resolve_locked(h,o);
 if(!valid(h,o)||slot>=o->nslots||ref_slot(o,slot))result=fail_locked(h,GCS_INVALID);
 else atomic_store_explicit(&o->slots[slot],value,memory_order_release);
 pthread_mutex_unlock(&h->lock);return result;
}
gcs_object *gcs_get_ref(gcs_heap *h,gcs_object *o,size_t slot){
 if(!h)return NULL;pthread_mutex_lock(&h->lock);
 o=resolve_locked(h,o);
 gcs_object *result=valid(h,o)&&ref_slot(o,slot)?(gcs_object*)atomic_load_explicit(&o->slots[slot],memory_order_acquire):NULL;
 if(result)result=resolve_locked(h,result);
 pthread_mutex_unlock(&h->lock);return result;
}
int gcs_begin(gcs_heap *h,gcs_object *const *roots,size_t count,bool hold){
 if(!h||(!roots&&count))return GCS_INVALID;
 pthread_mutex_lock(&h->lock);
 if(h->active||(h->started&&!h->joined)||h->error){pthread_mutex_unlock(&h->lock);return GCS_INVALID;}
 h->cutoff=h->sequence;h->top=h->qhead=h->qcount=0;
 h->marked=h->scanned=h->barriers=h->queued=h->new_skipped=h->duplicates=h->remarks=h->overflows=0;
 for(size_t i=0;i<h->count;i++)if(h->objects[i])atomic_fetch_and_explicit(&h->objects[i]->flags,~(uint32_t)DVM_GC_MARKED,memory_order_relaxed);
 h->active=true;h->held=hold;h->stop=h->ack=h->joined=false;h->mutator=pthread_self();
 for(size_t i=0;i<count&&!h->error;i++)offer_locked(h,roots[i]);
 if(h->error){h->active=false;int error=h->error;pthread_mutex_unlock(&h->lock);return error;}
 if(pthread_create(&h->worker,NULL,marker,h)){h->active=false;int error=fail_locked(h,GCS_THREAD_ERROR);pthread_mutex_unlock(&h->lock);return error;}
 h->started=true;
 while(!h->ack)pthread_cond_wait(&h->wake,&h->lock);
 pthread_mutex_unlock(&h->lock);return GCS_OK;
}
int gcs_offer_root(gcs_heap *h,gcs_object *o){
 if(!h)return GCS_INVALID;pthread_mutex_lock(&h->lock);
 int result=h->active&&!h->error?offer_locked(h,o):GCS_INVALID;
 pthread_mutex_unlock(&h->lock);return result;
}
int gcs_finish(gcs_heap *h){
 if(!h)return GCS_INVALID;pthread_mutex_lock(&h->lock);
 if(!h->started||h->joined){pthread_mutex_unlock(&h->lock);return GCS_INVALID;}
 h->held=false;pthread_cond_broadcast(&h->wake);
 struct timespec deadline;clock_gettime(CLOCK_REALTIME,&deadline);deadline.tv_sec+=5;
 while(!h->error&&(h->top||h->qcount)){
  if(pthread_cond_timedwait(&h->wake,&h->lock,&deadline)){fail_locked(h,GCS_TIMEOUT);break;}
 }
 /* Remark owns the same mutex as every pre-barrier/store. The worker scans
  * under that mutex too: no in-flight scan or producer can be hidden here.
  * Recheck both queues once while producers are stopped, before active=0. */
 if(!h->error){h->remarks++;atomic_thread_fence(memory_order_seq_cst);
  if(h->top||h->qcount)fail_locked(h,GCS_INVALID);}
 h->active=false;h->stop=true;pthread_cond_broadcast(&h->wake);
 int result=h->error;pthread_mutex_unlock(&h->lock);
 if(pthread_join(h->worker,NULL))return GCS_THREAD_ERROR;
 h->joined=true;return result;
}
bool gcs_marked(gcs_heap *h,gcs_object *o){
 if(!h)return false;pthread_mutex_lock(&h->lock);
 o=resolve_locked(h,o);
 bool result=valid(h,o)&&(atomic_load_explicit(&o->flags,memory_order_acquire)&DVM_GC_MARKED);
 pthread_mutex_unlock(&h->lock);return result;
}

#include "gc_isolated_evacuate.inc"

static void report(FILE *csv,const char *name,gcs_heap *h,int status){
 if(csv)fprintf(csv,"isolated-satb,%s,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%d,%d,0,0\n",name,h->marked,h->scanned,h->barriers,h->queued,h->new_skipped,h->duplicates,h->remarks,h->overflows,h->distinct_thread,status);
}
int gc_isolated_satb_selftest(FILE *out,FILE *csv){
 uint32_t edge[]={0};struct dvm_gc_layout empty={0},one={.instance_slots=edge,.instance_count=1};
 unsigned checks=0,failed=0;
 if(csv)fprintf(csv,"stage,case,marked,scanned,pre_barriers,satb_enqueued,new_skipped,duplicate_marks,remarks,overflows,distinct_marker_thread,status,real_vm_moving_enabled,real_vm_collection_enabled\n");
#define CHECK(name,condition) do{bool pass=(condition);checks++;failed+=!pass;if(out)fprintf(out,"GC4_%s %s\n",name,pass?"PASS":"FAIL");}while(0)
 gcs_heap *h=gcs_create(262144,1200,32);if(!h)return 1;
 gcs_object *a=gcs_alloc(h,&empty,0);int result=gcs_begin(h,&a,1,false);if(!result)result=gcs_finish(h);
 CHECK("a_single_root",result==0&&gcs_marked(h,a)&&h->marked==1&&h->scanned==1&&h->remarks==1&&h->distinct_thread);report(csv,"a",h,result);gcs_destroy(h);
 h=gcs_create(262144,1200,32);if(!h)return 1;
 gcs_object *root=NULL,*last=NULL;bool setup=true;
 for(unsigned i=0;i<1000;i++){gcs_object *o=gcs_alloc(h,&one,1);if(!o){setup=false;break;}if(last&&gcs_put_ref(h,last,0,o))setup=false;if(!root)root=o;last=o;}
 result=gcs_begin(h,&root,1,false);if(!result)result=gcs_finish(h);
 CHECK("b_depth1000_explicit_stack",setup&&result==0&&h->marked==1000&&h->scanned==1000);report(csv,"b",h,result);gcs_destroy(h);
 h=gcs_create(16384,32,8);if(!h)return 1;
 a=gcs_alloc(h,&one,1);gcs_object *b=gcs_alloc(h,&one,1),*c=gcs_alloc(h,&one,1);
 setup=a&&b&&c&&!gcs_put_ref(h,a,0,b)&&!gcs_put_ref(h,b,0,c)&&!gcs_put_ref(h,c,0,a);
 result=gcs_begin(h,&a,1,false);if(!result)result=gcs_finish(h);
 CHECK("c_depth3_cycle_once",setup&&result==0&&h->marked==3&&h->scanned==3);report(csv,"c",h,result);gcs_destroy(h);
 h=gcs_create(16384,32,8);if(!h)return 1;
 a=gcs_alloc(h,&one,1);b=gcs_alloc(h,&empty,0);c=gcs_alloc(h,&empty,0);
 setup=a&&b&&c&&!gcs_put_ref(h,a,0,b);result=gcs_begin(h,&a,1,true);
 bool unscanned=h->scanned==0;int write=gcs_put_ref(h,a,0,c);if(!result)result=gcs_finish(h);
 CHECK("d_deleted_edge_retained",setup&&unscanned&&write==0&&result==0&&gcs_marked(h,b)&&gcs_marked(h,c)&&h->queued==1&&h->scanned==3&&h->remarks==1&&h->distinct_thread);report(csv,"d",h,result);gcs_destroy(h);
 h=gcs_create(16384,32,8);if(!h)return 1;
 a=gcs_alloc(h,&one,1);result=gcs_begin(h,&a,1,true);c=gcs_alloc(h,&empty,0);
 write=gcs_put_ref(h,a,0,c);if(!result)result=gcs_finish(h);
 CHECK("e_null_to_new_memo",c&&write==0&&result==0&&gcs_marked(h,a)&&!gcs_marked(h,c)&&h->queued==0&&h->new_skipped==1);
 if(out)fprintf(out,"GC4_MEMO e new_object_birth_after_snapshot=1 excluded_from_this_cycle=1 mark_only_no_reclaim=1\n");report(csv,"e",h,result);gcs_destroy(h);
 h=gcs_create(16384,32,8);if(!h)return 1;
 a=gcs_alloc(h,&empty,0);result=gcs_begin(h,&a,1,true);bool offered=true;
 for(unsigned i=0;i<5;i++)if(gcs_offer_root(h,a))offered=false;
 if(!result)result=gcs_finish(h);
 CHECK("f_repeat5_idempotent",offered&&result==0&&h->marked==1&&h->scanned==1&&h->duplicates==5);report(csv,"f",h,result);gcs_destroy(h);
 h=gcs_create(16384,32,8);if(!h)return 1;
 a=gcs_alloc(h,&one,2);b=gcs_alloc(h,&empty,0);
 setup=a&&b&&!gcs_put_scalar(h,a,1,(uintptr_t)b);result=gcs_begin(h,&a,1,true);c=gcs_alloc(h,&empty,0);
 int offered_new=gcs_offer_root(h,c);if(!result)result=gcs_finish(h);
 CHECK("g_new_allocation_excluded",setup&&c&&offered_new==0&&result==0&&!gcs_marked(h,c)&&h->new_skipped==1);
 CHECK("precise_layout_skips_scalar_pointer",!gcs_marked(h,b)&&h->marked==1);report(csv,"g",h,result);gcs_destroy(h);
 h=gcs_create(16384,32,1);if(!h)return 1;
 a=gcs_alloc(h,&one,1);b=gcs_alloc(h,&empty,0);c=gcs_alloc(h,&empty,0);
 setup=a&&b&&c&&!gcs_put_ref(h,a,0,b);result=gcs_begin(h,&a,1,true);
 int first_write=gcs_put_ref(h,a,0,c),overflow=gcs_put_ref(h,a,0,b);
 gcs_object *unchanged=gcs_get_ref(h,a,0);int finish=gcs_finish(h);
 CHECK("h_overflow_explicit_write_rejected",setup&&result==0&&first_write==0&&overflow==GCS_SATB_OVERFLOW&&finish==GCS_SATB_OVERFLOW&&unchanged==c&&h->overflows==1);
 if(out)fprintf(out,"GC4_EXPECTED_ERROR type=SATB_QUEUE_OVERFLOW code=%d capacity=1 attempted_write_rejected=1\n",overflow);
 report(csv,"h",h,finish);gcs_destroy(h);
 if(out)fprintf(out,"GC4_ISOLATED_SELFTEST %s checks=%u failed=%u real_vm_moving_enabled=0 real_vm_collection_enabled=0\n",failed?"FAIL":"PASS",checks,failed);
#undef CHECK
 return failed?1:0;
}
