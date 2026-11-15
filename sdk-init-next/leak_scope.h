#pragma once
#include <stdbool.h>
#include <stdint.h>
struct dvm;
struct dvm_method;
/* Correctness guard, not a sampling probe. No heap walks or IO. Unknown
 * effects poison the proof; objects then retain their original lifetime. */
enum leak_reason { LEAK_STORE=1,LEAK_UNKNOWN_CALL,LEAK_UNKNOWN_JNI,
 LEAK_EXCEPTION,LEAK_MONITOR,LEAK_NESTED,LEAK_OBSERVER,LEAK_RETURN,
 LEAK_RETAINED_ROOT,LEAK_CAPACITY,LEAK_REASON_COUNT };
struct leak_scope {struct dvm *vm;uint32_t array,box;uint64_t array_id,box_id;unsigned reason;};
extern _Thread_local struct leak_scope *leak_scope_current;
bool leak_scope_begin(struct leak_scope *,struct dvm *,uint32_t,uint32_t,int64_t);
void leak_scope_end(struct leak_scope *,bool,uint32_t);
void leak_scope_cancel(struct dvm *,unsigned);
/* A phase-local fail-closed check, not an observer certificate. Recheck at
 * return: a post-only observer must never bypass a pre-only presence test. */
static inline void leak_scope_raw_observers(struct dvm *vm,bool present){
 if(leak_scope_current&&present)leak_scope_cancel(vm,LEAK_OBSERVER);
}
bool leak_scope_mentions(struct dvm *,uint32_t,const uint32_t *,int);
bool leak_scope_box(struct dvm *,uint32_t);
void leak_scope_method(struct dvm *,const struct dvm_method *,uint32_t,const uint32_t *,int);
bool leak_scope_external_allowed(struct dvm *,const char *,const char *,const char *,uint32_t,const uint32_t *,int);
void leak_scope_opcode(struct dvm *,unsigned);
void leak_scope_monitor(struct dvm *,uint32_t);
void leak_scope_jni(struct dvm *,const char *);
void leak_scope_status(void); /* independent arena observer only */
bool leak_scope_owned_long(struct dvm *,uint32_t,int64_t);
bool dvm_leak_collection_off(struct dvm *);
bool dvm_leak_retire_pair(struct dvm *,uint32_t,uint32_t,uint64_t,uint64_t);
void dvm_leak_reap_pairs(struct dvm *);
void leak_scope_reaped(void);

/* Canonical linked observers are enrolled before any guest execution.  This
 * certificate is an in-process identity/shape guard, not native-body proof. */
enum leak_observer_role { LEAK_TASK_PRE,LEAK_TASK_POST,LEAK_PANEL_PRE,LEAK_PANEL_POST,LEAK_OBSERVER_ROLES };
struct leak_observer_certificate {
 uintptr_t linked;uint64_t prefix_hash;unsigned prefix_bytes,role,selector;
 const char *owner,*name,*sig,*receiver,*shape;
};
void leak_observer_bootstrap(void);
bool leak_observer_check(struct dvm *,unsigned,uintptr_t,const struct dvm_method *,
 uint32_t,const uint32_t *,int,const void *,struct leak_observer_certificate *);

enum leak_certificate_reject { LEAK_CERT_IDENTITY,LEAK_CERT_PREFIX,LEAK_CERT_GENERATION,
 LEAK_CERT_GIL,LEAK_CERT_DISPATCH,LEAK_CERT_METHOD,LEAK_CERT_SHAPE,LEAK_CERT_REJECT_COUNT };
uint64_t leak_observer_rejections(unsigned);
