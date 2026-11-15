#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#define VM_ARENA_LIMIT (128u*1024u*1024u)
#define VM_ARENA_CONFIG_MAX (256u*1024u*1024u)
size_t vm_arena_configured_limit(void);
#define VM_ARENA_REGION_BYTES (1024u*1024u)
#define VM_EXTERNAL_THRESHOLD (8u*1024u*1024u)
#define VM_EXTERNAL_LIMIT (256u*1024u*1024u)
struct vm_external_snapshot {size_t bytes,peak,limit,physical_reclaimed_bytes,denied;};
void *vm_external_calloc(size_t bytes);
void vm_external_snapshot(struct vm_external_snapshot *);
/* Collector-only bounded temporary workspace, never a guest allocation. */
void *vm_gc_scratch_alloc(size_t);
void vm_gc_scratch_free(void *);
/* used/peak retain physical backing accounting. Live allocation bytes and
 * reusable capacity are separate; all region descriptors/bitmaps are charged. */
struct vm_arena_region_snapshot {
 bool active;
 size_t limit_bytes,committed_bytes,peak_committed_bytes;
 size_t live_bytes,live_requested_bytes,peak_live_bytes;
 size_t free_bytes,metadata_bytes,waste_bytes,bitmap_bytes;
 size_t regions,humongous_regions,live_allocations,reuse_hits,denied;
};
bool vm_arena_region_snapshot(struct vm_arena_region_snapshot *out);
/* Query current region data capacity, never dereferences the supplied pointer.
 * The bitmap describes allocator payload starts, not interior guest headers. */
bool vm_arena_locate(const void *pointer,size_t *region_id,bool *allocation_start);
/* Separate from the allocator bitmap: these bits describe explicitly
 * registered, aligned guest headers, including headers interior to a block. */
bool vm_arena_object_start_set(const void *pointer,bool present);
bool vm_arena_object_start(const void *pointer);
bool vm_arena_begin(size_t limit);
void vm_arena_end(void);
size_t vm_arena_limit(void);
size_t vm_arena_used(void);
size_t vm_arena_peak(void);
size_t vm_arena_denied(void);
/* Compatibility with task8a-r9 diagnostics. Hist live_charge is allocated
 * cell charge, while diagnostic.used is committed backing including metadata. */
#define VM_ARENA_HISTOGRAM_BINS 8
struct vm_arena_histogram_bin {
 size_t allocations,frees,live_blocks,live_requested,live_charge,peak_charge,denied;
};
struct vm_arena_diagnostic {
 size_t limit,ordinary_limit,used,peak,denied,header_size;
 size_t last_requested,last_charge,last_used,last_threshold,last_limit;
 unsigned last_emergency,last_reason;
 uintptr_t last_caller;
 struct vm_arena_histogram_bin bins[VM_ARENA_HISTOGRAM_BINS];
};
void vm_arena_diagnostic_snapshot(struct vm_arena_diagnostic *out);
void vm_arena_diagnostic_write(int fd,const char *phase);
void vm_arena_emergency_enter(void);
void vm_arena_emergency_leave(void);
void *vm_arena_malloc(size_t);
void *vm_arena_calloc(size_t,size_t);
void *vm_arena_realloc(void *,size_t);
void vm_arena_free(void *);
char *vm_arena_strdup(const char *);
char *vm_arena_strndup(const char *,size_t);
/* Opt-in destructive diagnostics only. Private sdkdex/gc.selftest.pressure
 * must contain exactly 1 (optional line ending); absent/invalid means OFF.
 * A gated selftest returns true when skipped, but logs SKIP, never PASS. */
bool vm_pressure_selftest_enabled(void);
bool vm_pressure_selftest_gate(const char *name);
bool vm_arena_selftest(void);
struct dvm;
bool vm_arena_java_selftest(struct dvm *);
