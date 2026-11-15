#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#define VM_ARENA_LIMIT (128u*1024u*1024u)
bool vm_arena_begin(size_t limit);
void vm_arena_end(void);
size_t vm_arena_limit(void);
size_t vm_arena_used(void);
size_t vm_arena_peak(void);
size_t vm_arena_denied(void);
/* Fixed-size diagnostics. No allocation or file creation; caller owns fd. */
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
bool vm_arena_selftest(void);
struct dvm;
bool vm_arena_java_selftest(struct dvm *);
