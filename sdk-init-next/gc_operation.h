#pragma once
#include "gc_runtime.h"
#include <string.h>
#include <stdio.h>
extern int sdk_trace_fprintf(FILE *,const char *,...);
/* Contract failures only; bounded queue sink, never producer filesystem IO. */
static inline void gc_operation_rejection(const char *file,unsigned line,const char *stage){
 static _Atomic unsigned records;
 if(atomic_fetch_add_explicit(&records,1,memory_order_relaxed)<8)
  sdk_trace_fprintf(stderr,"GC_OPERATION_REJECT file=%s line=%u stage=%s\n",file,line,stage);
}

/* An operation owns its native borrow interval, or reuses the caller's
 * matching VM/guest-depth interval. Cleanup runs on every C return. */
struct gc_operation {
 struct dvm_gc_native_scope scope;
 bool owned;
};
static inline bool gc_operation_begin_at(struct dvm *v,struct gc_operation *g,const char *file,unsigned line){
 memset(g,0,sizeof *g);
 if(dvm_gc_payload_scope_active(v))return true;
 g->owned=true;
 bool ok=dvm_gc_native_enter(v,&g->scope,0,"()V",NULL,0);
 if(!ok)gc_operation_rejection(file,line,"begin");
 return ok;
}
#define gc_operation_begin(v,g) gc_operation_begin_at(v,g,__FILE__,__LINE__)
static inline void gc_operation_end(struct gc_operation *g){
 if(g->owned)dvm_gc_native_leave(&g->scope);
}
static inline void gc_operation_result(struct gc_operation *g,const char *sig,dvm_ref ref){
 if(g->owned)dvm_gc_native_leave_result(&g->scope,sig,ref);
}
static inline bool gc_operation_admit_at(struct dvm *v,dvm_ref ref,const char *file,unsigned line){
 if(!dvm_gc_payload_scope_active(v)){
  gc_operation_rejection(file,line,"admit_missing_scope");
  return dvm_gc_contract_fail(v,GC_CONTRACT_MISSING_SCOPE);
 }
 if(!ref)return true;
 if(dvm_gc_require_payload_borrow(v,ref))return true;
 /* A dead/invalid receiver was already handled by each original dispatcher.
  * Keep that path and its exact exception; never treat a failed live borrow
  * as a successful admission. */
 return dvm_gc_payload_scope_active(v)&&!dvm__obj(v,ref);
}
#define gc_operation_admit(v,ref) gc_operation_admit_at(v,ref,__FILE__,__LINE__)
static inline struct dvm_object *gc_operation_object(struct dvm *v,dvm_ref ref){
 if(!gc_operation_admit(v,ref))return NULL;
 return dvm__obj(v,ref);
}

/* Visitors receive a raw body under the collector's GIL. Unlike ordinary
 * lookups they run while auto-borrow is suppressed (gc->visiting). Explicit
 * checked pin keeps both body and owner payload immovable for the callback. */
struct gc_operation_pin {struct dvm *vm;dvm_ref ref;bool held;};
static inline bool gc_operation_pin_begin(struct dvm *v,struct gc_operation_pin *p,struct dvm_object *o){
 memset(p,0,sizeof *p);p->vm=v;
 /* dvm_create has a TLS owner guard before installing the GIL. A matching
  * authorized operation proves that boot owner; gc_initializing alone does
  * not. checked_pin repeats the authoritative ownership check. */
 if(!dvm_gil_held(v)&&!dvm_gc_payload_scope_active(v))return dvm_gc_contract_fail(v,GC_CONTRACT_NON_GIL);
 p->ref=dvm_gc_ref_of(v,o);
 if(!p->ref)return dvm_gc_contract_fail(v,GC_CONTRACT_IDENTITY);
 p->held=dvm_gc_pin_checked(v,p->ref);return p->held;
}
static inline void gc_operation_pin_end(struct gc_operation_pin *p){
 if(p->held)(void)dvm_gc_unpin_checked(p->vm,p->ref);
}
