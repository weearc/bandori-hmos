/* Included in isolated SATB module only. No VM references enter this heap. */
static size_t block_index(gcs_heap *h,gcs_object *o){
 for(size_t i=0;i<h->count;i++)if(h->objects[i]==o)return i;
 return SIZE_MAX;
}
int gcs_pin(gcs_heap *h,gcs_object *o){
 if(!h||!h->page_backed)return GCS_INVALID;pthread_mutex_lock(&h->lock);
 size_t i=block_index(h,o);int rc=GCS_INVALID;
 if(!h->active&&i!=SIZE_MAX&&!h->blocks[i].retired&&h->blocks[i].pins!=UINT32_MAX){h->blocks[i].pins++;rc=GCS_OK;}
 pthread_mutex_unlock(&h->lock);return rc;
}
int gcs_unpin(gcs_heap *h,gcs_object *o){
 if(!h||!h->page_backed)return GCS_INVALID;pthread_mutex_lock(&h->lock);
 size_t i=block_index(h,o);int rc=GCS_INVALID;
 if(!h->active&&i!=SIZE_MAX&&h->blocks[i].pins){h->blocks[i].pins--;rc=GCS_OK;}
 pthread_mutex_unlock(&h->lock);return rc;
}
int gcs_evacuate_checked(gcs_heap *h,gcs_object **roots,size_t count,gcs_root_writable_fn writable,void *context){
 if(!h||count>SIZE_MAX/sizeof *roots||!writable)return GCS_INVALID;
 if(count&&(!roots||!writable(context,roots,count*sizeof *roots)))return GCS_INVALID;
 return gcs_evacuate(h,roots,count);
}
gcs_heap *gcs_create_evacuating(size_t bytes,size_t cap){
 size_t page=host_page_size();
 if(page<=0||!bytes||bytes>SIZE_MAX-((size_t)page-1))return NULL;
 bytes=(bytes+(size_t)page-1)/(size_t)page*(size_t)page;
 gcs_heap *h=gcs_create(1,cap,cap);if(!h)return NULL;
 void *arena=mmap(NULL,bytes,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
 if(arena==MAP_FAILED){gcs_destroy(h);return NULL;}
 h->blocks=calloc(cap,sizeof *h->blocks);
 if(!h->blocks){munmap(arena,bytes);gcs_destroy(h);return NULL;}
 free(h->arena);h->arena=arena;h->bytes=bytes;h->page_size=(size_t)page;
 h->page_backed=true;h->free_head=SIZE_MAX;h->evac.reserved_bytes=bytes;return h;
}
gcs_object *gcs_resolve(gcs_heap *h,gcs_object *o){
 if(!h)return NULL;pthread_mutex_lock(&h->lock);
 gcs_object *result=resolve_locked(h,o);pthread_mutex_unlock(&h->lock);return result;
}
uint64_t gcs_borrow(gcs_heap *h,gcs_object *o){
 if(!h||!h->page_backed)return 0;pthread_mutex_lock(&h->lock);
 size_t i=block_index(h,o);uint64_t token=0;
 if(i!=SIZE_MAX&&h->blocks[i].borrows!=UINT32_MAX){
  h->blocks[i].borrows++;token=((uint64_t)h->blocks[i].generation<<32)|(i+1);
 }
 pthread_mutex_unlock(&h->lock);return token;
}
int gcs_release(gcs_heap *h,uint64_t token){
 if(!h||!h->page_backed)return GCS_INVALID;pthread_mutex_lock(&h->lock);
 uint32_t slot=(uint32_t)token,gen=(uint32_t)(token>>32);int result=GCS_INVALID;
 if(slot&&slot<=h->count){struct gcs_block *b=&h->blocks[slot-1];
  if(h->objects[slot-1]&&b->generation==gen&&b->borrows){b->borrows--;result=GCS_OK;}}
 pthread_mutex_unlock(&h->lock);return result;
}
void gcs_evac_stats(gcs_heap *h,struct gcs_evac_stats *out){
 if(!h||!out)return;pthread_mutex_lock(&h->lock);*out=h->evac;pthread_mutex_unlock(&h->lock);
}
static int reclaim_retired(gcs_heap *h){
 /* Preserve every transitively reachable forwarding stub, not just the
  * originally borrowed address: A->oldB->newB must remain dereferenceable. */
 for(size_t i=0;i<h->count;i++)h->blocks[i].keep=false;
 for(size_t i=0;i<h->count;i++)if(h->objects[i]&&h->blocks[i].borrows){
  gcs_object *p=h->objects[i];
  for(size_t n=0;n<=h->count;n++){
   size_t j=block_index(h,p);if(j==SIZE_MAX)return GCS_INVALID;
   h->blocks[j].keep=true;
   gcs_object *next=atomic_load_explicit(&p->brooks,memory_order_acquire);
   if(next==p)break;p=next;if(n==h->count)return GCS_INVALID;
  }
 }
 h->evac.retained_forwarding_bytes=0;
 for(size_t i=0;i<h->count;i++)if(h->objects[i]&&h->blocks[i].retired){
  struct gcs_block *b=&h->blocks[i];
  if(b->keep){h->evac.retained_forwarding_bytes+=b->bytes;continue;}
  unsigned char *address=h->arena+b->offset;
  if(mprotect(address,b->bytes,PROT_NONE)||madvise(address,b->bytes,MADV_DONTNEED))return GCS_INVALID;
  /* Only after both OS operations succeed is this block reusable. */
  h->objects[i]=NULL;b->retired=false;b->next_free=h->free_head;h->free_head=i;
  h->evac.physical_reclaimed_bytes+=b->bytes;h->evac.committed_bytes-=b->bytes;h->evac.decommit_calls++;
 }
 return GCS_OK;
}
int gcs_evacuate(gcs_heap *h,gcs_object **roots,size_t count){
 if(!h||!h->page_backed||(!roots&&count))return GCS_INVALID;
 /* Capture explicit borrowed references as extra roots. Collection is
  * quiescent here; synchronization with mutating evacuation is GC10. */
 size_t original_count=h->count,nroots=count;
 if(count>SIZE_MAX-original_count)return GCS_OOM;
 gcs_object **all=calloc(count+original_count+1,sizeof *all);
 gcs_object **copies=calloc(original_count?original_count:1,sizeof *copies);
 gcs_object **sources=calloc(original_count?original_count:1,sizeof *sources);
 if(!all||!copies||!sources){free(all);free(copies);free(sources);return GCS_OOM;}
 for(size_t i=0;i<original_count;i++)if(h->objects[i]&&!h->blocks[i].retired)sources[i]=h->objects[i];
 for(size_t i=0;i<count;i++)all[i]=roots[i];
 for(size_t i=0;i<original_count;i++)if(h->objects[i]&&(h->blocks[i].borrows||h->blocks[i].pins))all[nroots++]=h->objects[i];
 int result=gcs_begin(h,all,nroots,false);if(!result)result=gcs_finish(h);
 free(all);if(result){free(copies);free(sources);return result;}
 /* All copies must be allocated before publishing a single forwarding
  * pointer. OOM is explicit and cannot produce a successful collection. */
 for(size_t i=0;i<original_count;i++){
  gcs_object *o=sources[i];if(!o)continue;
  if(!(atomic_load_explicit(&o->flags,memory_order_acquire)&DVM_GC_MARKED))continue;
  if(h->blocks[i].pins)continue;
  copies[i]=gcs_alloc(h,o->layout,o->nslots);
  if(!copies[i]){free(copies);free(sources);return h->error?h->error:GCS_OOM;}
  for(size_t j=0;j<o->nslots;j++)atomic_store_explicit(&copies[i]->slots[j],atomic_load_explicit(&o->slots[j],memory_order_acquire),memory_order_release);
 }
 for(size_t i=0;i<original_count;i++)if(copies[i]){
  atomic_store_explicit(&sources[i]->brooks,copies[i],memory_order_release);h->evac.copied_objects++;
 }
 /* Repair managed roots/fields before releasing source pages. Brooks still
  * serves borrowed raw pointers; managed slots no longer need old stubs. */
 for(size_t i=0;i<count;i++)if(roots[i]){roots[i]=resolve_locked(h,roots[i]);if(!roots[i])result=GCS_INVALID;}
 for(size_t i=0;i<original_count;i++)if(copies[i]||(sources[i]&&h->blocks[i].pins)){
  gcs_object *o=copies[i]?copies[i]:sources[i];
  for(size_t j=0;j<o->layout->instance_count;j++){
   size_t slot=o->layout->instance_slots[j];gcs_object *old=(gcs_object *)atomic_load_explicit(&o->slots[slot],memory_order_acquire);
   gcs_object *target=old?resolve_locked(h,old):NULL;
   if(old&&!target)result=GCS_INVALID;
   atomic_store_explicit(&o->slots[slot],(uintptr_t)target,memory_order_release);
  }
 }
 for(size_t i=0;i<original_count;i++)if(sources[i]){
  if(h->blocks[i].pins)continue;
  h->blocks[i].retired=true;h->evac.logical_reclaimed_bytes+=h->blocks[i].bytes;
  if(!copies[i])h->evac.logical_dead_bytes+=h->blocks[i].bytes;
 }
 free(copies);free(sources);
 if(result)return fail_locked(h,result);
 return reclaim_retired(h);
}

static void evac_report(FILE *csv,const char *name,gcs_heap *h){
 struct gcs_evac_stats s;gcs_evac_stats(h,&s);
 if(csv)fprintf(csv,"isolated-evacuate,%s,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,0,0\n",name,s.logical_reclaimed_bytes,s.physical_reclaimed_bytes,s.retained_forwarding_bytes,s.logical_dead_bytes,s.committed_bytes,s.reserved_bytes,s.reuse_hits,s.copied_objects,s.decommit_calls);
}
static size_t free_resident_pages(gcs_heap *h){
 size_t total=0;
 for(size_t i=0;i<h->count;i++)if(!h->objects[i]&&h->blocks[i].bytes){
  size_t pages=h->blocks[i].bytes/h->page_size;
  unsigned char *vec=malloc(pages);if(!vec)return SIZE_MAX;
  int rc=mincore(h->arena+h->blocks[i].offset,h->blocks[i].bytes,vec);
  if(rc){free(vec);return SIZE_MAX;}
  for(size_t j=0;j<pages;j++)total+=!!(vec[j]&1);
  free(vec);
 }
 return total;
}
int gc_isolated_evacuate_selftest(FILE *f,FILE *csv){
 uint32_t ref[]={0};struct dvm_gc_layout leaf={0},node={.instance_slots=ref,.instance_count=1};
 unsigned checks=0,failed=0;
 if(csv)fprintf(csv,"stage,case,logical_reclaimed_bytes,physical_reclaimed_bytes,retained_forwarding_bytes,logical_dead_bytes,committed_bytes,reserved_bytes,reuse_hits,copied_objects,decommit_calls,real_vm_moving_enabled,real_vm_collection_enabled\n");
#define EVAC_CHECK(name,expr) do{bool pass=(expr);checks++;failed+=!pass;if(f)fprintf(f,"GC5_%s %s\n",name,pass?"PASS":"FAIL");}while(0)
 gcs_heap *h=gcs_create_evacuating(32*1024*1024,4096);if(!h)return 1;
 gcs_object *a=gcs_alloc(h,&leaf,0);int result=gcs_evacuate(h,&a,1);
 EVAC_CHECK("a_single_physical",result==0&&a&&h->evac.physical_reclaimed_bytes>0&&h->evac.physical_reclaimed_bytes==h->evac.logical_reclaimed_bytes&&h->evac.retained_forwarding_bytes==0);
 EVAC_CHECK("a_kernel_mincore_released",free_resident_pages(h)==0);
 evac_report(csv,"a",h);gcs_destroy(h);
 h=gcs_create_evacuating(32*1024*1024,4096);if(!h)return 1;
 gcs_object *root=NULL,*last=NULL;bool setup=true;
 for(unsigned i=0;i<1000;i++){gcs_object *o=gcs_alloc(h,&node,1);if(!o){setup=false;break;}if(last&&gcs_put_ref(h,last,0,o))setup=false;if(!root)root=o;last=o;}
 uint64_t borrow=gcs_borrow(h,root);gcs_object *old=root;result=gcs_evacuate(h,&root,1);
 size_t length=0;for(gcs_object *o=root;o&&length<=1000;o=gcs_get_ref(h,o,0))length++;
 bool resolved=gcs_resolve(h,old)==root;
 int release=gcs_release(h,borrow);if(!result)result=gcs_evacuate(h,&root,1);
 EVAC_CHECK("b_chain1000_from_returned",setup&&result==0&&release==0&&resolved&&length==1000&&h->evac.retained_forwarding_bytes==0&&h->evac.physical_reclaimed_bytes==h->evac.logical_reclaimed_bytes&&h->evac.copied_objects==2000);
 evac_report(csv,"b",h);gcs_destroy(h);
 h=gcs_create_evacuating(1024*1024,128);if(!h)return 1;
 a=gcs_alloc(h,&node,1);gcs_object *b=gcs_alloc(h,&node,1),*c=gcs_alloc(h,&node,1);
 setup=!gcs_put_ref(h,a,0,b)&&!gcs_put_ref(h,b,0,c)&&!gcs_put_ref(h,c,0,a);result=gcs_evacuate(h,&a,1);
 EVAC_CHECK("c_cycle3",setup&&result==0&&gcs_get_ref(h,gcs_get_ref(h,gcs_get_ref(h,a,0),0),0)==a&&h->evac.retained_forwarding_bytes==0&&h->evac.physical_reclaimed_bytes==h->evac.logical_reclaimed_bytes);
 evac_report(csv,"c",h);gcs_destroy(h);
 h=gcs_create_evacuating(1024*1024,128);if(!h)return 1;
 a=gcs_alloc(h,&leaf,0);old=a;borrow=gcs_borrow(h,old);result=gcs_evacuate(h,&a,1);
 EVAC_CHECK("d_borrow_retains",result==0&&borrow&&h->evac.physical_reclaimed_bytes<h->evac.logical_reclaimed_bytes&&h->evac.retained_forwarding_bytes==h->page_size&&gcs_resolve(h,old)==a);evac_report(csv,"d-held",h);
 gcs_object *middle=a;result=gcs_evacuate(h,&a,1);
 EVAC_CHECK("e_two_level_idempotent",result==0&&atomic_load(&old->brooks)==middle&&atomic_load(&middle->brooks)==a&&gcs_resolve(h,old)==a&&gcs_resolve(h,gcs_resolve(h,old))==a&&h->evac.retained_forwarding_bytes==2*h->page_size);evac_report(csv,"e",h);
 release=gcs_release(h,borrow);result=gcs_evacuate(h,&a,1);
 EVAC_CHECK("d_release_physical_catches_logical",release==0&&result==0&&h->evac.physical_reclaimed_bytes==h->evac.logical_reclaimed_bytes&&!h->evac.retained_forwarding_bytes);evac_report(csv,"d-released",h);gcs_destroy(h);
 h=gcs_create_evacuating(4*1024*1024,512);if(!h)return 1;
 gcs_object *roots[50];for(unsigned i=0;i<100;i++){gcs_object *o=gcs_alloc(h,&leaf,1);if(o)gcs_put_scalar(h,o,0,i+100);if(i<50)roots[i]=o;}
 size_t before=h->evac.committed_bytes;result=gcs_evacuate(h,roots,50);
 EVAC_CHECK("f_half_dead_committed",result==0&&h->evac.committed_bytes==before/2&&h->evac.logical_dead_bytes==before/2);
 if(f)fprintf(f,"GC5_COMMITTED before=%zu after=%zu dead_objects=50 total_objects=100\n",before,h->evac.committed_bytes);evac_report(csv,"f",h);
 EVAC_CHECK("f_kernel_mincore_released",free_resident_pages(h)==0);
 size_t hits=h->evac.reuse_hits;uintptr_t expected=(uintptr_t)(h->arena+h->blocks[h->free_head].offset);
 gcs_object *again=gcs_alloc(h,&node,1);bool zeroed=again&&atomic_load(&again->slots[0])==0;int set=gcs_put_ref(h,again,0,roots[0]);
 EVAC_CHECK("h_reuse_and_live_integrity",again&&(uintptr_t)again==expected&&zeroed&&set==0&&gcs_get_ref(h,again,0)==roots[0]&&h->evac.reuse_hits>hits&&gcs_resolve(h,roots[49])==roots[49]&&atomic_load(&roots[49]->slots[0])==149);evac_report(csv,"h",h);gcs_destroy(h);
 h=gcs_create_evacuating(1024*1024,128);if(!h)return 1;
 result=gcs_evacuate(h,NULL,0);EVAC_CHECK("g_empty",result==0&&h->evac.committed_bytes==0);
 a=gcs_alloc(h,&leaf,0);result=gcs_evacuate(h,&a,1);EVAC_CHECK("g_single",result==0&&a);
 result=gcs_evacuate(h,NULL,0);EVAC_CHECK("g_all_dead",result==0&&h->evac.committed_bytes==0&&h->evac.physical_reclaimed_bytes==h->evac.logical_reclaimed_bytes);evac_report(csv,"g",h);gcs_destroy(h);
 if(f)fprintf(f,"GC5_ISOLATED_SELFTEST %s checks=%u failed=%u real_vm_moving_enabled=0 real_vm_collection_enabled=0\n",failed?"FAIL":"PASS",checks,failed);
#undef EVAC_CHECK
 return failed?1:0;
}
