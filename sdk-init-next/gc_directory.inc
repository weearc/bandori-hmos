/* Included by dvm.c. Heap/GIL ownership is unchanged, including exclusive
 * VM boot/destruction. The worker reads counters only, never the directory.
 * Keep blocks first: the existing VM field and reverse lookup retain their
 * ABI/offsets. No struct dvm size or layout changes, including other TUs. */
#include "gc_directory.h"
#include "gc_generation.h"
struct gc_rev_node {uintptr_t body;uint32_t next;};
struct gc_id_directory {
 struct dvm_object *blocks[DVM_ID_DIRECTORY_BLOCKS];
 struct dvm_object **pages[DVM_ID_DIRECTORY_BLOCKS];
 struct gc_rev_node *rev_pages[DVM_REV_DIRECTORY_BLOCKS];
 uint32_t rev_buckets[DVM_REV_DIRECTORY_BUCKETS];
 /* Publication-only shadow; not consulted by either lookup direction. */
 struct gen_shadow *generation;
};
_Static_assert(offsetof(struct gc_id_directory,blocks)==0,"heap table ABI");
_Static_assert((uint64_t)DVM_ID_DIRECTORY_BLOCKS*DVM_HEAP_BLOCK<=UINT32_MAX,
 "directory IDs must fit dvm_ref");
static _Atomic uint64_t gc_dir_hits,gc_dir_misses,gc_dir_mismatches,gc_dir_overflows;
static _Atomic uint64_t gc_dir_metadata,gc_dir_bad_ref,gc_dir_bad_old,gc_dir_bad_new;
static _Atomic uint64_t gc_rev_hits,gc_rev_misses,gc_rev_mismatches,gc_rev_overflows;
static _Atomic uint64_t gc_rev_bad_address,gc_rev_bad_old,gc_rev_bad_new,gc_rev_bad_reason;
static _Thread_local unsigned gc_dir_error;
unsigned dvm_gc_directory_last_error(void){return gc_dir_error;}
void dvm_gc_directory_snapshot(struct dvm_gc_directory_stats *s){
 if(!s)return;
 s->hits=atomic_load_explicit(&gc_dir_hits,memory_order_relaxed);
 s->misses=atomic_load_explicit(&gc_dir_misses,memory_order_relaxed);
 s->queries=s->hits+s->misses;
 s->mismatches=atomic_load_explicit(&gc_dir_mismatches,memory_order_relaxed);
 s->overflows=atomic_load_explicit(&gc_dir_overflows,memory_order_relaxed);
 s->metadata_bytes=atomic_load_explicit(&gc_dir_metadata,memory_order_relaxed);
 s->mismatch_ref=atomic_load_explicit(&gc_dir_bad_ref,memory_order_relaxed);
 s->mismatch_legacy=atomic_load_explicit(&gc_dir_bad_old,memory_order_relaxed);
 s->mismatch_directory=atomic_load_explicit(&gc_dir_bad_new,memory_order_relaxed);
 s->rev_hits=atomic_load_explicit(&gc_rev_hits,memory_order_relaxed);
 s->rev_misses=atomic_load_explicit(&gc_rev_misses,memory_order_relaxed);
 s->rev_queries=s->rev_hits+s->rev_misses;
 s->rev_mismatches=atomic_load_explicit(&gc_rev_mismatches,memory_order_acquire);
 s->rev_overflows=atomic_load_explicit(&gc_rev_overflows,memory_order_relaxed);
 s->rev_bad_address=atomic_load_explicit(&gc_rev_bad_address,memory_order_relaxed);
 s->rev_bad_legacy=atomic_load_explicit(&gc_rev_bad_old,memory_order_relaxed);
 s->rev_bad_directory=atomic_load_explicit(&gc_rev_bad_new,memory_order_relaxed);
 s->rev_bad_reason=atomic_load_explicit(&gc_rev_bad_reason,memory_order_relaxed);
}
/* The ONLY retained fixed-address query. Enabled as an oracle in dual mode.
 * The bounds/live predicate below is shared so null/dead IDs preserve NULL. */
