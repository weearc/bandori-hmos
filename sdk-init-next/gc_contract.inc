/* No filesystem IO here. Failure counters are consumed by the arena worker.
 * Only the creating thread can access its VM before the GIL is installed.
 * A process-wide teardown flag is NOT proof of thread ownership. */
static _Atomic uint64_t gc_contract_faults[GC_CONTRACT_REASONS];
static _Thread_local bool gc_contract_raising;
static _Thread_local struct dvm *gc_contract_boot_vm;
struct gc_contract_boot_guard {struct dvm *previous;};
static void gc_contract_boot_restore(struct gc_contract_boot_guard *g){gc_contract_boot_vm=g->previous;}
static bool gc_contract_access(struct dvm *vm){
 return vm&&(dvm_gil_held(vm)||(vm->gc_initializing&&gc_contract_boot_vm==vm));
}
void dvm_gc_contract_note(unsigned reason){
 if(reason<GC_CONTRACT_REASONS)atomic_fetch_add_explicit(&gc_contract_faults[reason],1,memory_order_relaxed);
}
bool dvm_gc_contract_fail(struct dvm *vm,unsigned reason){
 if(reason>=GC_CONTRACT_REASONS)reason=GC_CONTRACT_IDENTITY;
 dvm_gc_contract_note(reason);
 /* Never manufacture a pending exception from a thread without VM ownership.
  * Checked APIs return false in that case and latch collection admission off. */
 if(gc_contract_access(vm)&&!vm->gc_destroying&&!gc_contract_raising&&!dvm_exception(vm)){
  gc_contract_raising=true;
  dvm__throw(vm,"java/lang/IllegalStateException","GC contract violation reason=%u",reason);
  gc_contract_raising=false;
 }
 return false;
}
bool dvm_gc_contract_healthy(void){
 for(unsigned i=0;i<GC_CONTRACT_REASONS;i++)if(i!=GC_CONTRACT_STALE_HANDLE&&atomic_load_explicit(&gc_contract_faults[i],memory_order_relaxed))return false;
 return true;
}
void dvm_gc_contract_snapshot(uint64_t *out){
 if(out)for(unsigned i=0;i<GC_CONTRACT_REASONS;i++)out[i]=atomic_load_explicit(&gc_contract_faults[i],memory_order_relaxed);
}
bool dvm_gc_pin_checked(struct dvm *vm,uint32_t ref){
 if(!gc_contract_access(vm))return dvm_gc_contract_fail(vm,GC_CONTRACT_NON_GIL);
 struct dvm_object *o=dvm__obj(vm,ref);
 if(!o)return ref?dvm_gc_contract_fail(vm,GC_CONTRACT_IDENTITY):true;
 if(o->pins==UINT32_MAX)return dvm_gc_contract_fail(vm,GC_CONTRACT_PIN_OVERFLOW);
 ++o->pins;return true;
}
bool dvm_gc_unpin_checked(struct dvm *vm,uint32_t ref){
 if(!gc_contract_access(vm))return dvm_gc_contract_fail(vm,GC_CONTRACT_NON_GIL);
 struct dvm_object *o=dvm__obj(vm,ref);
 if(!o)return ref?dvm_gc_contract_fail(vm,GC_CONTRACT_IDENTITY):true;
 if(!o->pins)return dvm_gc_contract_fail(vm,GC_CONTRACT_PIN_UNDERFLOW);
 --o->pins;return true;
}
