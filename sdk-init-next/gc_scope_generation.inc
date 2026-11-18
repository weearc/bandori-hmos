/* Only scope entries and handoff temporaries carry gen_ref this round.
 * GIL/exclusive owner required. No allocation, IO or locks in fault capture. */
enum {GC_SCOPE_CAPTURE=1,GC_SCOPE_TOUCH,GC_SCOPE_ROOT,GC_SCOPE_BORROW,
 GC_SCOPE_LEAVE,GC_SCOPE_HANDOFF,GC_SCOPE_VISITOR};
struct gc_scope_fault {
 _Atomic unsigned ready,phase;_Atomic uint64_t vm,tid,ref,identity,current_ticket,mono_ns;
};
#define GC_SCOPE_FAULT_CAPACITY 32u
static struct gc_scope_fault gc_scope_faults[GC_SCOPE_FAULT_CAPACITY];
static _Atomic uint64_t gc_scope_fault_count,gc_scope_fault_lost;
static _Thread_local enum gen_result gc_scope_last_error;
enum gen_result dvm_gc_scope_last_error(void){return gc_scope_last_error;}
static struct gen_shadow *gc_scope_shadow(struct dvm *v){
 return v&&v->heap_blocks?((struct gc_id_directory*)v->heap_blocks)->generation:NULL;
}
static bool gc_scope_reject(struct dvm *v,struct dvm_gc_native_scope *q,const struct dvm_gc_scope_ref *r,unsigned phase){
 gc_scope_last_error=GEN_STALE_REFERENCE;if(q)q->failed=true;
 uint64_t i=atomic_fetch_add_explicit(&gc_scope_fault_count,1,memory_order_relaxed);
 if(i<GC_SCOPE_FAULT_CAPACITY){
  struct gc_scope_fault *f=&gc_scope_faults[i];struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);
  atomic_store(&f->phase,phase);atomic_store(&f->vm,(uintptr_t)v);atomic_store(&f->tid,(uint64_t)syscall(SYS_gettid));
  atomic_store(&f->ref,r->ref);atomic_store(&f->identity,r->identity);
  atomic_store(&f->current_ticket,gen_ticket_of(gc_scope_shadow(v),gen_slot(r->ref)));
  atomic_store(&f->mono_ns,(uint64_t)t.tv_sec*1000000000ull+t.tv_nsec);
  atomic_store_explicit(&f->ready,1,memory_order_release);
 }else atomic_fetch_add_explicit(&gc_scope_fault_lost,1,memory_order_relaxed);
 return dvm_gc_contract_fail(v,GC_CONTRACT_IDENTITY);
}
static struct dvm_object *gc_scope_resolve(struct dvm *v,struct dvm_gc_native_scope *q,const struct dvm_gc_scope_ref *r,unsigned phase){
 /* Never mint a new current ticket here: the stored expected ticket wins. */
 if(!gen_check(gc_scope_shadow(v),gen_slot(r->ref),gen_ticket(r->ref))){gc_scope_reject(v,q,r,phase);return NULL;}
 struct dvm_object *o=gc_peek_object(v,gen_slot(r->ref));
 if(!o||o->identity_generation!=r->identity){gc_scope_reject(v,q,r,phase);return NULL;}
 return o;
}
static bool gc_scope_capture(struct dvm *v,struct dvm_gc_native_scope *q,uint32_t slot,struct dvm_object *o,struct dvm_gc_scope_ref *out){
 *out=(struct dvm_gc_scope_ref){gen_encode(slot,gen_ticket_of(gc_scope_shadow(v),slot)),o?o->identity_generation:0};
 return gc_scope_resolve(v,q,out,GC_SCOPE_CAPTURE)!=NULL;
}
/* Worker-only export, called only if a fault exists. */
static void gc_scope_fault_export(int fd){
 uint64_t n=atomic_load(&gc_scope_fault_count);if(n>GC_SCOPE_FAULT_CAPACITY)n=GC_SCOPE_FAULT_CAPACITY;
 for(uint64_t i=0;i<n;i++){
  struct gc_scope_fault *f=&gc_scope_faults[i];if(!atomic_load_explicit(&f->ready,memory_order_acquire))continue;
  dprintf(fd,"scope_fault=%llu error=%u phase=%u vm=%llu tid=%llu ref=%llu identity=%llu current_ticket=%llu mono_ns=%llu\n",(unsigned long long)i,GEN_STALE_REFERENCE,atomic_load(&f->phase),(unsigned long long)atomic_load(&f->vm),(unsigned long long)atomic_load(&f->tid),(unsigned long long)atomic_load(&f->ref),(unsigned long long)atomic_load(&f->identity),(unsigned long long)atomic_load(&f->current_ticket),(unsigned long long)atomic_load(&f->mono_ns));
 }
}
