/* Real interpreter + builtin boundary matrix. No collection/movement. */
struct e1_matrix_case {
 unsigned depth,type;bool fail,leaf_ok,roots_ok;unsigned reached;
 dvm_ref existing,value,error;unsigned char *borrow;
 struct dvm_method guest[3],leaf;
};
static _Thread_local struct e1_matrix_case *e1_matrix;
static bool e1_matrix_leaf(struct dvm *vm,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 (void)self;(void)a;(void)n;struct e1_matrix_case *t=e1_matrix;if(!t)return false;
 t->value=t->type==0||t->type==2?dvm_new_array(vm,'B',"B",9):t->existing;
 struct dvm_object *o=dvm__obj(vm,t->value);
 t->leaf_ok=o&&gc_current_scope&&gc_current_scope->guest_depth==gc_guest_execution_depth&&o->gc_body_borrows&&o->gc_payload_borrows;
 if(t->type==3)t->leaf_ok=t->leaf_ok&&dvm_array_data(vm,t->value)==t->borrow&&t->borrow[0]==0x6b;
 if(t->fail){dvm__throw(vm,"java/lang/RuntimeException","e1-matrix-original");t->error=dvm_exception(vm);return false;}
 if(t->type==0)out->i=(int32_t)t->value;else out->l=t->value;
 return t->leaf_ok;
}
static void e1_matrix_probe(struct dvm *vm){
 struct e1_matrix_case *t=e1_matrix;if(!t||!gc_exec_frames)return;
 struct frame *fr=gc_exec_frames->frame;unsigned level=0;
 while(level<t->depth&&fr->m!=&t->guest[level])level++;
 if(level==t->depth)return;
 if(gc_guest_execution_depth>t->reached)t->reached=gc_guest_execution_depth;
 union dvm_value result={0};bool called=dvm_call(vm,level+1<t->depth?&t->guest[level+1]:&t->leaf,0,NULL,0,&result);
 if(t->fail){
  dvm_ref pending=dvm_exception(vm);struct gc2_root_check roots={.vm=vm,.wanted=pending};
  t->roots_ok=t->roots_ok&&!called&&pending&&pending==t->error&&dvm_gc_visit_roots(vm,DVM_GC_ROOT_STRONG,gc2_root_check_visit,&roots)&&roots.seen&&!roots.invalid;
  /* Each actual guest frame rethrows the SAME object with opcode throw.
   * Preserve identity/message, rather than synthesizing a replacement. */
  fr->regs[0]=pending;dvm_clear_exception(vm);
 }else{t->roots_ok=t->roots_ok&&called&&!dvm_exception(vm);fr->regs[0]=result.u;}
}
static bool e1_nested_matrix_selftest(struct dvm *vm){
 if(gc_exec_test_probe||e1_matrix||dvm_exception(vm))return false;
 FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/gc2-nested-matrix.txt","w");if(!f)return false;
 const char *names[]={"primitive","existing_ref","new_object","borrowed_object"};
 unsigned passed=0,total=0;bool quiet=vm->quiet_uncaught;vm->quiet_uncaught=true;
 for(unsigned depth=1;depth<=3;depth++)for(unsigned type=0;type<4;type++)for(unsigned fail=0;fail<2;fail++){
  total++;struct e1_matrix_case t={.depth=depth,.type=type,.fail=fail,.roots_ok=true};
  struct dvm_gc_native_scope parent={0};uint32_t handles_before=0,strong_before=0,loans_before=0;
  dvm_gc_handle_counts(vm,&handles_before,&strong_before,&loans_before);
  bool entered=dvm_gc_native_enter(vm,&parent,0,"()V",NULL,0);
  if(type==1||type==3){t.existing=dvm_new_array(vm,'B',"B",9);t.borrow=dvm_array_data(vm,t.existing);if(t.borrow)t.borrow[0]=0x6b;}
  struct dvm_class *cls=dvm_find_class_or_external(vm,"java/lang/Object");
  static const uint16_t ret_int[]={0x000f},ret_ref[]={0x0011},throw_ref[]={0x0027};
  char *sig=type==0?"()I":"()Ljava/lang/Object;";
  for(unsigned j=0;j<depth;j++){
   t.guest[j]=(struct dvm_method){.cls=cls,.name="e1MatrixGuest",.sig=sig,.access=DEX_ACC_STATIC,.has_code=true,.ret_kind=type==0?'I':'L'};
   t.guest[j].code.registers_size=1;t.guest[j].code.insns=fail?throw_ref:type==0?ret_int:ret_ref;t.guest[j].code.insns_size=1;
  }
  t.leaf=(struct dvm_method){.cls=cls,.name="e1MatrixNativeLeaf",.sig=sig,.access=DEX_ACC_STATIC,.ret_kind=type==0?'I':'L',.builtin=e1_matrix_leaf};
  e1_matrix=&t;gc_exec_test_probe=e1_matrix_probe;union dvm_value result={0};
  unsigned starting_depth=gc_guest_execution_depth;
  bool called=entered&&dvm_call(vm,&t.guest[0],0,NULL,0,&result);
  gc_exec_test_probe=NULL;e1_matrix=NULL;
  struct dvm_object *value=gc_peek_object(vm,t.value);unsigned child_loans=value?value->gc_body_borrows:0;
  dvm_ref exception=dvm_exception(vm);bool semantics=false;
  if(fail){
   struct dvm_object *err=gc_peek_object(vm,exception);struct gc2_root_check roots={.vm=vm,.wanted=exception};
   semantics=!called&&exception==t.error&&err&&err->cls&&!strcmp(err->cls->name,"java/lang/RuntimeException")&&dvm_gc_visit_roots(vm,DVM_GC_ROOT_STRONG,gc2_root_check_visit,&roots)&&roots.seen&&!roots.invalid;
  }else if(type==0)semantics=called&&result.u==t.value&&value&&!gc2_scope_contains(&parent,t.value)&&!value->gc_body_borrows&&!value->gc_payload_borrows;
  else semantics=called&&result.l==t.value&&value&&gc2_scope_contains(&parent,t.value)&&value->gc_body_borrows&&value->gc_payload_borrows;
  if(type==3)semantics=semantics&&t.borrow&&t.borrow[0]==0x6b;
  bool ok=entered&&t.leaf_ok&&t.roots_ok&&semantics&&t.reached==starting_depth+depth&&gc_guest_execution_depth==starting_depth;
  uint32_t parent_count=parent.count;dvm_clear_exception(vm);dvm_gc_native_leave(&parent);
  value=gc_peek_object(vm,t.value);ok=ok&&value&&!value->gc_body_borrows&&!value->gc_payload_borrows;
  uint32_t handles_after=0,strong_after=0,loans_after=0;dvm_gc_handle_counts(vm,&handles_after,&strong_after,&loans_after);
  ok=ok&&handles_before==handles_after&&strong_before==strong_after&&loans_before==loans_after;
  dprintf(fileno(f),"%s depth=%u return=%s exception=%u actual_dvm_call=1 guest_depth_seen=%u leaf_ok=%d roots_ok=%d semantics=%d parent_borrows=%u child_borrows_after_return=%u handles_before=%u handles_after=%u strong_before=%u strong_after=%u loans_before=%u loans_after=%u result_ref=%u pending_exception_ref=%u\n",ok?"PASS":"FAIL",depth,names[type],fail,t.reached,t.leaf_ok,t.roots_ok,semantics,parent_count,child_loans,handles_before,handles_after,strong_before,strong_after,loans_before,loans_after,result.l,exception);
  if(ok)passed++;
  if(t.value)dvm__discard_temporary(vm,t.value);if(t.existing&&t.existing!=t.value)dvm__discard_temporary(vm,t.existing);
 }
 vm->quiet_uncaught=quiet;dprintf(fileno(f),"E1_NESTED_MATRIX %s pid=%d checks=%u passed=%u failed=%u real_vm_collection_enabled=0 real_vm_moving_enabled=0\n",passed==total?"PASS":"FAIL",getpid(),total,passed,total-passed);
 bool io_ok=!ferror(f);fclose(f);return passed==total&&io_ok;
}
