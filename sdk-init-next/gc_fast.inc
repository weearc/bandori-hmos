/* Non-moving full-STW collection. An explicit mutator rendezvous is confirmed
 * under the GIL, which stays held through mark AND sweep. Unknown native edge
 * extents remain fail-safe: STW does not make an unknown graph traceable. */
#include <setjmp.h>
#include <dlfcn.h>
/* Collector scratch must not consume ordinary guest arena headroom. Bounded
 * by heap slot count and reused until VM shutdown; never a guest payload. */
/* The candidate force-includes allocator headers before feature macros. */
extern int pthread_getattr_np(pthread_t,pthread_attr_t *);
#ifdef fprintf
#pragma push_macro("fprintf")
#undef fprintf
#define GC_FAST_RESTORE_FPRINTF
#endif
#define GC_FAST_DIR "/data/storage/el2/base/haps/entry/files/sdkdex/"
#define GC_STW_MAX_THREADS 128
struct gc_stw_row {long tid;unsigned state;bool acknowledged;uint64_t last_ns;char where[384];};
struct gc_stw_receipt {uint64_t epoch;long coordinator;unsigned total,arrived;bool ok;const char *failure;struct gc_stw_row rows[GC_STW_MAX_THREADS];};
static void gc_stw_thread_waiting(struct dvm *);
static void gc_stw_thread_acquired(struct dvm *);
static void gc_stw_thread_parked(struct dvm *);
static bool gc_stw_begin(struct dvm *,struct gc_stw_receipt *);
static void gc_stw_end(void);
static void gc_stw_write(const struct gc_stw_receipt *,uint64_t);
struct gc_fast_range {uintptr_t first,last,max_last;dvm_ref ref;};
struct gc_fast_slice {const char *phase,*last_class;uint64_t ns;dvm_ref last_ref;};
struct gc_fast_class_count {struct dvm_class *cls;size_t count[3];uintptr_t destructor[3];bool retention_known,retention;};
struct gc_fast_mark {struct dvm *vm;dvm_ref *work;size_t count,capacity;bool failed;
 struct gc_fast_range *ranges;size_t nranges,range_capacity;const char *failure;unsigned char *weak_targets;
 struct gc_fast_slice *slices;size_t nslices,slice_capacity;uint64_t slice_begin,snapshot_identity;
 struct gc_fast_class_count *classes;size_t class_capacity,unknown,atomic,refs,quarantine,inventory_overflow;
 const char *retain_reason,*last_class;dvm_ref last_ref;bool retain_all,young_only;};
static uint64_t gc_fast_ns(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000u+(uint64_t)t.tv_nsec;}
/* Buffered native destruction probe. This is diagnostics, not a deferred
 * destruction queue. No native cleanup or guest finalization is skipped. */
struct gc_destroy_sample {uint64_t cycle,start,finalized,values,arrays,shared,end;size_t cells;int had_stmt;dvm_ref ref;const char *kind,*cls;};
static struct gc_destroy_sample gc_destroy_samples[128];
static size_t gc_destroy_count,gc_destroy_dropped;
static pthread_mutex_t gc_destroy_lock=PTHREAD_MUTEX_INITIALIZER;
static _Atomic uint64_t gc_destroy_cycle;
bool dvm_gc_native_destroy_defer_enabled(void){return atomic_load_explicit(&gc_destroy_cycle,memory_order_acquire)!=0;}
static void gc_destroy_record(struct gc_destroy_sample value){
 value.cycle=atomic_load_explicit(&gc_destroy_cycle,memory_order_acquire);if(!value.cycle)return;
 pthread_mutex_lock(&gc_destroy_lock);if(gc_destroy_count<128)gc_destroy_samples[gc_destroy_count++]=value;else gc_destroy_dropped++;pthread_mutex_unlock(&gc_destroy_lock);
}
void dvm_gc_cursor_destroy_note(uint64_t start,uint64_t finalized,uint64_t values,uint64_t arrays,uint64_t shared,uint64_t end,size_t cells,int had_stmt){
 gc_destroy_record((struct gc_destroy_sample){.start=start,.finalized=finalized,.values=values,.arrays=arrays,.shared=shared,.end=end,.cells=cells,.had_stmt=had_stmt,.kind="cursor_payload",.cls="android/database/Cursor"});
}
static void gc_destroy_flush(void){
 struct gc_destroy_sample rows[128];size_t n,dropped;
 pthread_mutex_lock(&gc_destroy_lock);n=gc_destroy_count;dropped=gc_destroy_dropped;memcpy(rows,gc_destroy_samples,n*sizeof *rows);gc_destroy_count=gc_destroy_dropped=0;pthread_mutex_unlock(&gc_destroy_lock);
 static pid_t pid;bool first=pid!=getpid();pid=getpid();FILE *f=fopen(GC_FAST_DIR "gc-destroy.csv",first?"w":"a");if(!f)return;
 if(first)fputs("pid,mono_ms,kind,class,ref,total_ms,finalize_ms,values_ms,arrays_ms,shared_release_ms,free_ms,cells,had_stmt,dropped_records\n",f);
 for(size_t i=0;i<n;i++){struct gc_destroy_sample *r=&rows[i];fprintf(f,"%d,%llu,%s,%s,%u,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%zu,%d,%zu\n",getpid(),(unsigned long long)r->cycle,r->kind,r->cls?r->cls:"none",r->ref,(double)(r->end-r->start)/1e6,r->finalized?(double)(r->finalized-r->start)/1e6:0,r->values?(double)(r->values-r->finalized)/1e6:0,r->arrays?(double)(r->arrays-r->values)/1e6:0,r->shared?(double)(r->shared-r->arrays)/1e6:0,r->shared?(double)(r->end-r->shared)/1e6:0,r->cells,r->had_stmt,dropped);}
 if(!n)fprintf(f,"%d,0,no_samples,none,0,0,0,0,0,0,0,0,0,%zu\n",getpid(),dropped);
 fclose(f);
}
static bool gc_fast_running,gc_fast_enabled,gc_fast_initialized,gc_fast_rollback_latched;
bool dvm_leak_collection_off(struct dvm *vm){
 return vm&&dvm_gil_held(vm)&&!gc_fast_running&&!gc_fast_enabled;
}
static bool gc_young_only=true,gc_young_selftested;
#define GC_YOUNG_PROMOTION_AGE 2u
#define GC_YOUNG_TRIGGER_BYTES (24u*1024u*1024u)
static unsigned gc_fast_poll;
static uint64_t gc_fast_last,gc_fast_handoffs,gc_fast_last_unknown,gc_fast_last_quarantine,gc_fast_last_handoffs;
static struct dvm *gc_fast_vm;
static pthread_t gc_fast_owner;
static uint64_t gc_fast_owner_acquired_ns;
void dvm_gc_fast_retention_counts(uint64_t *c,uint64_t *q,uint64_t *h){
 if(c)*c=gc_fast_last_unknown;if(q)*q=gc_fast_last_quarantine;if(h)*h=gc_fast_last_handoffs;
}
/* Called ONLY with GIL ownership, on both acquisition paths. A native call may
 * write through a previously borrowed payload, so a DEX-store-only dirty bit
 * is insufficient. Until all such stores have barriers, any other VM entrant
 * conservatively invalidates the remaining sweep candidates. */
