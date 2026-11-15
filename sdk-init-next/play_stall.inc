/* Opt-in bounded recording. No file IO on render/touch paths. The observer
 * samples native counters only and never walks a running VM's mutable stack. */
#include <pthread.h>
#include <sys/syscall.h>
#define RESULT_EVENTS 524288
static atomic_uint_fast64_t result_sequence,result_capture_start;
static struct {atomic_uint_fast64_t ticket,ns,a,b,tid;atomic_int kind;} result_events[RESULT_EVENTS];
void perf_result_event(int kind,uint64_t a,uint64_t b){
 if(!perf_play_enabled())return;
 uint64_t n=atomic_fetch_add(&result_sequence,1)+1;
 unsigned at=(unsigned)((n-1)%RESULT_EVENTS);
 atomic_store(&result_events[at].ticket,0);
 atomic_store(&result_events[at].ns,perf_now());atomic_store(&result_events[at].a,a);atomic_store(&result_events[at].b,b);
 atomic_store(&result_events[at].tid,(uint64_t)syscall(SYS_gettid));atomic_store(&result_events[at].kind,kind);
 atomic_store_explicit(&result_events[at].ticket,n,memory_order_release);
}
static void result_events_write(void){
 uint64_t end=atomic_load(&result_sequence),first=atomic_load(&result_capture_start)+1;
 if(end>=RESULT_EVENTS&&first<end-RESULT_EVENTS+1)first=end-RESULT_EVENTS+1;
 FILE *f=fopen(DIR "result-black-events.csv","w");if(!f)return;
 fputs("pid,sequence,mono_ns,tid,kind,a,b\n",f);
 for(uint64_t n=first;n<=end;n++){unsigned at=(unsigned)((n-1)%RESULT_EVENTS);
  if(atomic_load_explicit(&result_events[at].ticket,memory_order_acquire)!=n)continue;
  uint64_t ns=atomic_load(&result_events[at].ns),a=atomic_load(&result_events[at].a),b=atomic_load(&result_events[at].b),tid=atomic_load(&result_events[at].tid);int kind=atomic_load(&result_events[at].kind);
  if(atomic_load_explicit(&result_events[at].ticket,memory_order_acquire)==n)fprintf(f,"%d,%llu,%llu,%llu,%d,%llu,%llu\n",getpid(),(unsigned long long)n,(unsigned long long)ns,(unsigned long long)tid,kind,(unsigned long long)a,(unsigned long long)b);
 }fclose(f);
}
#define PLAY_FRAMES 131072
#define PLAY_SAMPLES 98304
#define PLAY_DURATION_MAX_MS 660000
static atomic_int play_enabled,play_thread_started;
static atomic_uint play_frame_count,play_gc_active;
static atomic_uint_fast64_t play_start,play_gc_count,play_gc_ns;
struct play_frame {atomic_uint ready;uint64_t start,end,previous_end,vm,egl,trace,gil_wait,tid;int breakdown;};
static struct play_frame play_frames[PLAY_FRAMES];
extern void dvm_gil_play_status(char *,size_t) __attribute__((weak));
extern void sdk_input_play_snapshot(uint64_t *) __attribute__((weak));
int perf_play_enabled(void){return atomic_load_explicit(&play_enabled,memory_order_relaxed)==1;}
uint64_t perf_play_session(void){return perf_play_enabled()?atomic_load(&play_start):0;}
void perf_play_gc(int begin){
 static _Thread_local struct perf_block_scope gc_scope;
 if(begin)gc_scope=perf_block_begin(PERF_BLOCK_GC);else perf_block_finish(&gc_scope);
 if(begin){atomic_store(&play_gc_active,1);atomic_fetch_add(&play_gc_count,1);atomic_store(&play_gc_ns,perf_now());}
 else atomic_store(&play_gc_active,0);
}
static void play_frame_done(uint64_t start,uint64_t end){
 static _Thread_local uint64_t previous;
 uint64_t prior=previous;previous=end;
 if(!perf_play_enabled()||start<atomic_load(&play_start))return;
 unsigned n=atomic_fetch_add(&play_frame_count,1);if(n>=PLAY_FRAMES)return;
 struct play_frame *r=&play_frames[n];r->start=start;r->end=end;r->previous_end=prior;
 r->vm=frame.vm;r->egl=frame.egl;r->trace=frame.trace;r->gil_wait=frame.gil_wait;r->tid=long_thread();r->breakdown=atomic_load(&jitter_observe);
 atomic_store_explicit(&r->ready,1,memory_order_release);
}
static void *play_observer(void *unused){
 (void)unused;
 static struct {uint64_t time,touch[8],gc_count,gc_start;unsigned gc;char gil[768];} samples[PLAY_SAMPLES];
 for(;;){
  if(!perf_play_enabled()){struct timespec pause={0,20000000};nanosleep(&pause,NULL);continue;}
  unsigned count=0;uint64_t begin=atomic_load(&play_start);
  int duration_ms=read_flag("play-stall-duration-ms",120000);
  if(duration_ms<1000||duration_ms>PLAY_DURATION_MAX_MS)duration_ms=120000;
  while(perf_play_enabled()&&count<PLAY_SAMPLES&&perf_now()-begin<(uint64_t)duration_ms*1000000ull){
   samples[count].time=perf_now();memset(samples[count].touch,0,sizeof samples[count].touch);
   if(sdk_input_play_snapshot)sdk_input_play_snapshot(samples[count].touch);
   samples[count].gc=atomic_load(&play_gc_active);samples[count].gc_count=atomic_load(&play_gc_count);samples[count].gc_start=atomic_load(&play_gc_ns);
   if(dvm_gil_play_status)dvm_gil_play_status(samples[count].gil,sizeof samples[count].gil);
   else snprintf(samples[count].gil,sizeof samples[count].gil,"not_measured");
   count++;struct timespec delay={0,10000000};nanosleep(&delay,NULL);
  }
  atomic_store(&play_enabled,2);
  result_events_write();
  int export_ok=1;
  FILE *f=fopen(DIR "play-stall-frames.csv","w");
  if(f){fputs("pid,start_ns,end_ns,previous_end_ns,frame_ms,interval_ms,stall_gt200ms,unity_native_ms,vm_ms,egl_same_thread_ms,trace_ms,gil_wait_ms,breakdown_measured\n",f);
   unsigned n=atomic_load(&play_frame_count);for(unsigned i=0;i<n&&i<PLAY_FRAMES;i++){
    struct play_frame *r=&play_frames[i];if(!atomic_load_explicit(&r->ready,memory_order_acquire))continue;
    uint64_t gap=r->previous_end&&r->previous_end>=begin?r->end-r->previous_end:0;
    uint64_t own=r->vm+r->egl+r->trace,total=r->end-r->start;
    fprintf(f,"%d,%llu,%llu,%llu,%.6f,%.6f,%d,%.6f,%.6f,%.6f,%.6f,%.6f,%d\n",getpid(),(unsigned long long)r->start,(unsigned long long)r->end,(unsigned long long)r->previous_end,total/1e6,gap/1e6,total>200000000||gap>200000000,(total>own?total-own:0)/1e6,r->vm/1e6,r->egl/1e6,r->trace/1e6,r->gil_wait/1e6,r->breakdown);
   }if(ferror(f))export_ok=0;if(fclose(f))export_ok=0;
  }else export_ok=0;
  /* Only completed instrumented spans. Cross-thread sums are correlation,
   * not proof that another thread blocked this render. Unknown stays visible. */
  f=fopen(DIR "long-frame-breakdown.csv","w");if(f){
   fputs("pid,start_ns,end_ns,tid,frame_ms,gil_wait_ms,gc_overlap_ms,egl_same_ms,egl_other_thread_sum_ms,diagnostic_same_ms,diagnostic_other_thread_sum_ms,file_io_same_ms,file_io_other_thread_sum_ms,trace_same_ms,unassigned_lower_bound_ms,intervals_incomplete,inflight_at_export\n",f);
   uint64_t last=atomic_load(&long_sequence);unsigned n=atomic_load(&play_frame_count);
   for(unsigned i=0;i<n&&i<PLAY_FRAMES;i++){
    struct play_frame *r=&play_frames[i];if(!atomic_load_explicit(&r->ready,memory_order_acquire)||r->end-r->start<=33000000)continue;
    struct long_totals t=long_sum(r->start,r->end,r->tid,last);
    uint64_t total=r->end-r->start,gc=t.same[PERF_BLOCK_GC]+t.other[PERF_BLOCK_GC];
    uint64_t upper=r->gil_wait+gc+t.same[PERF_BLOCK_EGL]+t.same[PERF_BLOCK_DIAG]+t.same[PERF_BLOCK_FILE]+r->trace;
    fprintf(f,"%d,%llu,%llu,%llu,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%d,%u\n",getpid(),(unsigned long long)r->start,(unsigned long long)r->end,(unsigned long long)r->tid,total/1e6,r->gil_wait/1e6,gc/1e6,t.same[PERF_BLOCK_EGL]/1e6,t.other[PERF_BLOCK_EGL]/1e6,t.same[PERF_BLOCK_DIAG]/1e6,t.other[PERF_BLOCK_DIAG]/1e6,t.same[PERF_BLOCK_FILE]/1e6,t.other[PERF_BLOCK_FILE]/1e6,r->trace/1e6,(total>upper?total-upper:0)/1e6,t.lost!=0||atomic_load(&long_dropped)!=0||atomic_load(&long_inflight)!=0,atomic_load(&long_inflight));
   }if(ferror(f))export_ok=0;if(fclose(f))export_ok=0;
  }else export_ok=0;
  f=fopen(DIR "play-stall-samples.csv","w");if(f){
   fputs("pid,mono_ns,queued,injected,failed,dropped,depth,drain_calls,drained,unpaired,gc_active,gc_count,gc_begin_ns,gil\n",f);
   for(unsigned i=0;i<count;i++)fprintf(f,"%d,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%u,%llu,%llu,%s\n",getpid(),(unsigned long long)samples[i].time,(unsigned long long)samples[i].touch[0],(unsigned long long)samples[i].touch[1],(unsigned long long)samples[i].touch[2],(unsigned long long)samples[i].touch[3],(unsigned long long)samples[i].touch[4],(unsigned long long)samples[i].touch[5],(unsigned long long)samples[i].touch[6],(unsigned long long)samples[i].touch[7],samples[i].gc,(unsigned long long)samples[i].gc_count,(unsigned long long)samples[i].gc_start,samples[i].gil);
   if(ferror(f))export_ok=0;if(fclose(f))export_ok=0;
  }else export_ok=0;
  f=fopen(DIR "play-stall-status.txt","w");if(f){fprintf(f,"pid=%d samples=%u frame_count=%u frame_capacity=%d observed_ms=%.3f max_duration_ms=%d export_ok=%d frame_overflow=%d event_overflow=%d moving_enabled=0 IO_paths=control_poll_and_diag_worker_only method_requires_gil_deep_trace=1\n",getpid(),count,atomic_load(&play_frame_count),PLAY_FRAMES,(perf_now()-begin)/1e6,duration_ms,export_ok,atomic_load(&play_frame_count)>PLAY_FRAMES,atomic_load(&result_sequence)-atomic_load(&result_capture_start)>RESULT_EVENTS);fclose(f);}
  f=fopen(DIR "play-stall-profile","w");if(f){fputs("0\n",f);fclose(f);}
  atomic_store(&play_enabled,0);
 }
 return NULL;
}
static void play_controls(void){
 if(read_flag("play-stall-profile",0)==1&&!atomic_load(&play_enabled)){
  atomic_store(&long_capture_first,atomic_load(&long_sequence));atomic_store(&long_dropped,0);
  atomic_store(&result_capture_start,atomic_load(&result_sequence));
  atomic_store(&play_frame_count,0);memset(play_frames,0,sizeof play_frames);atomic_store(&play_start,perf_now());
  if(!atomic_exchange(&play_thread_started,1)){pthread_t t;if(pthread_create(&t,NULL,play_observer,NULL)){atomic_store(&play_thread_started,0);return;}pthread_detach(t);}
  atomic_store(&play_enabled,1);
 }else if(read_flag("play-stall-profile",0)!=1&&perf_play_enabled())atomic_store(&play_enabled,2);
}
