#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
/* Process-wide single-VM counters. No allocation or IO in collector hooks.
 * The existing arena observer is the only filesystem consumer. */
enum dvm_gc_reason { DVM_GC_PRESSURE, DVM_GC_EXPLICIT, DVM_GC_IDLE, DVM_GC_REASONS };
struct dvm_gc_metrics {
 uint64_t attempts[DVM_GC_REASONS], completed, aborted, reclaimed_objects,
          reclaimed_bytes, last_live, pause_samples, pause_max_ns, dropped;
 uint64_t pause_bins[129]; /* 250us upper bounds; final bin is censored. */
 unsigned last_reason;
 bool last_live_valid;
};
void dvm_gc_metrics_reset(void);
void dvm_gc_metrics_attempt(const char *);
void dvm_gc_metrics_finish(const char *,bool,size_t,size_t,size_t,uint64_t);
bool dvm_gc_metrics_snapshot(struct dvm_gc_metrics *);
/* Called only by the existing heap observer, regardless of heap-observe. */
void dvm_gc_metrics_worker(void);