static void gc_fast_gil_acquired(struct dvm *vm){
 gc_stw_thread_acquired(vm);
 if(gc_fast_running&&gc_fast_vm==vm){if(!pthread_equal(pthread_self(),gc_fast_owner))++gc_fast_handoffs;else gc_fast_owner_acquired_ns=gc_fast_ns();}
}
static void gc_fast_slice_end(struct gc_fast_mark *s,const char *phase,bool force){
 uint64_t now=gc_fast_ns();if(!force&&now-s->slice_begin<250000u)return;
 if(s->slices&&s->nslices<s->slice_capacity)s->slices[s->nslices++]=(struct gc_fast_slice){.phase=phase,.last_class=s->last_class,.ns=now-s->slice_begin,.last_ref=s->last_ref};
 else {s->failed=true;s->failure="slice_log_capacity";}
 s->slice_begin=now; /* phase interval, NOT a separately resumed pause */
}
static struct gc_fast_class_count *gc_fast_class_entry(struct gc_fast_mark *s,struct dvm_class *cls){
 size_t i=((uintptr_t)cls>>4)&(s->class_capacity-1),probes=0;
 while(s->classes[i].cls&&s->classes[i].cls!=cls){i=(i+1)&(s->class_capacity-1);if(++probes==s->class_capacity){s->inventory_overflow++;return NULL;}}
 s->classes[i].cls=cls;return &s->classes[i];
}
static bool gc_fast_class_retention(struct gc_fast_mark *s,struct dvm_class *cls){
 struct gc_fast_class_count *entry=gc_fast_class_entry(s,cls);
 if(entry&&entry->retention_known)return entry->retention;
 bool retain=false;
 for(struct dvm_class *c=cls;c;c=c->super){
  if(c->name&&(!strcmp(c->name,"java/lang/ref/Reference")||!strcmp(c->name,"java/lang/ref/FinalizerReference")))retain=true;
  for(int i=0;i<c->nmethods;i++)if(c->name&&strcmp(c->name,"java/lang/Object")&&!strcmp(c->methods[i].name,"finalize")&&!strcmp(c->methods[i].sig,"()V"))retain=true;
 }
 if(entry){entry->retention_known=true;entry->retention=retain;}return retain;
}
static void gc_fast_classify(struct gc_fast_mark *s,struct dvm_object *o){
 if(!dvm_gc_payload_requires_visitor(o))return;
 int kind=dvm_gc_payload_visit(s->vm,o,NULL,NULL);
 unsigned category=kind==DVM_GC_PAYLOAD_ATOMIC?0:kind==DVM_GC_PAYLOAD_REFS?1:2;
 if(category==0)++s->atomic;else if(category==1)++s->refs;else{++s->unknown;s->retain_all=true;if(!s->retain_reason)s->retain_reason="unknown_payload_transitive_extent";}
 struct gc_fast_class_count *entry=gc_fast_class_entry(s,o->cls);if(!entry)return;
 ++entry->count[category];entry->destructor[category]=(uintptr_t)o->data_destroy;
}
/* Linear heap pass, hash aggregation, and diagnostic IO with GIL RELEASED.
 * The previous nclasses*heap_size inventory added ~400ms of hidden STW. */
