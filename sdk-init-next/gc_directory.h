#pragma once
#include <stdint.h>
#ifndef DVM_ID_DIRECTORY_BLOCKS
#define DVM_ID_DIRECTORY_BLOCKS 1024u
#endif
#ifndef DVM_ID_DIRECTORY_DUAL
#define DVM_ID_DIRECTORY_DUAL 1
#endif
#ifndef DVM_REV_DIRECTORY_BLOCKS
#define DVM_REV_DIRECTORY_BLOCKS DVM_ID_DIRECTORY_BLOCKS
#endif
#define DVM_REV_DIRECTORY_BUCKETS 65536u
/* Query-only experiment. No relocation, generations or lifetime ownership. */
enum dvm_gc_directory_error { GC_DIR_OK, GC_DIR_INVALID_REF,
 GC_DIR_CAPACITY, GC_DIR_ALLOCATION };
struct dvm_gc_directory_stats {
 uint64_t queries,hits,misses,mismatches,overflows,metadata_bytes;
 uint64_t mismatch_ref,mismatch_legacy,mismatch_directory;
 uint64_t rev_queries,rev_hits,rev_misses,rev_mismatches,rev_overflows;
 uint64_t rev_bad_address,rev_bad_legacy,rev_bad_directory,rev_bad_reason;
};
void dvm_gc_directory_snapshot(struct dvm_gc_directory_stats *);
unsigned dvm_gc_directory_last_error(void);
