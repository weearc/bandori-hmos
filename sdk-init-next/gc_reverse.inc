/* Exact-key, bounded chained hash. Nodes are keyed by full body address;
 * IDs link nodes, not body-address arithmetic. Freed physical headers stay
 * registered until VM teardown; live/publication checks preserve legacy.
 * No new identity generation is introduced: ABA remains deliberately unsafe. */
static uint32_t gc_rev_legacy_ref_of(struct dvm *v,struct dvm_object *o){
 if(!v||!o)return 0;uintptr_t p=(uintptr_t)o;
 for(uint32_t b=0;b<(v->heap_size+DVM_HEAP_BLOCK-1)/DVM_HEAP_BLOCK;b++){
  uintptr_t base=(uintptr_t)v->heap_blocks[b],span=DVM_HEAP_BLOCK*sizeof *o;
  if(p>=base&&p-base<span&&(p-base)%sizeof *o==0){uint32_t r=b*DVM_HEAP_BLOCK+(p-base)/sizeof *o+1;return r<=v->heap_size&&o->live?r:0;}
 }return 0;
}
static void gc_rev_fault(uintptr_t address,uint32_t old,uint32_t found,unsigned reason){
 atomic_store_explicit(&gc_rev_bad_address,address,memory_order_relaxed);
 atomic_store_explicit(&gc_rev_bad_old,old,memory_order_relaxed);
 atomic_store_explicit(&gc_rev_bad_new,found,memory_order_relaxed);
 atomic_store_explicit(&gc_rev_bad_reason,reason,memory_order_relaxed);
 atomic_fetch_add_explicit(&gc_rev_misses,1,memory_order_relaxed);
 atomic_fetch_add_explicit(&gc_rev_mismatches,1,memory_order_release);
 __builtin_trap();
}
static uint32_t gc_rev_ref_of(struct dvm *v,struct dvm_object *o){
 uint32_t result=0,old=0;
#if DVM_ID_DIRECTORY_DUAL
 old=gc_rev_legacy_ref_of(v,o);
#endif
 if(v&&o&&v->heap_blocks){
  struct gc_id_directory *dir=(void*)v->heap_blocks;
  uint32_t id=dir->rev_buckets[gc_rev_bucket((uintptr_t)o)],steps=0;
  while(id){
   uint32_t index=id-1u,b=index/DVM_HEAP_BLOCK;
   if(id>v->heap_cap||b>=DVM_REV_DIRECTORY_BLOCKS||!dir->rev_pages[b]||++steps>v->heap_cap)
    gc_rev_fault((uintptr_t)o,old,0,1); /* malformed ID/cycle */
   struct gc_rev_node *node=&dir->rev_pages[b][index%DVM_HEAP_BLOCK];
   if(node->body==(uintptr_t)o){
    /* Validate the registered exact key through the forward directory;
     * never dereference the caller's arbitrary/interior pointer directly. */
    if(id<=v->heap_size&&gc_dir_lookup(v,id,true)==o)result=id;
    break;
   }
   id=node->next;
  }
 }
#if DVM_ID_DIRECTORY_DUAL
 if(result!=old)gc_rev_fault((uintptr_t)o,old,result,0);
#endif
 gc_dir_error=result?GC_DIR_OK:GC_DIR_INVALID_REF;
 atomic_fetch_add_explicit(result?&gc_rev_hits:&gc_rev_misses,1,memory_order_relaxed);
 return result;
}
