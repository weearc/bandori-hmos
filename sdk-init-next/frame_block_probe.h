#ifndef FRAME_BLOCK_PROBE_H
#define FRAME_BLOCK_PROBE_H
#include "perf_attr.h"
extern struct perf_block_scope perf_block_begin(unsigned) __attribute__((weak));
extern void perf_block_finish(struct perf_block_scope *) __attribute__((weak));
static inline struct perf_block_scope frame_probe_begin(unsigned kind){return perf_block_begin?perf_block_begin(kind):(struct perf_block_scope){0,0};}
static inline void frame_probe_finish(struct perf_block_scope *s){if(perf_block_finish)perf_block_finish(s);}
#define FRAME_PROBE_SCOPE(kind) struct perf_block_scope diagnostic_probe \
 __attribute__((cleanup(frame_probe_finish)))=frame_probe_begin(kind)
#define FRAME_DIAG_SCOPE FRAME_PROBE_SCOPE(PERF_BLOCK_DIAG)
#endif
