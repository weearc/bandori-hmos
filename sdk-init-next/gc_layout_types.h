#pragma once
#include <stddef.h>
#include <stdint.h>
struct dvm_gc_level_snapshot;
/* Shared GC1 precise slot layout; primitive slots are never traversed. */
struct dvm_gc_layout {
   uint64_t stw_validated_epoch;
   unsigned levels;
   struct dvm_gc_level_snapshot *chain;
   uint32_t *instance_slots, *static_slots;
   size_t instance_count, static_count;
};