static void gc_fast_inventory_write(struct gc_fast_mark *s,uint64_t cycle){
 static pid_t csv_pid;pid_t pid=getpid();FILE *f=fopen(GC_FAST_DIR "gc-payload-inventory.csv",csv_pid==pid?"a":"w");if(!f)return;
 if(csv_pid!=pid)fputs("pid,mono_ms,class,count,category,destructor,module_offset\n",f);csv_pid=pid;
 const char *names[]={"A_native_atomic","B_guest_refs","C_conservative_retain"};
 for(size_t i=0;i<s->class_capacity;i++){struct gc_fast_class_count *c=&s->classes[i];if(!c->cls)continue;
  for(unsigned k=0;k<3;k++)if(c->count[k]){Dl_info info={0};bool found=dladdr((void *)c->destructor[k],&info)!=0;fprintf(f,"%d,%llu,%s,%zu,%s,0x%llx,0x%llx\n",pid,(unsigned long long)cycle,c->cls->name,c->count[k],names[k],(unsigned long long)c->destructor[k],(unsigned long long)(found?c->destructor[k]-(uintptr_t)info.dli_fbase:0));}
 }fclose(f);
}
static void gc_fast_mark_slot(dvm_ref *slot,void *user){
 struct gc_fast_mark *s=user;gc_fast_slice_end(s,"mark_edges",false);if(!slot||!*slot)return;struct dvm_object *o=gc_peek_object(s->vm,*slot);
 /* Validate even a root beyond the snapshot; exempt only a VALID new object. */
 if(!o){s->failed=true;s->failure="invalid_typed_root_or_edge";return;}
 if(*slot>s->capacity)return; /* valid post-snapshot allocations are never swept */
 if(s->young_only&&(o->gc_flags&DVM_GC_OLD))return;
 if(o->gc_flags&DVM_GC_MARKED)return;
 if(s->count==s->capacity){s->failed=true;s->failure="mark_stack_capacity";return;}
 o->gc_flags|=DVM_GC_MARKED;s->work[s->count++]=*slot;
}
static void gc_fast_candidate(struct gc_fast_mark *s,uint32_t r){if(gc_peek_object(s->vm,r))gc_fast_mark_slot(&r,s);}
static void gc_fast_keep_weak(dvm_ref *slot,void *user){struct gc_fast_mark *s=user;if(slot&&*slot&&*slot<=s->capacity)s->weak_targets[*slot]=1;gc_fast_mark_slot(slot,user);}
static int gc_fast_range_order(const void *a,const void *b){const struct gc_fast_range *x=a,*y=b;return x->first<y->first?-1:x->first>y->first;}
static void gc_fast_range_add(struct gc_fast_mark *s,const void *p,size_t n,dvm_ref ref){
 if(!p||!n)return;uintptr_t first=(uintptr_t)p;
 if(n>UINTPTR_MAX-first||s->nranges==s->range_capacity){s->failed=true;s->failure="native_range_capacity_or_overflow";return;}
 s->ranges[s->nranges++]=(struct gc_fast_range){first,first+n,0,ref};
}
struct gc_fast_pool {void *work,*weak,*ranges,*classes,*slices;size_t work_n,weak_n,ranges_n,classes_n,slices_n;};
static struct gc_fast_pool gc_fast_pool;
static bool gc_fast_reserve(void **p,size_t *capacity,size_t bytes){
 if(*capacity>=bytes)return true;size_t n=*capacity?*capacity:4096;while(n<bytes){if(n>SIZE_MAX/2)return false;n*=2;}
 void *next=vm_gc_scratch_alloc(n);if(!next)return false;vm_gc_scratch_free(*p);*p=next;*capacity=n;return true;
}
static void gc_fast_pool_destroy(void){vm_gc_scratch_free(gc_fast_pool.work);vm_gc_scratch_free(gc_fast_pool.weak);vm_gc_scratch_free(gc_fast_pool.ranges);vm_gc_scratch_free(gc_fast_pool.classes);vm_gc_scratch_free(gc_fast_pool.slices);memset(&gc_fast_pool,0,sizeof gc_fast_pool);}
static bool gc_fast_build_ranges(struct gc_fast_mark *s){
 if(s->capacity>SIZE_MAX/(6*sizeof(struct gc_fast_range)))return false;
 s->range_capacity=s->capacity*6;
 if(!gc_fast_reserve(&gc_fast_pool.ranges,&gc_fast_pool.ranges_n,(s->range_capacity?s->range_capacity:1)*sizeof *s->ranges))return false;s->ranges=gc_fast_pool.ranges;
 for(dvm_ref r=1;r<=s->vm->heap_size;r++){
  struct dvm_object *o=gc_peek_object(s->vm,r);if(!o)continue;
  gc_fast_range_add(s,o,sizeof *o,r);
  if(o->slots&&o->cls&&o->cls->islots>0)gc_fast_range_add(s,o->slots,(size_t)o->cls->islots*sizeof(union dvm_value),r);
  gc_fast_range_add(s,o->utf8,o->builder_capacity?o->builder_capacity:(size_t)o->utf8_len+1,r);
  gc_fast_range_add(s,o->string_units,((size_t)o->string_length+1)*sizeof(uint16_t),r);
  if(o->kind==DVM_OBJ_ARRAY)gc_fast_range_add(s,o->data,(size_t)o->length*dvm__elem_width(o->elem_kind),r);
  else gc_fast_range_add(s,o->data,1,r);
  if((r&31u)==0)gc_fast_slice_end(s,"native_ranges",false);
 }
 gc_fast_slice_end(s,"native_ranges",true);
 qsort(s->ranges,s->nranges,sizeof *s->ranges,gc_fast_range_order);uintptr_t last=0;
 for(size_t i=0;i<s->nranges;i++){if(s->ranges[i].last>last)last=s->ranges[i].last;s->ranges[i].max_last=last;}
 gc_fast_slice_end(s,"range_sort",true);return !s->failed;
}
static void gc_fast_pointer(struct gc_fast_mark *s,uintptr_t pointer){
 if(!pointer)return;
 /* Native body/interior payload pointers are roots too, not just guest IDs.
  * Include external array payloads; overlapping/shared ranges retain all owners. */
 size_t lo=0,hi=s->nranges;while(lo<hi){size_t mid=lo+(hi-lo)/2;if(s->ranges[mid].first<=pointer)lo=mid+1;else hi=mid;}
 while(lo){struct gc_fast_range *r=&s->ranges[--lo];if(r->max_last<=pointer)break;if(r->first<=pointer&&pointer<r->last)gc_fast_mark_slot(&r->ref,s);}
}
static void gc_fast_words(struct gc_fast_mark *s,const void *p,size_t n){
 const unsigned char *b=p;for(size_t i=0;i+4<=n;i+=4){uint32_t r;memcpy(&r,b+i,4);gc_fast_candidate(s,r);}
 for(size_t i=0;i+sizeof(uintptr_t)<=n;i+=sizeof(uintptr_t)){uintptr_t pointer;memcpy(&pointer,b+i,sizeof pointer);gc_fast_pointer(s,pointer);}
}
static int gc_native_word_cmp(const void *a,const void *b){uintptr_t x=*(const uintptr_t *)a,y=*(const uintptr_t *)b;return x<y?-1:x>y;}
static void gc_native_capture(struct gc_fast_mark *s,const void *p,size_t n,uintptr_t *words,size_t *count){
 const unsigned char *b=p;
 for(size_t i=0;i+4<=n;i+=4){dvm_ref r;memcpy(&r,b+i,4);gc_fast_candidate(s,r);}
 for(size_t i=0;i+sizeof(uintptr_t)<=n;i+=sizeof(uintptr_t)){uintptr_t v;memcpy(&v,b+i,sizeof v);if(v)words[(*count)++]=v;}
}
static void gc_native_interval(struct gc_fast_mark *s,uintptr_t *words,size_t count,const void *p,size_t n,dvm_ref owner){
 if(!p||!n)return;uintptr_t first=(uintptr_t)p;if(n>UINTPTR_MAX-first){s->failed=true;s->failure="native_range_overflow";return;}
 size_t lo=0,hi=count;while(lo<hi){size_t mid=lo+(hi-lo)/2;if(words[mid]<first)lo=mid+1;else hi=mid;}
 if(lo<count&&words[lo]<first+n)gc_fast_mark_slot(&owner,s);
}
static bool gc_fast_native_stack(struct gc_fast_mark *s){
 jmp_buf registers;memset(&registers,0,sizeof registers);(void)setjmp(registers);
 pthread_attr_t a;void *base=NULL;size_t length=0;if(pthread_getattr_np(pthread_self(),&a))return false;
 int rc=pthread_attr_getstack(&a,&base,&length);pthread_attr_destroy(&a);
 uintptr_t here=(uintptr_t)&a,lo=(uintptr_t)base,hi=lo+length;
 if(rc||hi<lo||here<lo||here>=hi)return false;here&=~(uintptr_t)(sizeof(uintptr_t)-1);
 size_t capacity=(hi-here)/sizeof(uintptr_t)+sizeof registers/sizeof(uintptr_t)+2;
 if(capacity>SIZE_MAX/sizeof(uintptr_t)||!gc_fast_reserve(&gc_fast_pool.ranges,&gc_fast_pool.ranges_n,capacity*sizeof(uintptr_t)))return false;
 uintptr_t *words=gc_fast_pool.ranges;size_t count=0;
 gc_native_capture(s,&registers,sizeof registers,words,&count);gc_native_capture(s,(void *)here,hi-here,words,&count);
 qsort(words,count,sizeof *words,gc_native_word_cmp);size_t unique=0;for(size_t i=0;i<count;i++)if(!unique||words[i]!=words[unique-1])words[unique++]=words[i];
 /* Same conservative intervals as the old 6*heap range sort, including
  * overlapping owners and native interior array/string/slot pointers. */
 for(dvm_ref r=1;r<=s->capacity;r++){
  struct dvm_object *o=gc_peek_object(s->vm,r);if(!o)continue;
  if(s->young_only&&(o->gc_flags&DVM_GC_OLD))continue; /* old addresses need no retention; old edges are scanned separately */
  gc_native_interval(s,words,unique,o,sizeof *o,r);
  if(o->slots&&o->cls&&o->cls->islots>0)gc_native_interval(s,words,unique,o->slots,(size_t)o->cls->islots*sizeof(union dvm_value),r);
  gc_native_interval(s,words,unique,o->utf8,o->builder_capacity?o->builder_capacity:(size_t)o->utf8_len+1,r);
  gc_native_interval(s,words,unique,o->string_units,((size_t)o->string_length+1)*sizeof(uint16_t),r);
  if(o->kind==DVM_OBJ_ARRAY)gc_native_interval(s,words,unique,o->data,(size_t)o->length*dvm__elem_width(o->elem_kind),r);
  else gc_native_interval(s,words,unique,o->data,1,r);
  gc_fast_slice_end(s,"native_stack",false);
 }
 return !s->failed;
}
static void gc_fast_configuration(struct dvm *vm){
 perf_config_poll();
 if(gc_fast_vm!=vm){gc_fast_vm=vm;gc_fast_initialized=false;gc_fast_enabled=false;gc_fast_last=0;gc_fast_rollback_latched=false;gc_young_selftested=false;}
 if(!gc_fast_initialized){
  FILE *manual=fopen(GC_FAST_DIR "gc-collection-now","wx");if(manual){fputs("0\n",manual);fclose(manual);chmod(GC_FAST_DIR "gc-collection-now",0666);}
  if(!access(GC_FAST_DIR "gc-collection-dirty",F_OK)){
   gc_fast_rollback_latched=true;
   FILE *f=fopen(GC_FAST_DIR "gc-collection-enabled","w");if(f){fputs("0\n",f);fclose(f);}
   fprintf(stderr,"GC_FAST_ROLLBACK reason=unclean_previous_process collection_enabled=0 moving_enabled=0\n");
  }
  gc_fast_initialized=true;
 }
 FILE *young=fopen(GC_FAST_DIR "gc_young_only","r");int young_flag=1;
 if(young){if(fscanf(young,"%d",&young_flag)!=1)young_flag=0;fclose(young);}
 else if(errno==ENOENT){young=fopen(GC_FAST_DIR "gc_young_only","wx");if(young){fputs("1\n",young);fclose(young);chmod(GC_FAST_DIR "gc_young_only",0666);}}
 if(gc_young_only!=(young_flag==1))fprintf(stderr,"GC_YOUNG_MODE pid=%d young_only=%d moving_enabled=0\n",getpid(),young_flag==1);
 gc_young_only=young_flag==1;
 FILE *f=fopen(GC_FAST_DIR "gc-collection-enabled","r");int flag=0;
 if(f){(void)fscanf(f,"%d",&flag);fclose(f);}
 else if(errno==ENOENT&&!gc_fast_rollback_latched){f=fopen(GC_FAST_DIR "gc-collection-enabled","wx");if(f){flag=0;fputs("0\n",f);fclose(f);}}
 bool enabled=flag==1&&!gc_fast_rollback_latched;
 static pid_t status_pid;static int status_flag=-1;
 if(status_pid!=getpid()||status_flag!=flag){
  FILE *status=fopen(GC_FAST_DIR "gc-control-status.csv",status_pid==getpid()?"a":"w");
  if(status){if(status_pid!=getpid())fputs("pid,mono_ms,requested_flag,latched_rollback,enabled_before,requested_enabled\n",status);fprintf(status,"%d,%llu,%d,%d,%d,%d\n",getpid(),(unsigned long long)dvm__now_ms(),flag,gc_fast_rollback_latched,gc_fast_enabled,enabled);fclose(status);}
  status_pid=getpid();status_flag=flag;
 }
 if(enabled!=gc_fast_enabled){
  fprintf(stderr,"GC_FAST_CONFIG pid=%d mono_ms=%llu collection_enabled=%d moving_enabled=0\n",getpid(),(unsigned long long)dvm__now_ms(),enabled);
  if(enabled){f=fopen(GC_FAST_DIR "gc-collection-dirty","w");if(!f)enabled=false;else{bool written=fprintf(f,"%d\n",getpid())>0;bool flushed=fflush(f)==0;bool closed=fclose(f)==0;enabled=written&&flushed&&closed;}}
  else unlink(GC_FAST_DIR "gc-collection-dirty");
  gc_fast_enabled=enabled;
 }
}
extern bool activity_runtime_gc_visit_owner(struct dvm *,dvm_ref,dvm_gc_visit_fn,void *) __attribute__((weak));
extern bool android_basics_gc_visit_owner(struct dvm *,dvm_ref,dvm_gc_visit_fn,void *) __attribute__((weak));
extern bool view_state_runtime_gc_visit_owner(struct dvm *,dvm_ref,dvm_gc_visit_fn,void *) __attribute__((weak));
extern bool surface_view_runtime_gc_visit_owner(struct dvm *,dvm_ref,dvm_gc_visit_fn,void *) __attribute__((weak));
extern bool window_attributes_gc_visit_owner(struct dvm *,dvm_ref,dvm_gc_visit_fn,void *) __attribute__((weak));
extern bool thread_extras_gc_visit_owner(struct dvm *,dvm_ref,dvm_gc_visit_fn,void *) __attribute__((weak));
extern bool thread_runtime_gc_visit_owner(struct dvm *,dvm_ref,dvm_gc_visit_fn,void *) __attribute__((weak));
extern bool reflection_runtime_gc_visit_owner(struct dvm *,dvm_ref,dvm_gc_visit_fn,void *) __attribute__((weak));
static const char *gc_fast_owner_edges(struct dvm *v,dvm_ref r,dvm_gc_visit_fn visit,void *user){
 if(activity_runtime_gc_visit_owner&&!activity_runtime_gc_visit_owner(v,r,visit,user))return "activity_runtime";
 if(android_basics_gc_visit_owner&&!android_basics_gc_visit_owner(v,r,visit,user))return "android_basics";
 if(view_state_runtime_gc_visit_owner&&!view_state_runtime_gc_visit_owner(v,r,visit,user))return "view_state_runtime";
 if(surface_view_runtime_gc_visit_owner&&!surface_view_runtime_gc_visit_owner(v,r,visit,user))return "surface_view_runtime";
 if(window_attributes_gc_visit_owner&&!window_attributes_gc_visit_owner(v,r,visit,user))return "window_attributes";
 if(thread_extras_gc_visit_owner&&!thread_extras_gc_visit_owner(v,r,visit,user))return "thread_extras";
 if(thread_runtime_gc_visit_owner&&!thread_runtime_gc_visit_owner(v,r,visit,user))return "thread_runtime";
 if(reflection_runtime_gc_visit_owner&&!reflection_runtime_gc_visit_owner(v,r,visit,user))return "reflection_runtime";
 return NULL;
}
/* Shared by the old-generation one-layer scan and the young transitive walk.
 * The callback queues young targets only; it never recursively enters old. */
