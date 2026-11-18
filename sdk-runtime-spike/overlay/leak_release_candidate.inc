/* Candidate: existing global GIL serializes all slot/queue/object operations.
 * Worker sees only atomics; no new mutex, export lock, or IO on VM/frame paths.
 * No quarantine, cumulative diagnostic table, or runtime observer sampling. */
#include <stdatomic.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>
#include "leak_scope.h"
int sdk_trace_fprintf(FILE *,const char *,...);
#ifndef LR_DIR
#define LR_DIR "/data/storage/el2/base/haps/entry/files/sdkdex/"
#endif
#define LR_CAP 256u
struct lr_pair {struct dvm *vm;uint32_t a,b;uint64_t ai,bi,returned;int64_t value;void (*destroy)(void *);unsigned next;bool used;};
static struct lr_pair lr_pairs[LR_CAP];
static unsigned lr_queue[LR_CAP],lr_head,lr_tail,lr_pending,lr_free,lr_used;
static bool lr_queue_ready;
static _Atomic bool lr_on,lr_started,lr_fault;
static _Atomic uint64_t lr_freed_pairs,lr_freed_bytes,lr_blocked,lr_rejected,lr_overflow,lr_cancelled,lr_pending_count;
static _Atomic uint64_t lr_frames,lr_gt33,lr_max_frame;
static _Thread_local void *lr_allowed[4];
static _Thread_local bool lr_freeing;
static _Thread_local uint64_t lr_bytes;
static uint64_t lr_now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000ull+t.tv_nsec;}
static int lr_read_control(void){
 int fd=open(LR_DIR "leak-release-control",O_RDONLY|O_CLOEXEC);if(fd<0)return errno==ENOENT?1:0;
 char b[16];ssize_t n=read(fd,b,sizeof b);close(fd);
 if(n<1||n==(ssize_t)sizeof b)return 0;
 if(b[0]!='0'&&b[0]!='1')return 0;
 for(ssize_t i=1;i<n;i++)if(b[i]!='\n'&&b[i]!='\r'&&b[i]!=' '&&b[i]!='\t')return 0;
 return b[0]=='1';
}
void lr_start(void){
 if(atomic_exchange(&lr_started,true))return;
 /* hdc transfers may create 0660; shell cannot chmod an app-owned inode.
  * Repair as its owner before reading, at startup only, never on a callback. */
 int owned=open(LR_DIR "leak-release-control",O_RDONLY|O_CLOEXEC);
 if(owned>=0){if(fchmod(owned,0644)){atomic_store(&lr_fault,true);sdk_trace_fprintf(stderr,"LEAK_RELEASE_ERROR control_mode_repair_failed errno=%d\n",errno);}close(owned);}
 bool on=lr_read_control()==1;atomic_store(&lr_on,on);
 int fd=open(LR_DIR "leak-release-control",O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0644);
 if(fd>=0){(void)fchmod(fd,0644);if(write(fd,"1\n",2)!=2)atomic_store(&lr_fault,true);close(fd);}
 sdk_trace_fprintf(stderr,"LEAK_RELEASE_CONFIG enabled=%d default=1 restart_required=1 delay_ns=2000000 moving=0 collection=0\n",on);
}
bool lr_enabled(void){return atomic_load_explicit(&lr_on,memory_order_relaxed)&&!atomic_load(&lr_fault);}
void lr_denied(void){atomic_store(&lr_fault,true);atomic_store(&lr_on,false);}
void lr_arena_released(void *p,size_t span){
 if(lr_freeing)for(unsigned j=0;j<4;j++)if(p==lr_allowed[j]){lr_bytes+=span;break;}
}
static void lr_slot_drop(unsigned i){lr_pairs[i].used=false;lr_pairs[i].vm=NULL;lr_pairs[i].next=lr_free;lr_free=i;lr_used--;}
static bool lr_plain_array(struct dvm_object *o){return !o->data_destroy&&!o->data_owner&&!o->utf8&&!o->string_units&&!o->slots;}
static bool lr_owned_box(struct dvm_object *o,struct lr_pair *p){
 int64_t v=0;if(o->data)memcpy(&v,o->data,sizeof v);
 return o->cls&&!o->cls->dex&&o->cls->name&&!strcmp(o->cls->name,"java/lang/Long")&&o->kind==DVM_OBJ_PLAIN&&o->data&&o->data_destroy&&o->data_destroy==p->destroy&&!o->data_owner&&!o->slots&&!o->utf8&&!o->string_units&&v==p->value;
}
unsigned lr_register(struct dvm *v,uint32_t a,uint32_t b,int64_t value){
 if(!lr_enabled()||!v||!dvm_gil_held(v)||!dvm_leak_collection_off(v)||v->gc_destroying)return 0;
 struct dvm_object *x=heap_slot(v,a),*y=heap_slot(v,b);
 if(!x||!y||!x->live||!y->live||x->kind!=DVM_OBJ_ARRAY||x->length!=1||x->elem_kind!='L'||!x->data||((uint32_t*)x->data)[0]!=b||!value||(value>=-128&&value<=127)||!leak_scope_owned_long(v,b,value)){atomic_fetch_add(&lr_rejected,1);return 0;}
 if(!lr_queue_ready){for(unsigned j=0;j<LR_CAP;j++)lr_pairs[j].next=j+1;lr_free=0;lr_queue_ready=true;}
 if(lr_free==LR_CAP){atomic_fetch_add(&lr_overflow,1);return 0;}
 unsigned i=lr_free;lr_free=lr_pairs[i].next;lr_used++;
 lr_pairs[i]=(struct lr_pair){.vm=v,.a=a,.b=b,.ai=x->identity_generation,.bi=y->identity_generation,.value=value,.destroy=y->data_destroy,.used=true};return i+1;
}
void lr_returned(unsigned token){
 if(!token||token>LR_CAP)return;unsigned i=token-1;struct lr_pair *p=&lr_pairs[i];
 if(!p->used||!dvm_gil_held(p->vm)||p->returned)return;
 if(!lr_enabled()){lr_slot_drop(i);atomic_fetch_add(&lr_cancelled,1);return;}
 p->returned=lr_now();lr_queue[lr_tail++%LR_CAP]=i;lr_pending++;atomic_store(&lr_pending_count,lr_pending);
}
void lr_poll(struct dvm *v){
 if(!v||!dvm_gil_held(v)||!lr_pending||v->gc_destroying||v->gc_initializing)return;
 uint64_t now=lr_now();unsigned budget=lr_pending<32?lr_pending:32;
 for(unsigned j=0;j<budget;j++){
  unsigned i=lr_queue[lr_head++%LR_CAP];lr_pending--;struct lr_pair *p=&lr_pairs[i];
  if(!lr_enabled()){lr_slot_drop(i);atomic_fetch_add(&lr_cancelled,1);continue;}
  if(p->vm!=v||now-p->returned<2000000ull){lr_queue[lr_tail++%LR_CAP]=i;lr_pending++;continue;}
  if(!dvm_leak_collection_off(v)){lr_slot_drop(i);atomic_fetch_add(&lr_rejected,1);continue;}
  struct dvm_object *a=heap_slot(v,p->a),*b=heap_slot(v,p->b);
  if(!a||!b||!a->live||!b->live||a->identity_generation!=p->ai||b->identity_generation!=p->bi){lr_slot_drop(i);atomic_fetch_add(&lr_rejected,1);continue;}
  if(a->pins||b->pins||a->gc_strong_handles||b->gc_strong_handles||a->gc_body_borrows||b->gc_body_borrows||a->gc_payload_borrows||b->gc_payload_borrows||a->monitor_depth||b->monitor_depth){atomic_fetch_add(&lr_blocked,1);lr_queue[lr_tail++%LR_CAP]=i;lr_pending++;continue;}
  if(!lr_plain_array(a)||!lr_owned_box(b,p)||a->kind!=DVM_OBJ_ARRAY||a->length!=1||a->elem_kind!='L'||!a->data||((uint32_t*)a->data)[0]!=p->b){lr_slot_drop(i);atomic_fetch_add(&lr_rejected,1);continue;}
  lr_allowed[0]=a->data;lr_allowed[1]=a->slots;lr_allowed[2]=b->data;lr_allowed[3]=b->slots;lr_freeing=true;lr_bytes=0;
  heap_free(v,p->a);heap_free(v,p->b);lr_freeing=false;
  if(a->live||b->live){lr_denied();sdk_trace_fprintf(stderr,"LEAK_RELEASE_FAILED identity_checked=1\n");}
  else {atomic_fetch_add(&lr_freed_pairs,1);atomic_fetch_add(&lr_freed_bytes,lr_bytes);}
  lr_slot_drop(i);
 }
 atomic_store(&lr_pending_count,lr_pending);
}
void lr_forget(struct dvm *v){
 /* Called after all of this VM's threads finish. Existing global GIL keeps
  * the shared queue exclusive; no new lock is introduced. */
 dvm_gil_acquire(v);unsigned n=lr_pending;
 for(unsigned j=0;j<n;j++){unsigned i=lr_queue[lr_head++%LR_CAP];lr_pending--;if(lr_pairs[i].vm==v){lr_slot_drop(i);atomic_fetch_add(&lr_cancelled,1);}else {lr_queue[lr_tail++%LR_CAP]=i;lr_pending++;}}
 for(unsigned i=0;i<LR_CAP;i++)if(lr_pairs[i].used&&lr_pairs[i].vm==v){lr_slot_drop(i);atomic_fetch_add(&lr_cancelled,1);}
 atomic_store(&lr_pending_count,lr_pending);dvm_gil_release(v);
}
void lr_frame(uint64_t now){
 static _Thread_local uint64_t previous;if(previous&&now>previous){uint64_t dt=now-previous;atomic_fetch_add_explicit(&lr_frames,1,memory_order_relaxed);if(dt>33000000)atomic_fetch_add_explicit(&lr_gt33,1,memory_order_relaxed);uint64_t old=atomic_load_explicit(&lr_max_frame,memory_order_relaxed);while(dt>old&&!atomic_compare_exchange_weak_explicit(&lr_max_frame,&old,dt,memory_order_relaxed,memory_order_relaxed)){} }previous=now;
}
void lr_worker(void){
 static uint64_t last,previous_live,previous_ns,hour;uint64_t now=lr_now();
 if(!atomic_load(&lr_started)||now-last<60000000000ull)return;
 struct vm_arena_region_snapshot s={0};if(!vm_arena_region_snapshot(&s)||!s.active)return;
 if(s.denied||s.limit_bytes!=268435456)lr_denied();
 /* Snapshot and atomics only above. All formatting/IO below, outside arena/GIL. */
 double rate=previous_ns?((double)s.live_bytes-(double)previous_live)/1024.0/((now-previous_ns)/1e9):0;
 char text[2048];int n=snprintf(text,sizeof text,"pid=%d enabled=%d default_enabled=1 restart_required=1 mono_ns=%llu freed_pairs=%llu freed_bytes=%llu live_charge=%zu committed=%zu denied=%zu rate_valid=%d rate_interval_s=%.3f rate_60s_KiB_s=%.6f pending=%llu blocked=%llu rejected=%llu overflow=%llu cancelled=%llu fault=%d moving=0 collection=0 arena_limit=%zu frames_n=%llu frames_gt33=%llu frames_max_ns=%llu frame_max_scope=last_status_interval frame_metric=nativeRender_interval_not_physical\n",getpid(),lr_enabled(),(unsigned long long)now,(unsigned long long)atomic_load(&lr_freed_pairs),(unsigned long long)atomic_load(&lr_freed_bytes),s.live_bytes,s.committed_bytes,s.denied,previous_ns!=0,previous_ns?(now-previous_ns)/1e9:0,rate,(unsigned long long)atomic_load(&lr_pending_count),(unsigned long long)atomic_load(&lr_blocked),(unsigned long long)atomic_load(&lr_rejected),(unsigned long long)atomic_load(&lr_overflow),(unsigned long long)atomic_load(&lr_cancelled),atomic_load(&lr_fault),s.limit_bytes,(unsigned long long)atomic_load(&lr_frames),(unsigned long long)atomic_load(&lr_gt33),(unsigned long long)atomic_exchange(&lr_max_frame,0));
 int fd=open(LR_DIR "leak-release-status.txt.part",O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC,0644);
 if(fd>=0){(void)fchmod(fd,0644);bool ok=n>0&&n<(int)sizeof text&&write(fd,text,n)==n;if(close(fd))ok=false;if(ok)rename(LR_DIR "leak-release-status.txt.part",LR_DIR "leak-release-status.txt");else atomic_store(&lr_fault,true);}else atomic_store(&lr_fault,true);
 if(!hour||now-hour>=3600000000000ull){
  fd=open(LR_DIR "leak-release-hourly.csv",O_WRONLY|O_CREAT|O_APPEND|O_CLOEXEC,0644);if(fd>=0){(void)fchmod(fd,0644);char b[256];int m=snprintf(b,sizeof b,"%d,%llu,%zu,%zu,%zu\n",getpid(),(unsigned long long)now,s.live_bytes,s.committed_bytes,s.denied);if(write(fd,b,m)!=m)atomic_store(&lr_fault,true);close(fd);}else atomic_store(&lr_fault,true);hour=now;
 }
 previous_live=s.live_bytes;previous_ns=last=now;
}
