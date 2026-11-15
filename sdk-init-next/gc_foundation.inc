/* Included in dvm.c. All object inspection runs with the VM GIL held. */
#include "next_arena.h"
#include <time.h>
#ifdef fprintf
#pragma push_macro("fprintf")
#undef fprintf
#define DVM_GC_RESTORE_FPRINTF 1
#endif

struct gc_root_observation {struct dvm *vm;uint64_t slots,invalid;};
static void gc_observe_root(dvm_ref *slot,void *user)
{
   struct gc_root_observation *s=user;++s->slots;
   if(!slot || (*slot&&!gc_peek_object(s->vm,*slot)))++s->invalid;
}

bool dvm_gc_observe(struct dvm *vm,const char *path,const char *reason)
{
   if (!vm || !path || !reason) return false;
   struct vm_arena_region_snapshot s;
   if (!vm_arena_region_snapshot(&s) || !s.active) return false;
   uint32_t objects=0,pins=0,unknown=0,bad_self=0,body_borrows=0,payload_borrows=0,borrowed_objects=0;
   uint64_t typed_entries=0,typed_slots=0,conservative_frames=0,conservative_slots=0,tstates=0,result_candidates=0;
   bool execution_roots_complete=dvm_gc_execution_root_counts(vm,&typed_entries,&typed_slots,&conservative_frames,&conservative_slots,&tstates,&result_candidates);
   struct gc_root_observation roots={.vm=vm};
   bool strong_roots_complete=dvm_gc_visit_roots(vm,DVM_GC_ROOT_STRONG,gc_observe_root,&roots)&&!roots.invalid;
   for (uint32_t r=1;r<=vm->heap_size;++r) {
      if(!heap_slot(vm,r)->live)continue;
      struct dvm_object *o=gc_peek_object(vm,r);
      if (!o) {++bad_self;continue;}
      ++objects;pins+=o->pins;
      body_borrows+=o->gc_body_borrows;payload_borrows+=o->gc_payload_borrows;
      borrowed_objects+=!!(o->gc_body_borrows||o->gc_payload_borrows);
      unknown+=dvm_gc_payload_requires_visitor(o)&&dvm_gc_payload_visit(vm,o,NULL,NULL)<0;
      bad_self+=atomic_load_explicit(&o->gc_self,memory_order_acquire)!=o;
   }
   static pid_t csv_pid;
   pid_t pid=getpid();FILE *f=fopen(path,csv_pid==pid?"a+":"w+");if (!f) return false;
   csv_pid=pid;
   fseek(f,0,SEEK_END);
   if (!ftell(f)) fprintf(f,"pid,epoch,stage,event,collection_enabled,pause_ms,objects,reclaimed_objects,reclaimed_bytes,committed_bytes,live_allocation_bytes,live_requested_bytes,free_bytes,metadata_bytes,bitmap_bytes,waste_bytes,regions,humongous_regions,reuse_hits,denied,pins,unregistered_payloads,bad_self,gc_handles,gc_strong_handles,gc_borrow_handles,root_providers,native_scopes,body_borrows,payload_borrows,typed_entries,typed_root_slots,conservative_frames,conservative_slots,saved_tstates,result_candidates,execution_roots_complete,borrowed_objects,scope_borrow_max,strong_root_slots,invalid_root_slots,strong_roots_complete,retained_unknown_payloads,quarantine_objects,mutator_handoffs\n");
   /* NA is deliberate: a storage snapshot is not a collection pause. */
   uint32_t handles=0,strong=0,borrows=0,scopes=0,scope_borrow_max=0;
   dvm_gc_handle_counts(vm,&handles,&strong,&borrows);
   if(vm->gc)for(struct dvm_gc_native_scope *q=vm->gc->scopes;q;q=q->next_vm){++scopes;if(q->count>scope_borrow_max)scope_borrow_max=q->count;}
   uint64_t retained_unknown=0,quarantine=0,handoffs=0;dvm_gc_fast_retention_counts(&retained_unknown,&quarantine,&handoffs);
   fprintf(f,"%d,%lld,gc2,%s,0,NA,%u,0,0,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%llu,%llu,%llu,%llu,%llu,%llu,%u,%u,%u,%llu,%llu,%u,%llu,%llu,%llu\n",
      getpid(),(long long)time(NULL),reason,objects,s.committed_bytes,
      s.live_bytes,s.live_requested_bytes,s.free_bytes,s.metadata_bytes,
      s.bitmap_bytes,s.waste_bytes,s.regions,s.humongous_regions,s.reuse_hits,
      s.denied,pins,unknown,bad_self,handles,strong,borrows,vm->gc?vm->gc->providers_count:0,scopes,body_borrows,payload_borrows,
      (unsigned long long)typed_entries,(unsigned long long)typed_slots,(unsigned long long)conservative_frames,(unsigned long long)conservative_slots,(unsigned long long)tstates,(unsigned long long)result_candidates,(unsigned)execution_roots_complete,borrowed_objects,scope_borrow_max,(unsigned long long)roots.slots,(unsigned long long)roots.invalid,(unsigned)strong_roots_complete,(unsigned long long)retained_unknown,(unsigned long long)quarantine,(unsigned long long)handoffs);
   bool ok=!ferror(f);if (fclose(f)) ok=false;return ok;
}

