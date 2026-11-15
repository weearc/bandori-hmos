#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include "gc_layout_types.h"
typedef struct gcs_heap gcs_heap;
typedef struct gcs_object gcs_object;
enum gcs_status { GCS_OK=0, GCS_OOM=-1, GCS_SATB_OVERFLOW=-2,
                  GCS_INVALID=-3, GCS_THREAD_ERROR=-4, GCS_TIMEOUT=-5 };
/* Isolated, nonmoving, mark-only heap. Immutable layouts outlive objects. All roots are
 * captured at begin; no external/native roots or weak/finalizable objects.
 * No reclamation. Never pass real VM objects or slots to these APIs. */
gcs_heap *gcs_create(size_t bytes,size_t object_capacity,size_t satb_capacity);
void gcs_destroy(gcs_heap *);
gcs_object *gcs_alloc(gcs_heap *,const struct dvm_gc_layout *,size_t slots);
int gcs_put_ref(gcs_heap *,gcs_object *,size_t,gcs_object *);
int gcs_put_scalar(gcs_heap *,gcs_object *,size_t,uintptr_t);
gcs_object *gcs_get_ref(gcs_heap *,gcs_object *,size_t);
/* hold_worker is a deterministic test scheduling gate, not a fake marker:
 * a real pthread starts and acknowledges the gate before begin returns. */
int gcs_begin(gcs_heap *,gcs_object *const *roots,size_t count,bool hold_worker);
int gcs_offer_root(gcs_heap *,gcs_object *);
int gcs_finish(gcs_heap *);
bool gcs_marked(gcs_heap *,gcs_object *);
int gc_isolated_satb_selftest(FILE *,FILE *);
/* GC5: page-backed isolated mode. Unborrowed raw pointers expire at collection;
 * roots are rewritten. Borrow tokens retain forwarding chains until release.
 * Evacuation is single-mutator/quiescent; concurrent coordination is GC10. */
struct gcs_evac_stats {
 size_t logical_reclaimed_bytes,physical_reclaimed_bytes,retained_forwarding_bytes;
 size_t logical_dead_bytes,committed_bytes,reserved_bytes,reuse_hits,copied_objects;
 size_t decommit_calls;
};
gcs_heap *gcs_create_evacuating(size_t bytes,size_t object_capacity);
uint64_t gcs_borrow(gcs_heap *,gcs_object *);
int gcs_release(gcs_heap *,uint64_t);
gcs_object *gcs_resolve(gcs_heap *,gcs_object *);
int gcs_evacuate(gcs_heap *,gcs_object **roots,size_t count);
void gcs_evac_stats(gcs_heap *,struct gcs_evac_stats *);
int gc_isolated_evacuate_selftest(FILE *,FILE *);
