#ifndef PERF_WAIT_CONTEXT_H
#define PERF_WAIT_CONTEXT_H
#include <stdint.h>
/* Native-only checkpoint; no allocation, file IO, guest call or stack walk. */
struct perf_wait_context {int tid,kind;uintptr_t pc,fp,sp,condition,mutex;uint64_t start_ns,generation;};
void perf_wait_enter(int kind,void *condition,void *mutex,void *pc,void *fp);
void perf_wait_leave(void);
int perf_wait_get(int tid,struct perf_wait_context *out);
#endif
