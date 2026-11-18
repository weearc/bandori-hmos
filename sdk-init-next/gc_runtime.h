#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stddef.h>
#include "gc_generation.h"

struct dvm;
struct dvm_class;
struct dvm_object;
typedef uint64_t dvm_gc_handle;
typedef void (*dvm_gc_visit_fn)(uint32_t *,void *);
typedef bool (*dvm_gc_root_fn)(struct dvm *,dvm_gc_visit_fn,void *,void *);
enum { DVM_GC_HANDLE_STRONG=1u,DVM_GC_HANDLE_WEAK=2u,
       DVM_GC_HANDLE_BORROW_BODY=4u,DVM_GC_HANDLE_BORROW_PAYLOAD=8u };
enum { DVM_GC_ROOT_STRONG=1u,DVM_GC_ROOT_WEAK=2u };
enum dvm_gc_contract_reason { GC_CONTRACT_NON_GIL,GC_CONTRACT_PIN_OVERFLOW,
 GC_CONTRACT_PIN_UNDERFLOW,GC_CONTRACT_BORROW_OVERFLOW,GC_CONTRACT_BORROW_UNDERFLOW,
 GC_CONTRACT_IDENTITY,GC_CONTRACT_SCOPE_ORDER,GC_CONTRACT_SCOPE_DEPTH,
 GC_CONTRACT_MISSING_SCOPE,GC_CONTRACT_STALE_HANDLE,GC_CONTRACT_LOAN,
 GC_CONTRACT_REASONS };
bool dvm_gc_contract_fail(struct dvm *,unsigned);
void dvm_gc_contract_note(unsigned);
bool dvm_gc_contract_healthy(void);
void dvm_gc_contract_snapshot(uint64_t *);
bool dvm_gc_pin_checked(struct dvm *,uint32_t);
bool dvm_gc_unpin_checked(struct dvm *,uint32_t);
bool dvm_gc_payload_scope_active(struct dvm *);
bool dvm_gc_require_payload_borrow(struct dvm *,uint32_t);
dvm_gc_handle dvm_gc_handle_new(struct dvm *,uint32_t,unsigned);
uint32_t dvm_gc_handle_ref(struct dvm *,dvm_gc_handle);
bool dvm_gc_handle_drop(struct dvm *,dvm_gc_handle);
bool dvm_gc_root_provider_add(struct dvm *,const char *,unsigned,dvm_gc_root_fn,void *);
void dvm_gc_root_provider_remove(struct dvm *,const char *,void *);
bool dvm_gc_visit_roots(struct dvm *,unsigned,dvm_gc_visit_fn,void *);
struct dvm_object *dvm_gc_resolve(struct dvm *,struct dvm_object *);
void dvm_gc_forget_identity(struct dvm *,uint32_t,uint64_t);
bool dvm_gc_handles_selftest(struct dvm *,const char *);
void dvm_gc_handles_destroy(struct dvm *);
void dvm_gc_handle_counts(struct dvm *,uint32_t *,uint32_t *,uint32_t *);
struct dvm_gc_scope_ref {gen_ref ref;uint64_t identity;};
/* Layout oracle only; never used as a production reference carrier. */
struct dvm_gc_scope_ref_layout32 {uint32_t ref;uint64_t identity;};
_Static_assert(sizeof(struct dvm_gc_scope_ref_layout32)==16,"old scope entry size");
_Static_assert(sizeof(struct dvm_gc_scope_ref)==sizeof(struct dvm_gc_scope_ref_layout32),"scope entry must not grow");
_Static_assert(offsetof(struct dvm_gc_scope_ref,identity)==8,"scope identity offset unchanged");
struct dvm_gc_native_scope {
   struct dvm *vm;
   struct dvm_gc_native_scope *previous_thread,*next_vm;
   struct dvm_gc_scope_ref *refs,inline_refs[32];
   uint32_t count,capacity,guest_depth;
   bool active,failed;
};
_Static_assert(sizeof(struct dvm_gc_native_scope)==560,"native scope ABI remains 560 bytes on supported 64-bit hosts");
enum gen_result dvm_gc_scope_last_error(void);
bool dvm_gc_native_enter(struct dvm *,struct dvm_gc_native_scope *,uint32_t,const char *,const uint32_t *,int);
void dvm_gc_native_leave(struct dvm_gc_native_scope *);
void dvm_gc_native_leave_result(struct dvm_gc_native_scope *,const char *,uint32_t);
void dvm_gc_native_touch(struct dvm *,uint32_t,struct dvm_object *);
void dvm_gc_store_ref(struct dvm *,uint32_t *,uint32_t,const char *);
/* Production builds compile the pre-barrier out until mutation coverage and
 * the incremental coordinator have both passed their safety gates. */