static void gc_fast_trace_one(struct gc_fast_mark *s,dvm_ref r){
 struct dvm_object *o=gc_peek_object(s->vm,r);if(!o){s->failed=true;s->failure="invalid_trace_owner";return;}
 s->last_ref=r;s->last_class=o->cls?o->cls->name:"none";
 if(!dvm_gc_visit_declared_refs(s->vm,r,gc_fast_mark_slot,s)){s->retain_all=true;s->retain_reason="declared_reference_layout";}
 if(gc_fast_owner_edges(s->vm,r,gc_fast_mark_slot,s)){s->retain_all=true;s->retain_reason="owner_reference_layout";}
 if(dvm_gc_payload_visit(s->vm,o,gc_fast_mark_slot,s)<0){s->retain_all=true;if(!s->retain_reason)s->retain_reason="unknown_payload_transitive_extent";}
}
static bool gc_young_promote(struct dvm_object *o){
 if(!o->live||(o->gc_flags&DVM_GC_OLD)||!(o->gc_flags&DVM_GC_MARKED))return false;
 if(o->gc_age<UINT8_MAX)++o->gc_age;
 if(o->gc_age<GC_YOUNG_PROMOTION_AGE)return false;
 o->gc_flags|=DVM_GC_OLD;return true;
}
static bool gc_fast_sweep_eligible(const struct gc_fast_mark *s,const struct dvm_object *o){return o->live&&(!s->young_only||!(o->gc_flags&DVM_GC_OLD));}
/* Accounted object/header + declared storage only. Opaque native payload sizes
 * are unknown here; this is young_used, NEVER physical committed or RSS. */
