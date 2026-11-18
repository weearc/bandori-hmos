/* Included after gc_directory.inc. GIL/exclusive boot owns all writes.
 * No query/check/ABI change: issue immediately before live publication.
 * Failure leaves the header unpublished and returns the existing alloc=0.
 * Counters are process-wide, read-only from the independent export worker. */
static _Atomic uint64_t gen_heap_attempts,gen_heap_published,gen_heap_reused;
static _Atomic uint64_t gen_heap_freed,gen_heap_failures;
static _Atomic uint64_t gen_heap_failure_reason[GEN_ALLOCATION_FAILED+1];
static _Atomic unsigned gen_heap_first_error;
static bool gen_heap_publish(struct dvm *vm,dvm_ref slot,bool reused){
 struct gc_id_directory *dir=(void*)vm->heap_blocks;
 enum gen_result result=GEN_INVALID_ARGUMENT;
 if(dir){
  if(!dir->generation)result=gen_shadow_create(DVM_GEN_SLOT_CAPACITY,&dir->generation);
  else result=GEN_SUCCESS;
  if(result==GEN_SUCCESS){uint32_t ignored_ticket;result=gen_shadow_publish(dir->generation,slot,&ignored_ticket);}
 }
 if(result!=GEN_SUCCESS){
  atomic_fetch_add_explicit(&gen_heap_failures,1,memory_order_relaxed);
  atomic_fetch_add_explicit(&gen_heap_failure_reason[result],1,memory_order_relaxed);
  unsigned empty=GEN_SUCCESS;
  (void)atomic_compare_exchange_strong_explicit(&gen_heap_first_error,&empty,result,memory_order_relaxed,memory_order_relaxed);
  return false;
 }
 atomic_fetch_add_explicit(&gen_heap_published,1,memory_order_relaxed);
 if(reused)atomic_fetch_add_explicit(&gen_heap_reused,1,memory_order_relaxed);
 return true;
}