static struct dvm_object *gc_dir_legacy_slot(struct dvm *vm,dvm_ref ref){
 if(!vm||!ref||ref>vm->heap_cap||!vm->heap_blocks)return NULL;
 uint32_t i=ref-1u;
 if(i/DVM_HEAP_BLOCK>=vm->heap_nblocks||!vm->heap_blocks[i/DVM_HEAP_BLOCK])return NULL;
 return &vm->heap_blocks[i/DVM_HEAP_BLOCK][i%DVM_HEAP_BLOCK];
}
static struct dvm_object *gc_dir_lookup(struct dvm *vm,dvm_ref ref,bool live){
 gc_dir_error=GC_DIR_OK;
 struct dvm_object *o=NULL;
 if(vm&&ref&&ref<=vm->heap_cap&&(!live||ref<=vm->heap_size)&&vm->heap_blocks){
  uint32_t i=ref-1u,b=i/DVM_HEAP_BLOCK;
  if(b<DVM_ID_DIRECTORY_BLOCKS&&b<vm->heap_nblocks){
   struct gc_id_directory *dir=(void*)vm->heap_blocks;
   o=dir->pages[b]?dir->pages[b][i%DVM_HEAP_BLOCK]:NULL;
  }
 }
#if DVM_ID_DIRECTORY_DUAL
 /* Calculate the oracle independently even for rejected IDs. The old
  * live-query bounds returned NULL before performing address arithmetic. */
 struct dvm_object *old=vm&&(!live||ref<=vm->heap_size)?gc_dir_legacy_slot(vm,ref):NULL;
 if(o!=old){
  /* Never dereference a corrupt directory body, never fall back silently.
   * Bounded lock-free fault record precedes the intentional fatal trap. */
  atomic_store_explicit(&gc_dir_bad_ref,ref,memory_order_relaxed);
  atomic_store_explicit(&gc_dir_bad_old,(uintptr_t)old,memory_order_relaxed);
  atomic_store_explicit(&gc_dir_bad_new,(uintptr_t)o,memory_order_relaxed);
  atomic_fetch_add_explicit(&gc_dir_misses,1,memory_order_relaxed);
  atomic_fetch_add_explicit(&gc_dir_mismatches,1,memory_order_relaxed);
  __builtin_trap();
 }
#endif
 if(!o||(live&&!o->live))goto missing;
 atomic_fetch_add_explicit(&gc_dir_hits,1,memory_order_relaxed);
 return o;
missing:
 gc_dir_error=GC_DIR_INVALID_REF;
 atomic_fetch_add_explicit(&gc_dir_misses,1,memory_order_relaxed);
 return NULL;
}
static struct dvm_object *heap_slot(struct dvm *vm,dvm_ref ref){
 /* Internal initialization/free-list/scans need dead physical headers.
  * This is distinct from the live-object lookup used by dvm__obj/peek. */
 return gc_dir_lookup(vm,ref,false);
}
static uint32_t gc_rev_bucket(uintptr_t p){
 uint64_t x=p;x^=x>>33;x*=UINT64_C(0xff51afd7ed558ccd);x^=x>>33;
 return (uint32_t)x&(DVM_REV_DIRECTORY_BUCKETS-1u);
}
static bool gc_dir_extend(struct dvm *vm){
 if(vm->heap_nblocks>=DVM_ID_DIRECTORY_BLOCKS||vm->heap_nblocks>=DVM_REV_DIRECTORY_BLOCKS){
  gc_dir_error=GC_DIR_CAPACITY;
  if(vm->heap_nblocks>=DVM_ID_DIRECTORY_BLOCKS)atomic_fetch_add_explicit(&gc_dir_overflows,1,memory_order_relaxed);
  if(vm->heap_nblocks>=DVM_REV_DIRECTORY_BLOCKS)atomic_fetch_add_explicit(&gc_rev_overflows,1,memory_order_relaxed);
  return false;
 }
 bool fresh=vm->heap_blocks==NULL;
 struct gc_id_directory *dir=fresh?calloc(1,sizeof *dir):(void*)vm->heap_blocks;
 if(!dir){gc_dir_error=GC_DIR_ALLOCATION;return false;}
 struct dvm_object **page=calloc(DVM_HEAP_BLOCK,sizeof *page);
 struct gc_rev_node *reverse=page?calloc(DVM_HEAP_BLOCK,sizeof *reverse):NULL;
 struct dvm_object *block=reverse?calloc(DVM_HEAP_BLOCK,sizeof *block):NULL;
 if(!block){free(reverse);free(page);if(fresh)free(dir);gc_dir_error=GC_DIR_ALLOCATION;return false;}
 uint32_t b=vm->heap_nblocks;
 /* All fallible allocation is complete. GIL/exclusive boot prevents a
  * reader during bucket linking; no callbacks, allocation or yield here. */
 for(uint32_t i=0;i<DVM_HEAP_BLOCK;i++){
  page[i]=&block[i];reverse[i].body=(uintptr_t)&block[i];
  uint32_t hash=gc_rev_bucket(reverse[i].body);
  reverse[i].next=dir->rev_buckets[hash];
  dir->rev_buckets[hash]=b*DVM_HEAP_BLOCK+i+1u;
 }
 dir->rev_pages[b]=reverse;dir->pages[b]=page;dir->blocks[b]=block;
 /* Publish only the complete pair. No allocation/yield after this point. */
 vm->heap_blocks=dir->blocks;
 vm->heap_nblocks++;vm->heap_cap+=DVM_HEAP_BLOCK;
 atomic_fetch_add_explicit(&gc_dir_metadata,(fresh?sizeof *dir:0)+DVM_HEAP_BLOCK*(sizeof *page+sizeof *reverse),memory_order_relaxed);
 gc_dir_error=GC_DIR_OK;return true;
}
static void gc_dir_destroy(struct dvm *vm){
 if(!vm->heap_blocks)return;
 struct gc_id_directory *dir=(void*)vm->heap_blocks;
 for(uint32_t b=0;b<vm->heap_nblocks;b++){free(dir->rev_pages[b]);free(dir->pages[b]);}
 gen_shadow_destroy(dir->generation);
 atomic_fetch_sub_explicit(&gc_dir_metadata,sizeof *dir+(uint64_t)vm->heap_nblocks*DVM_HEAP_BLOCK*(sizeof(void*)+sizeof(struct gc_rev_node)),memory_order_relaxed);
 free(dir);vm->heap_blocks=NULL;
}
#include "gc_reverse.inc"
