/* HAP-process tests over actual VM objects and shared production trace,
 * eligibility and promotion helpers. Fixture-only sweep never visits unrelated
 * VM objects. Real rendezvous/root/native-stack acceptance is in gc-young.csv. */
static bool gc_young_selftest(struct dvm *vm){
 FILE *f=fopen(GC_FAST_DIR "E2-YOUNG-selftest.txt","w");if(!f)return false;
 bool ok=false,was_running=gc_fast_running,visiting=vm->gc?vm->gc->visiting:false;
 const char *stage="setup";unsigned checks=0;dvm_ref a=0,b=0,c=0,garbage=0;
 dvm_ref work[8];struct gc_fast_slice slices[32];
 struct gc_fast_mark s={.vm=vm,.work=work,.capacity=vm->heap_size,.young_only=true,.slices=slices,.slice_capacity=32,.slice_begin=gc_fast_ns()};
 if(!vm->gc||dvm_exception(vm))goto done;
 gc_fast_running=true;vm->gc->visiting=true;
 a=dvm_new_array(vm,'L',"Ljava/lang/Object;",1);
 b=dvm_new_array(vm,'L',"Ljava/lang/Object;",1);
 c=dvm_new_array(vm,'B',"B",9);
 garbage=dvm_new_array(vm,'B',"B",19);
 s.capacity=vm->heap_size;
#define YOUNG_CHECK(label,expr) do{stage=label;if(!(expr))goto done;checks++;fprintf(f,"PASS %s\n",label);}while(0)
 YOUNG_CHECK("new_objects_age_zero",a&&b&&c&&garbage&&!heap_slot(vm,b)->gc_age&&!(heap_slot(vm,b)->gc_flags&DVM_GC_OLD));
 struct dvm_object *oa=heap_slot(vm,a),*ob=heap_slot(vm,b),*oc=heap_slot(vm,c);
 void *b_body=ob,*c_body=oc,*b_data=ob->data;
 ((dvm_ref *)oa->data)[0]=b;((dvm_ref *)ob->data)[0]=c;
 oa->gc_flags|=DVM_GC_OLD;oa->gc_age=GC_YOUNG_PROMOTION_AGE;
 gc_fast_mark_slot(&a,&s);
 YOUNG_CHECK("young_root_does_not_enqueue_old",!s.failed&&s.count==0);
 /* a is deliberately NOT a fixture root. Every old object must be scanned. */
 gc_fast_trace_one(&s,a);
 YOUNG_CHECK("A_old_to_B_young_one_layer",!s.failed&&!s.retain_all&&s.count==1&&s.work[0]==b&&(ob->gc_flags&DVM_GC_MARKED));
 for(size_t i=0;i<s.count&&!s.failed;i++)gc_fast_trace_one(&s,s.work[i]);
 YOUNG_CHECK("old_to_young_to_young_chain",!s.failed&&!s.retain_all&&s.count==2&&(oc->gc_flags&DVM_GC_MARKED));
 YOUNG_CHECK("old_never_young_sweep_candidate",!gc_fast_sweep_eligible(&s,oa));
 YOUNG_CHECK("young_garbage_is_unmarked_candidate",gc_fast_sweep_eligible(&s,heap_slot(vm,garbage))&&!(heap_slot(vm,garbage)->gc_flags&DVM_GC_MARKED));
 size_t young_before=gc_young_used(vm);heap_free(vm,garbage);
 YOUNG_CHECK("young_garbage_reclaimed_and_used_drops",!heap_slot(vm,garbage)->live&&gc_young_used(vm)<young_before);
 YOUNG_CHECK("survivor_first_age",!gc_young_promote(ob)&&ob->gc_age==1&&!(ob->gc_flags&DVM_GC_OLD));
 YOUNG_CHECK("survivor_second_age_promotes",gc_young_promote(ob)&&ob->gc_age==GC_YOUNG_PROMOTION_AGE&&(ob->gc_flags&DVM_GC_OLD));
 YOUNG_CHECK("promoted_not_moved_or_young_reclaimed",ob==b_body&&ob->data==b_data&&!gc_fast_sweep_eligible(&s,ob)&&ob->live);
 YOUNG_CHECK("promotion_idempotent",!gc_young_promote(ob)&&ob->gc_age==GC_YOUNG_PROMOTION_AGE);
 /* Full-mode shared helpers must trace an old root and permit old garbage
  * to be considered by the ordinary full-STW sweep. */
 s.young_only=false;s.count=0;
 oa->gc_flags&=~DVM_GC_MARKED;ob->gc_flags&=~DVM_GC_MARKED;oc->gc_flags&=~DVM_GC_MARKED;
 gc_fast_mark_slot(&a,&s);
 for(size_t i=0;i<s.count&&!s.failed;i++)gc_fast_trace_one(&s,s.work[i]);
 YOUNG_CHECK("full_mode_old_root_transitive_closure",!s.failed&&!s.retain_all&&s.count==3&&(oa->gc_flags&DVM_GC_MARKED)&&(ob->gc_flags&DVM_GC_MARKED)&&(oc->gc_flags&DVM_GC_MARKED));
 YOUNG_CHECK("full_mode_old_sweep_eligibility",gc_fast_sweep_eligible(&s,oa));
 YOUNG_CHECK("chain_bodies_still_stationary",heap_slot(vm,b)==b_body&&heap_slot(vm,c)==c_body&&atomic_load_explicit(&ob->gc_self,memory_order_acquire)==ob&&atomic_load_explicit(&oc->gc_self,memory_order_acquire)==oc);
 ok=true;
done:
 if(a)heap_free(vm,a);if(b)heap_free(vm,b);if(c)heap_free(vm,c);if(garbage)heap_free(vm,garbage);
 if(vm->gc)vm->gc->visiting=visiting;gc_fast_running=was_running;
 fprintf(f,"E2_YOUNG_SELFTEST %s pid=%d checks=%u failed_stage=%s actual_VM=1 HAP_process=1 fixture_only_sweep=1 moving_enabled=0\n",ok?"PASS":"FAIL",getpid(),checks,ok?"none":stage);
 bool io_ok=!ferror(f);if(fclose(f))io_ok=false;return ok&&io_ok;
#undef YOUNG_CHECK
}