static size_t gc_young_object_bytes(const struct dvm_object *o){
 size_t n=sizeof *o;
 if(o->slots&&o->cls&&o->cls->islots>0)n+=(size_t)o->cls->islots*sizeof(union dvm_value);
 if(o->utf8)n+=o->builder_capacity?o->builder_capacity:(size_t)o->utf8_len+1;
 if(o->string_units)n+=((size_t)o->string_length+1)*sizeof(uint16_t);
 if(o->kind==DVM_OBJ_ARRAY&&o->data)n+=(size_t)o->length*dvm__elem_width(o->elem_kind);
 return n;
}
static size_t gc_young_used(struct dvm *vm){size_t bytes=0;for(dvm_ref r=1;r<=vm->heap_size;r++){struct dvm_object *o=heap_slot(vm,r);if(o->live&&!(o->gc_flags&DVM_GC_OLD))bytes+=gc_young_object_bytes(o);}return bytes;}
#include "gc_young_selftest.inc"
extern bool sdk_input_gc_touch_snapshot(uint64_t *,uint64_t *) __attribute__((weak));
bool dvm_gc_fast_collect(struct dvm *vm,const char *reason){
 if(!vm||!dvm_gil_held(vm)||gc_fast_running||vm->gc_initializing||vm->gc_destroying)return false;
 gc_fast_configuration(vm);if(!gc_fast_enabled)return false;
 if(!perf_gc_allowed(reason&&!strcmp(reason,"manual_debug")))return false;
 struct gil_site_scope gs __attribute__((cleanup(gil_site_leave)))=gil_site_enter("GC");
 gc_fast_running=true;gc_fast_owner=pthread_self();gc_fast_handoffs=0;
 perf_play_gc(1);
 uint64_t begin=dvm__now_ms(),wall_begin=gc_fast_ns(),touch_queued_before=0,touch_dropped_before=0;
 atomic_store_explicit(&gc_destroy_cycle,begin,memory_order_release);
 bool touch_valid=sdk_input_gc_touch_snapshot&&sdk_input_gc_touch_snapshot(&touch_queued_before,&touch_dropped_before);
 static struct gc_stw_receipt receipt;
 bool complete=gc_stw_begin(vm,&receipt);uint64_t enter_end=gc_fast_ns();
 const char *abort_reason=complete?"none":(receipt.failure?receipt.failure:"safepoint_incomplete"),*abort_class="none";
 size_t before=vm_arena_used(),reclaimed=0,retained_weak_finalizer=0;
 struct vm_arena_region_snapshot rb,ra;vm_arena_region_snapshot(&rb);
 struct vm_external_snapshot eb,ea;vm_external_snapshot(&eb);
 struct gc_fast_mark s={.vm=vm,.capacity=vm->heap_size,.snapshot_identity=vm->next_object_identity,.slice_begin=enter_end,.slice_capacity=4096,.young_only=gc_young_only};
 size_t young_before=gc_young_used(vm),old_scanned=0,promoted=0,young_marked=0,young_reclaimed_bytes=0;
 s.class_capacity=1024; /* bounded inventory only; overflow is explicit, never a tracing exemption */
 uint64_t scratch_begin=gc_fast_ns();
 size_t old_pool=gc_fast_pool.work_n+gc_fast_pool.weak_n+gc_fast_pool.ranges_n+gc_fast_pool.classes_n+gc_fast_pool.slices_n;
 if(complete){
  complete=gc_fast_reserve(&gc_fast_pool.work,&gc_fast_pool.work_n,(s.capacity?s.capacity:1)*sizeof(dvm_ref))&&
    gc_fast_reserve(&gc_fast_pool.weak,&gc_fast_pool.weak_n,s.capacity+1)&&
    gc_fast_reserve(&gc_fast_pool.classes,&gc_fast_pool.classes_n,s.class_capacity*sizeof *s.classes)&&
    gc_fast_reserve(&gc_fast_pool.slices,&gc_fast_pool.slices_n,s.slice_capacity*sizeof *s.slices);
  if(!complete)abort_reason="root_workspace_unavailable";
 }
 s.slices=gc_fast_pool.slices;
 s.work=gc_fast_pool.work;s.weak_targets=gc_fast_pool.weak;s.classes=gc_fast_pool.classes;
 if(complete){memset(s.weak_targets,0,s.capacity+1);memset(s.classes,0,s.class_capacity*sizeof *s.classes);}
 uint64_t scratch_end=gc_fast_ns();
 if(s.slices){s.slices[s.nslices++]=(struct gc_fast_slice){.phase="pre_mark_accounting",.ns=scratch_begin-enter_end};s.slice_begin=scratch_begin;}
 gc_fast_slice_end(&s,"scratch",true);
 if(complete)gc_layout_stw_epoch=++gc_layout_next_stw_epoch;
 for(dvm_ref r=1;complete&&r<=s.capacity;r++){
  struct dvm_object *o=heap_slot(vm,r);s.last_ref=r;s.last_class=o->live&&o->cls?o->cls->name:"none";if(o->live){o->gc_flags&=~DVM_GC_MARKED;gc_fast_classify(&s,o);}
  gc_fast_slice_end(&s,"classify",false);
 }
 gc_fast_slice_end(&s,"classify",true);
 if(complete&&(!dvm_gc_visit_roots(vm,DVM_GC_ROOT_STRONG,gc_fast_mark_slot,&s)||s.failed)){complete=false;abort_reason="root_enumeration_incomplete";}
 gc_fast_slice_end(&s,"strong_roots",true);
 if(complete&&(!dvm_gc_visit_roots(vm,DVM_GC_ROOT_WEAK,gc_fast_keep_weak,&s)||s.failed)){complete=false;abort_reason="weak_root_enumeration_incomplete";}
 gc_fast_slice_end(&s,"weak_roots",true);
 if(complete){
  if(!gc_fast_native_stack(&s)){complete=false;abort_reason="native_stack_unavailable";}
 }
 gc_fast_slice_end(&s,"native_stack",true);
 /* Collector inspection must not create thousands of artificial raw borrows
  * in its enclosing JNI scope. Root enumeration manages this flag itself. */
 bool visiting=vm->gc?vm->gc->visiting:false;if(vm->gc)vm->gc->visiting=true;
 for(dvm_ref r=1;complete&&r<=s.capacity;r++){
  struct dvm_object *o=heap_slot(vm,r);s.last_ref=r;s.last_class=o->live&&o->cls?o->cls->name:"none";if(!o->live)continue;
  bool weak_finalizer=o->gc_weak_head!=0||s.weak_targets[r];
  bool retain=o->pins||o->gc_strong_handles||o->gc_body_borrows||o->gc_payload_borrows||o->host_handle||o->monitor_depth;
  weak_finalizer=weak_finalizer||gc_fast_class_retention(&s,o->cls);
  if(weak_finalizer){retained_weak_finalizer++;retain=true;}
  if(retain)gc_fast_mark_slot(&r,&s);
  gc_fast_slice_end(&s,"retention_roots",false);
 }
 gc_fast_slice_end(&s,"retention_roots",true);
 uint64_t old_begin=gc_fast_ns();
 for(dvm_ref r=1;complete&&s.young_only&&r<=s.capacity;r++){
  struct dvm_object *o=heap_slot(vm,r);if(!o->live||!(o->gc_flags&DVM_GC_OLD))continue;
  gc_fast_trace_one(&s,r);old_scanned++;if(s.failed){complete=false;abort_reason=s.failure?s.failure:"invalid_old_edge";}
 }
 uint64_t old_end=gc_fast_ns();gc_fast_slice_end(&s,"old_roots_one_layer",true);
 for(size_t i=0;complete&&i<s.count;i++){
  gc_fast_trace_one(&s,s.work[i]);
  if(s.failed){complete=false;abort_reason=s.failure?s.failure:"invalid_edge";}
  gc_fast_slice_end(&s,"mark_traversal",false);
 }
 gc_layout_stw_epoch=0;
 if(vm->gc)vm->gc->visiting=visiting;
 gc_fast_slice_end(&s,"mark_traversal",true);uint64_t mark_end=gc_fast_ns();
 /* True full STW: no GIL handoff anywhere before this sweep completes. C=0
  * permits a precise sweep; unresolved C still needs transitive quarantine.
  * A stopped world alone does not reveal edges inside an unknown payload. */
 size_t marked=0,bad_self=0,objects=0,sweep_batch=0;
 for(dvm_ref r=1;complete&&r<=s.capacity;r++){
  struct dvm_object *o=heap_slot(vm,r);s.last_ref=r;s.last_class=o->live&&o->cls?o->cls->name:"none";if(!o->live)continue;
  if(!gc_fast_sweep_eligible(&s,o)){objects++;bad_self+=atomic_load_explicit(&o->gc_self,memory_order_acquire)!=o;continue;}
  if(s.retain_all){o->gc_flags|=DVM_GC_MARKED;s.quarantine++;}
  if(o->gc_flags&DVM_GC_MARKED){marked++;if(!(o->gc_flags&DVM_GC_OLD))young_marked++;}
  else {
   const char *cls=o->cls?o->cls->name:"none";bool cursor=cls&&!strcmp(cls,"android/database/Cursor");
   size_t young_bytes=(o->gc_flags&DVM_GC_OLD)?0:gc_young_object_bytes(o);
   uint64_t destroy_begin=gc_fast_ns();heap_free(vm,r);uint64_t destroy_end=gc_fast_ns();
   if(cursor||destroy_end-destroy_begin>=1000000u)gc_destroy_record((struct gc_destroy_sample){.start=destroy_begin,.end=destroy_end,.kind="object_free",.cls=cls,.ref=r});
   if(!o->live){reclaimed++;young_reclaimed_bytes+=young_bytes;}
  }
  if(o->live){objects++;bad_self+=atomic_load_explicit(&o->gc_self,memory_order_acquire)!=o;}
  gc_fast_slice_end(&s,"sweep",false);
  if(++sweep_batch==256){gc_fast_slice_end(&s,"sweep_256_objects",true);sweep_batch=0;}
 }
 uint64_t sweep_end=gc_fast_ns();gc_fast_slice_end(&s,"sweep",true);
 if(complete&&s.young_only&&!s.retain_all)for(dvm_ref r=1;r<=s.capacity;r++)if(gc_young_promote(heap_slot(vm,r)))promoted++;
 uint64_t promotion_end=gc_fast_ns();gc_fast_slice_end(&s,"promotion",true);
 size_t young_after=gc_young_used(vm);
 size_t after=vm_arena_used();vm_external_snapshot(&ea);vm_arena_region_snapshot(&ra);
 uint64_t touch_queued_after=0,touch_dropped_after=0;touch_valid=touch_valid&&sdk_input_gc_touch_snapshot(&touch_queued_after,&touch_dropped_after);
 gc_fast_last_unknown=s.unknown;gc_fast_last_quarantine=s.quarantine;gc_fast_last_handoffs=gc_fast_handoffs;
 size_t scratch_bytes=gc_fast_pool.work_n+gc_fast_pool.weak_n+gc_fast_pool.ranges_n+gc_fast_pool.classes_n+gc_fast_pool.slices_n;
 size_t ordinary_physical=before>after?before-after:0;
 size_t external_physical=ea.physical_reclaimed_bytes>=eb.physical_reclaimed_bytes?ea.physical_reclaimed_bytes-eb.physical_reclaimed_bytes:0;
 size_t live_reclaimed=rb.live_bytes>ra.live_bytes?rb.live_bytes-ra.live_bytes:0;
 perf_gc_executed(complete);
 gc_stw_end();gc_fast_slice_end(&s,"exit_accounting",true);
 atomic_store_explicit(&gc_destroy_cycle,0,memory_order_release);
 unsigned depth=dvm_gil_unlock_all(vm);uint64_t world_end=gc_fast_ns();
 double hold_ms=(double)(world_end-wall_begin)/1e6;
 double enter_ms=(double)(enter_end-wall_begin)/1e6,scratch_ms=(double)(scratch_end-scratch_begin)/1e6;
 double mark_ms=(double)(mark_end-scratch_end+scratch_begin-enter_end)/1e6,sweep_ms=(double)(sweep_end-mark_end)/1e6,exit_ms=(double)(world_end-sweep_end)/1e6;
 /* All CSV/inventory IO is outside the measured STW, not a hidden debug scan. */
 static pid_t young_pid;bool young_first=young_pid!=getpid();young_pid=getpid();
 FILE *yf=fopen(GC_FAST_DIR "gc-young.csv",young_first?"w":"a");if(yf){
  if(young_first)fputs("pid,mono_ms,young_only,completed,marked,reclaimed,promoted,old_scanned,young_used_before,young_used_after,young_reclaimed_accounted_bytes,pause_ms,prepare_ms,roots_ms,old_scan_ms,young_mark_ms,sweep_ms,promotion_ms,exit_ms,bad_self,dropped_before,dropped_after,moving_enabled\n",yf);
  fprintf(yf,"%d,%llu,%d,%d,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%zu,%llu,%llu,0\n",getpid(),(unsigned long long)begin,s.young_only,complete,young_marked,reclaimed,promoted,old_scanned,young_before,young_after,young_reclaimed_bytes,hold_ms,(double)(scratch_end-wall_begin)/1e6,(double)(old_begin-scratch_end)/1e6,(double)(old_end-old_begin)/1e6,(double)(mark_end-old_end)/1e6,(double)(sweep_end-mark_end)/1e6,(double)(promotion_end-sweep_end)/1e6,(double)(world_end-promotion_end)/1e6,bad_self,(unsigned long long)touch_dropped_before,(unsigned long long)touch_dropped_after);fclose(yf);
 }
 gc_destroy_flush();gc_stw_write(&receipt,begin);if(s.classes&&complete)gc_fast_inventory_write(&s,begin);
 static pid_t csv_pid;bool first=csv_pid!=getpid();csv_pid=getpid();
 FILE *f=fopen(GC_FAST_DIR "gc-collect-slices.csv",first?"w":"a");if(f){
  if(first)fputs("pid,mono_ms,phase,work_ms,independent_pause,last_ref,last_class\n",f);
  for(size_t i=0;i<s.nslices;i++)fprintf(f,"%d,%llu,%s,%.6f,0,%u,%s\n",getpid(),(unsigned long long)begin,s.slices[i].phase,(double)s.slices[i].ns/1e6,s.slices[i].last_ref,s.slices[i].last_class?s.slices[i].last_class:"none");fclose(f);
 }
 f=fopen(GC_FAST_DIR "gc-collect-stw.csv",first?"w":"a");if(f){
  if(first)fputs("pid,mono_ms,marked_objects,pause_ms,STW_enter_ms,scratch_ms,mark_ms,sweep_ms,STW_exit_ms,total_threads,arrived_threads,touch_valid,queued_before,queued_after,dropped_before,dropped_after,scratch_reserved_bytes,scratch_grew,bad_self,denied_before,denied_after\n",f);
  fprintf(f,"%d,%llu,%zu,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%u,%u,%d,%llu,%llu,%llu,%llu,%zu,%d,%zu,%zu,%zu\n",getpid(),(unsigned long long)begin,marked,hold_ms,enter_ms,scratch_ms,mark_ms,sweep_ms,exit_ms,receipt.total,receipt.arrived,touch_valid,(unsigned long long)touch_queued_before,(unsigned long long)touch_queued_after,(unsigned long long)touch_dropped_before,(unsigned long long)touch_dropped_after,scratch_bytes,scratch_bytes!=old_pool,bad_self,rb.denied,ra.denied);fclose(f);
 }
 f=fopen(GC_FAST_DIR "gc-collect.csv",first?"w":"a");if(f){
  if(first)fputs("pid,mono_ms,reason,collection_enabled,moving_enabled,completed,marked_objects,reclaimed_objects,reclaimed_live_bytes,physical_reclaimed_bytes,committed_before,committed_after,live_before,live_after,external_before,external_after,retained_weak_finalizer,abort_reason,abort_class,pause_ms,scratch_bytes,retained_unknown_payloads,quarantine_objects,retain_reason,mutator_handoffs,traced_objects,objects_after,bad_self,inventory_overflow\n",f);
  fprintf(f,"%d,%llu,%s,1,0,%d,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%s,%s,%.6f,%zu,%zu,%zu,%s,%llu,%zu,%zu,%zu,%zu\n",getpid(),(unsigned long long)begin,reason,complete,marked,reclaimed,live_reclaimed,ordinary_physical+external_physical,before,after,rb.live_bytes,ra.live_bytes,eb.bytes,ea.bytes,retained_weak_finalizer,abort_reason,abort_class,hold_ms,scratch_bytes,s.unknown,s.quarantine,s.retain_reason?s.retain_reason:"none",(unsigned long long)gc_fast_last_handoffs,s.count,objects,bad_self,s.inventory_overflow);fclose(f);
 }
 if(!complete)fprintf(stderr,"GC_FAST_ABORT reason=%s class=%s marked=%zu reclaimed=%zu moving_enabled=0\n",abort_reason,abort_class,marked,reclaimed);
 dvm_gil_relock(vm,depth);gc_fast_last=dvm__now_ms();gc_fast_running=false;perf_play_gc(0);return complete;
}
extern int sqlite_gc_deferred_step(uint64_t,uint64_t *) __attribute__((weak));
static void gc_fast_deferred_poll(struct dvm *vm){
 if(!perf_gc_allowed(0))return; /* native cursor GC cleanup is also kept out of frames */
 uint64_t m[7];if(!sqlite_gc_deferred_step||!sqlite_gc_deferred_step(500000u,m))return;
 /* Cleanup itself held the GIL. Report the full cleanup-step body as well
  * as batches. GIL release/reacquire overhead is not in cleanup_ms; diagnostic
  * IO runs after release. This is not a whole end-to-end pause metric. */
 unsigned depth=dvm_gil_unlock_all(vm);
 static pid_t pid;bool first=pid!=getpid();pid=getpid();FILE *f=fopen(GC_FAST_DIR "gc-cursor-deferred.csv",first?"w":"a");
 if(f){if(first)fputs("pid,mono_ms,processed_cells,finished_cursors,pending_cursors,pending_cells,cleanup_ms,max_batch_ms,budget_ms\n",f);
 fprintf(f,"%d,%llu,%llu,%llu,%llu,%llu,%.6f,%.6f,%.6f\n",getpid(),(unsigned long long)dvm__now_ms(),(unsigned long long)m[0],(unsigned long long)m[1],(unsigned long long)m[2],(unsigned long long)m[3],(double)m[4]/1e6,(double)m[5]/1e6,(double)m[6]/1e6);fclose(f);}
 dvm_gil_relock(vm,depth);
}
static uint64_t gc_fast_debug_last;
static bool gc_frame_retry;
static void gc_fast_debug_poll(struct dvm *vm){
 if(!vm||gc_fast_running||vm->gc_initializing||vm->gc_destroying||!dvm_gil_held(vm))return;
 /* NativeRender and the enclosing doFrame can call back into Java. With the
  * opt-in policy, keep control reads/selftests off these active windows.
  * Owner/safepoint polling outside frames still handles pending requests. */
 if(perf_frame_controls_deferred()&&perf_frame_in_progress())return;
 uint64_t now=dvm__now_ms();if(now-gc_fast_debug_last<100)return;gc_fast_debug_last=now;
 gil_observe_poll(vm);
 perf_config_poll();
 static bool perf_tested;if(!perf_tested){perf_tested=true;if(!perf_guard_selftest()){gc_fast_rollback_latched=true;gc_fast_enabled=false;FILE *off=fopen(GC_FAST_DIR "gc-collection-enabled","w");if(off){fputs("0\n",off);fclose(off);}return;}}
 gc_fast_deferred_poll(vm);
 gc_fast_configuration(vm);
 if(!gc_young_selftested){gc_young_selftested=true;if(!gc_young_selftest(vm)){
  gc_fast_rollback_latched=true;gc_fast_enabled=false;FILE *off=fopen(GC_FAST_DIR "gc-collection-enabled","w");if(off){fputs("0\n",off);fclose(off);}return;
 }}
 if(!gc_fast_enabled)return;
 FILE *manual=fopen(GC_FAST_DIR "gc-collection-now","r");int requested=0;
 if(manual){(void)fscanf(manual,"%d",&requested);fclose(manual);}
 if(requested==1){
  size_t max_scope=0;if(vm->gc)for(struct dvm_gc_native_scope *q=vm->gc->scopes;q;q=q->next_vm)if(q->count>max_scope)max_scope=q->count;
  if(max_scope>4096&&max_scope>vm->heap_size/2)return; /* retry at next ordinary safepoint */
  if(!perf_gc_allowed(1)){gc_frame_retry=true;return;} /* preserve pending manual request until a frame boundary */
  (void)dvm_gc_fast_collect(vm,"manual_debug");
  manual=fopen(GC_FAST_DIR "gc-collection-now","w");if(manual){fputs("0\n",manual);fclose(manual);}return;
 }
}
void dvm_gc_frame_boundary(struct dvm *vm){
 if(!vm||!dvm_gil_held(vm))return;
 /* A return boundary is still on the render/Looper critical path. Never
  * turn a deferred manual request into a synchronous frame-tail collection. */
 if(perf_frame_controls_deferred())return;
 perf_config_poll();
 if(gc_frame_retry){gc_frame_retry=false;gc_fast_debug_last=0;gc_fast_debug_poll(vm);}
}
static void gc_fast_allocation_poll(struct dvm *vm,bool large){
 if(!vm||gc_fast_running||vm->gc_initializing||vm->gc_destroying||!dvm_gil_held(vm))return;
 if(!large&&(++gc_fast_poll%1024))return;
 if(!perf_gc_allowed(0))return; /* before configuration IO or full heap threshold scan */
 gc_fast_configuration(vm);if(!gc_fast_enabled)return;
 struct vm_arena_diagnostic s;vm_arena_diagnostic_snapshot(&s);
 bool young_pressure=gc_young_only&&gc_young_used(vm)>GC_YOUNG_TRIGGER_BYTES;
 if((large||young_pressure||s.used>s.ordinary_limit*7u/10u)&&dvm__now_ms()-gc_fast_last>=1000)
  (void)dvm_gc_fast_collect(vm,large?"before_large_array":young_pressure?"young_24MiB":"ordinary_70pct");
}
void dvm_gc_fast_shutdown(struct dvm *vm){
 /* VM teardown is quiescent, before arena destruction: queued native nodes
  * must not survive backing release or the next VM could see dangling nodes. */
 atomic_store_explicit(&gc_destroy_cycle,0,memory_order_release);
 if(sqlite_gc_deferred_step){uint64_t done[7];while(sqlite_gc_deferred_step(UINT64_MAX,done)){} }
 if(gc_fast_vm==vm){unlink(GC_FAST_DIR "gc-collection-dirty");gc_fast_enabled=false;gc_fast_initialized=false;gc_fast_vm=NULL;gc_fast_pool_destroy();}}
#ifdef GC_FAST_RESTORE_FPRINTF
#pragma pop_macro("fprintf")
#undef GC_FAST_RESTORE_FPRINTF
#endif