#ifndef DVM_GC_WRITE_BARRIER
#define DVM_GC_WRITE_BARRIER 0
#endif
#if DVM_GC_WRITE_BARRIER
#define GC_STORE_REF(vm,slot,value,site) dvm_gc_store_ref((vm),(slot),(value),(site))
#else
#define GC_STORE_REF(vm,slot,value,site) (*(slot)=(value))
#endif
bool dvm_gc_has_native_scopes(struct dvm *);
bool dvm_gc_execution_root_counts(struct dvm *,uint64_t *,uint64_t *,uint64_t *,uint64_t *,uint64_t *,uint64_t *);
/* GC-1 establishes metadata. Moving and collection are enabled only after
 * handles/native borrows and root coverage have passed their own stages. */
enum { DVM_GC_FORWARDED=1u, DVM_GC_MARKED=2u, DVM_GC_OLD=4u,
       DVM_GC_DEFERRED_FREE=8u };
bool dvm_gc_layout_ensure(struct dvm *,struct dvm_class *);
bool dvm_gc_layout_metadata_selftest(void);
void dvm_gc_layout_destroy(struct dvm_class *);
void dvm_gc_layout_invalidate(struct dvm_class *);
bool dvm_gc_visit_declared_refs(struct dvm *,uint32_t,
                              void (*)(uint32_t *,void *),void *);
bool dvm_gc_visit_static_refs(struct dvm *,struct dvm_class *,
                            void (*)(uint32_t *,void *),void *);
bool dvm_gc_payload_requires_visitor(struct dvm_object *);
/* Match a concrete native ownership discriminator. NULL visitor classifies
 * without touching edges. UNKNOWN/INVALID require a conservative transitive quarantine, never owner-only retention. */
enum {DVM_GC_PAYLOAD_INVALID=-2,DVM_GC_PAYLOAD_UNKNOWN=-1,
      DVM_GC_PAYLOAD_ATOMIC=0,DVM_GC_PAYLOAD_REFS=1};
int dvm_gc_payload_visit(struct dvm *,struct dvm_object *,dvm_gc_visit_fn,void *);
bool dvm_gc_foundation_selftest(struct dvm *,const char *);
bool dvm_gc_observe(struct dvm *,const char *,const char *);
bool dvm_gc_microbenchmark(struct dvm *,const char *path,const char *stage);
/* Owner-only edges: do not call these as global strong root providers. */
bool thread_extras_gc_visit_owner(struct dvm *,uint32_t,dvm_gc_visit_fn,void *);
bool thread_runtime_gc_visit_owner(struct dvm *,uint32_t,dvm_gc_visit_fn,void *);
bool reflection_runtime_gc_visit_owner(struct dvm *,uint32_t,dvm_gc_visit_fn,void *);
bool weak_runtime_gc_visit_owner(struct dvm *,uint32_t,dvm_gc_visit_fn,void *);
bool throwable_reference_gc_visit_owner(struct dvm *,uint32_t,dvm_gc_visit_fn,void *);
bool window_attributes_gc_visit_owner(struct dvm *,uint32_t,dvm_gc_visit_fn,void *);
bool surface_view_runtime_gc_visit_owner(struct dvm *,uint32_t,dvm_gc_visit_fn,void *);
bool dvm_gc_ui_side_identity_selftest(struct dvm *);

uint32_t dvm_gc_ref_of(struct dvm *,struct dvm_object *);

/* Most recent completed/aborted real collection, not foundation test counters. */
void dvm_gc_fast_retention_counts(uint64_t *,uint64_t *,uint64_t *);