struct gc1_visit_test {uint32_t count;dvm_ref value;bool mismatch;};
static _Atomic(const char *) gc1_last_step="not_run";
static atomic_int gc1_boot_state;
/* Lock-free startup status, also available while app_owner_retire retains a
 * failed VM. No GIL, file access, or heap walk on the UI heartbeat. */
int dvm_gc_foundation_boot_status(char *out,size_t capacity)
{
   int state=atomic_load(&gc1_boot_state);
   if(state!=3){if(capacity)out[0]=0;return 0;}
   snprintf(out,capacity,"E-GC1-SELFTEST-FAIL check=%s",atomic_load(&gc1_last_step));
   return 1;
}
static void gc1_test_visit(dvm_ref *slot,void *user)
{
   struct gc1_visit_test *s=user;++s->count;
   if (!slot || *slot!=s->value) s->mismatch=true;
}

bool dvm_gc_foundation_selftest(struct dvm *vm,const char *path)
{
   atomic_store(&gc1_last_step,"entry");atomic_store(&gc1_boot_state,1);
   if (!vm || !path || dvm_exception(vm)){atomic_store(&gc1_boot_state,3);return false;}
   /* This bounded startup gate must have reliable evidence and IO status.
    * Bypass the lossy async diagnostic cookie, never used on a frame path. */
   atomic_store(&gc1_last_step,"open");
   FILE *f=(fopen)(path,"w");if (!f){atomic_store(&gc1_boot_state,3);return false;}
   fprintf(f,"GC1_HEADER pid=%d sizeof=%zu alignment=%zu self_offset=%zu flags_offset=%zu age_offset=%zu identity_offset=%zu ref_bytes=%zu\n",
      getpid(),sizeof(struct dvm_object),_Alignof(struct dvm_object),offsetof(struct dvm_object,gc_self),
      offsetof(struct dvm_object,gc_flags),offsetof(struct dvm_object,gc_age),
      offsetof(struct dvm_object,identity_generation),sizeof(dvm_ref));
   unsigned checks=0;bool ok=false;const char *stage="metadata";
   dvm_ref obj=0,refs=0,ints=0,string=0,recycled=0;
   struct dvm_class base={0},child={0};
   struct dvm_field bf[2]={
      {.type="Ljava/lang/Object;",.kind='L',.width=1,.slot=0},
      {.type="J",.kind='J',.width=1,.slot=1}};
   struct dvm_field cf[2]={
      {.type="I",.kind='I',.width=1,.slot=2},
      {.type="[B",.kind='L',.width=1,.slot=3}};
   struct dvm_field sf[2]={
      {.type="Ljava/lang/Object;",.kind='L',.width=1,.slot=0},
      {.type="J",.kind='J',.width=1,.slot=1}};
   union dvm_value statics[2]={{0},{0}};
   base.ifields=bf;base.nifields=2;base.islots=2;
   child.super=&base;child.ifields=cf;child.nifields=2;child.islots=4;
   child.sfields=sf;child.nsfields=2;child.nsslots=2;child.sslots=statics;
#define GC1_CHECK(label,expression) do {stage=label;atomic_store(&gc1_last_step,label);if(!(expression))goto done;++checks;fprintf(f,"PASS %s\n",label);fflush(f);}while(0)
   GC1_CHECK("layout_metadata_invalidation_and_wide_union_slots",dvm_gc_layout_metadata_selftest());
   string=dvm_new_string(vm,"gc1-reference");
   obj=dvm_new_object(vm,&child);
   refs=dvm_new_array(vm,'L',"Ljava/lang/Object;",3);
   ints=dvm_new_array(vm,'I',"I",3);
   GC1_CHECK("real_vm_allocations",string&&obj&&refs&&ints);
   struct dvm_object *o=dvm__obj(vm,obj);
   GC1_CHECK("self_header_initialization",atomic_load(&o->gc_self)==o&&!o->gc_flags&&!o->gc_age);
   size_t region_id=0;bool start=false;
   GC1_CHECK("header_backed_by_real_region",vm_arena_locate(o,&region_id,&start)&&region_id);
   GC1_CHECK("guest_object_start_bitmap",vm_arena_object_start(o)&&!vm_arena_object_start((char *)o+16));
   GC1_CHECK("slot_allocation_start_bitmap",vm_arena_locate(o->slots,&region_id,&start)&&start);
   o->slots[0].l=string;o->slots[1].j=string;o->slots[2].i=string;o->slots[3].l=string;
   struct gc1_visit_test v={.value=string};
   GC1_CHECK("inherited_reference_slots_not_primitive_bits",dvm_gc_visit_declared_refs(vm,obj,gc1_test_visit,&v)&&v.count==2&&!v.mismatch);
   statics[0].l=string;statics[1].j=string;v.count=0;
   GC1_CHECK("static_reference_slots_not_wide_primitive",dvm_gc_visit_static_refs(vm,&child,gc1_test_visit,&v)&&v.count==1&&!v.mismatch);
   dvm_ref *a=dvm__obj(vm,refs)->data;for(unsigned i=0;i<3;++i)a[i]=string;
   v.count=0;
   GC1_CHECK("reference_array_32bit_slots",dvm_gc_visit_declared_refs(vm,refs,gc1_test_visit,&v)&&v.count==3&&!v.mismatch);
   memcpy(dvm__obj(vm,ints)->data,a,3*sizeof(dvm_ref));v.count=0;
   GC1_CHECK("primitive_array_not_scanned",dvm_gc_visit_declared_refs(vm,ints,gc1_test_visit,&v)&&!v.count);
   GC1_CHECK("array_payload_covered",!dvm_gc_payload_requires_visitor(dvm__obj(vm,refs)));
   o->data=malloc(16);
   GC1_CHECK("unknown_native_payload_fails_closed",o->data&&dvm_gc_payload_requires_visitor(o));
   object_data_destroy(o);
   dvm_pin(vm,obj);heap_free(vm,obj);
   GC1_CHECK("pinned_temporary_not_freed",dvm__obj(vm,obj)==o&&o->pins==1);
   dvm_unpin(vm,obj);uint64_t generation=o->identity_generation;
   heap_free(vm,obj);
   GC1_CHECK("freed_guest_start_cleared",!vm_arena_object_start(o));
   recycled=dvm_new_object(vm,&child);
   GC1_CHECK("reused_header_resets_self_and_identity",recycled==obj&&atomic_load(&o->gc_self)==o&&o->identity_generation>generation);
   obj=0;
   struct vm_arena_region_snapshot s;
   GC1_CHECK("physical_budget_and_accounting",vm_arena_region_snapshot(&s)&&s.active&&s.regions&&s.bitmap_bytes&&s.committed_bytes<=s.limit_bytes&&s.committed_bytes==s.live_bytes+s.free_bytes+s.metadata_bytes);
   GC1_CHECK("gc_csv_real_snapshot",dvm_gc_observe(vm,"/data/storage/el2/base/haps/entry/files/sdkdex/gc.csv","foundation_selftest"));
   ok=true;
done:
   /* Startup-only breadcrumbs: never called from render/vsync. Flush both
    * sides of every cleanup step so a blocked operation has an exact name. */
#define GC1_CLEANUP(label,expression) do {atomic_store(&gc1_last_step,"cleanup/" #label);fprintf(f,"GC1_CLEANUP before_%s\n",#label);fflush(f);expression;fprintf(f,"GC1_CLEANUP after_%s\n",#label);fflush(f);}while(0)
   GC1_CLEANUP(free_0_recycled,heap_free(vm,recycled));
   GC1_CLEANUP(free_1_obj,heap_free(vm,obj));
   GC1_CLEANUP(free_2_refs,heap_free(vm,refs));
   GC1_CLEANUP(free_3_ints,heap_free(vm,ints));
   GC1_CLEANUP(free_4_string,heap_free(vm,string));
   GC1_CLEANUP(layout_child,dvm_gc_layout_destroy(&child));
   GC1_CLEANUP(layout_base,dvm_gc_layout_destroy(&base));
   GC1_CLEANUP(exception_check,if(dvm_exception(vm)){ok=false;stage="cleanup_exception";});
   fprintf(f,"GC1_CLEANUP before_final_result stream_error=%d\n",ferror(f));fflush(f);
   fprintf(f,"GC1_SELFTEST %s pid=%d checks=%u failed_stage=%s collection_enabled=0 moving_enabled=0\n",ok?"PASS":"FAIL",getpid(),checks,ok?"none":stage);
   if (fflush(f)||ferror(f)){ok=false;stage="cleanup_write";}
   if(fclose(f)){ok=false;stage="cleanup_close";}
   atomic_store(&gc1_last_step,ok?"complete":stage);atomic_store(&gc1_boot_state,ok?2:3);
#undef GC1_CLEANUP
#undef GC1_CHECK
   return ok;
}
#ifdef DVM_GC_RESTORE_FPRINTF
#pragma pop_macro("fprintf")
#undef DVM_GC_RESTORE_FPRINTF
#endif
