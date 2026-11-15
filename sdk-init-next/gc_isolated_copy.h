#ifndef GC_ISOLATED_COPY_H
#define GC_ISOLATED_COPY_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* Standalone strong-reference arena. Never pass a real VM object here.
 * No weak references, finalizers, interior pointers or concurrent mutation.
 * All APIs are single-threaded. References older than TWO completed copies
 * expire; resolve them before the next collection. Roots and their array
 * remain writable/live until collection finishes. No allocation/mutation
 * during a copy. An OOM/error poisons this arena until destruction. */
typedef struct gci_heap gci_heap;
typedef struct gci_object gci_object;
enum gci_status { GCI_IDLE=0, GCI_MORE=1, GCI_DONE=2,
                 GCI_OOM=-1, GCI_INVALID=-2, GCI_BUSY=-3 };
struct gci_stats {
 size_t allocated_objects,allocated_bytes,live_objects,live_bytes;
 size_t reclaimed_objects,reclaimed_bytes,copied_objects,copied_bytes;
 size_t steps,last_slice_work,max_slice_work;
 /* reclaimed_bytes is logical dead-object size, not physical free. */
 size_t retained_forwarding_bytes,physical_space_bytes;
 unsigned collections;
};
gci_heap *gci_create(size_t capacity,size_t region_size);
void gci_destroy(gci_heap *);
gci_object *gci_alloc(gci_heap *,size_t references,size_t payload_bytes);
gci_object *gci_resolve(gci_heap *,gci_object *);
bool gci_set_ref(gci_heap *,gci_object *,size_t,gci_object *);
gci_object *gci_get_ref(gci_heap *,gci_object *,size_t);
void *gci_payload(gci_heap *,gci_object *,size_t *);
size_t gci_region(gci_heap *,gci_object *);
bool gci_is_large(gci_heap *,gci_object *);
int gci_begin(gci_heap *,gci_object **roots,size_t count);
/* Budget counts root slots + object headers + reference slots + 256-byte
 * copy chunks. Each invocation consumes <= budget units. budget must be >0.
 * This is deterministic cooperative work, not a wall-clock guarantee. */
int gci_slice(gci_heap *,size_t budget);
int gci_status(const gci_heap *);
void gci_get_stats(const gci_heap *,struct gci_stats *);
bool gci_selftest(FILE *evidence);
/* Device runner entry: 0=pass. csv contains measured per-slice wall time and
 * actual copy/reclaim counters, not reformatted assertions. */
int gc_isolated_selftest(FILE *evidence,FILE *csv);
#endif
