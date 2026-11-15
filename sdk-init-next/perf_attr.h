#ifndef PERF_ATTR_H
#define PERF_ATTR_H
#include <stdint.h>
#include <stddef.h>
uint64_t perf_now(void);
int perf_trace_level(void);
int perf_play_enabled(void);
uint64_t perf_play_session(void);
int perf_jitter_quiet(void);
int perf_frame_controls_deferred(void);
int perf_ffrt_mode(void);
int perf_ffrt_snapshots_enabled(void);
void perf_ffrt_start(void);
void perf_ffrt_frame(uint64_t,uint64_t,uint64_t);
void perf_ffrt_gil_acquired(void);
void perf_ffrt_gil_released(uint64_t);
void perf_ffrt_guest(const char *,const char *,const char *);
uint64_t perf_ffrt_native_begin(int);
void perf_ffrt_native_end(int,uint64_t);
void perf_ffrt_io(unsigned);
int perf_leak_metadata_names(void);
int perf_leak_small_boxing(void);
int perf_leak_proxy_reclaim(void);
void perf_leak_proxy_reclaim_configure(int);
/* Read-only buffered observation: 1/2 swap, 3/4 nativeRender, 5 vsync,
 * 6/7 doFrame dispatch; 8 independent clock; 9/10 pacing wait;
 * 12 host callback(start in a); 13/14 diagnostic setup. Numeric IDs only. */
void perf_result_event(int, uint64_t, uint64_t);
void perf_play_gc(int);
int perf_frame_in_progress(void);
int perf_trace_keep(const char *);
void perf_trace_done(uint64_t);
void perf_frame_begin(void);
void perf_frame_end(void);
void perf_window_enter(void);
void perf_window_leave(void);
void perf_egl_done(uint64_t);
void perf_gil_wait_done(uint64_t);
/* Capture-only, bounded interval probes. Categories may overlap: these are
 * measured waits/work, not an additive partition of a frame. */
enum perf_block_kind { PERF_BLOCK_EGL=1, PERF_BLOCK_DIAG=2, PERF_BLOCK_FILE=3,
 PERF_BLOCK_GC=4, PERF_BLOCK_COUNT=5 };
struct perf_block_scope {uint64_t start;unsigned kind;};
struct perf_block_scope perf_block_begin(unsigned);
void perf_block_finish(struct perf_block_scope *);
void perf_block_record(unsigned,uint64_t,uint64_t);
#define PERF_BLOCK_SCOPE(kind) struct perf_block_scope perf_block_local \
 __attribute__((cleanup(perf_block_finish)))=perf_block_begin(kind)
void perf_config_poll(void);
void perf_background(int);
int perf_gc_allowed(int);
void perf_gc_executed(int);
int perf_guard_selftest(void);
struct perf_scope {struct perf_scope *parent;uint64_t start,child;const char *cls,*method,*sig;int active;};
struct perf_external {struct perf_scope *parent;uint64_t start;};
struct perf_scope perf_vm_enter(const char *,const char *,const char *);
void perf_vm_install(struct perf_scope *);
void perf_jni_enter(const char *);
void perf_jni_leave(void);
void perf_vm_leave(struct perf_scope *);
struct perf_external perf_external_begin(void);
void perf_external_end(struct perf_external *);
#endif
