#define _GNU_SOURCE
#include "perf_attr.h"
#include "leak_scope.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include <unistd.h>
#define DIR "/data/storage/el2/base/haps/entry/files/sdkdex/"
#define LIMIT 120
#define METHODS 512
#define EVENTS 32768
static atomic_int level,guard=1,background,active,jitter_observe,jitter_quiet;
/* Opt-in: owner polling remains available; render/callback housekeeping
 * only reads this cached bit. Do not perform control IO to read the bit. */
static atomic_int defer_frame_controls;
/* Semantic allocation fixes; rollback controls are read by owner polling,
 * never by reflection/boxing. No per-call observation or filesystem IO. */
static atomic_int leak_metadata_names=1,leak_small_boxing=1;
static atomic_int leak_proxy_reclaim;
int perf_leak_proxy_reclaim(void){return atomic_load(&leak_proxy_reclaim);}
void perf_leak_proxy_reclaim_configure(int enabled){atomic_store(&leak_proxy_reclaim,enabled==1);}
int perf_leak_metadata_names(void){return atomic_load(&leak_metadata_names);}
int perf_leak_small_boxing(void){return atomic_load(&leak_small_boxing);}
int perf_frame_controls_deferred(void){return atomic_load(&defer_frame_controls);}
int perf_jitter_quiet(void){return atomic_load(&jitter_quiet);}
extern void egl_jitter_config(int,int);
extern void egl_jitter_selftest(void);
static atomic_uint_fast64_t last_frame,deferred,executed,frame_gc;
static atomic_int capture;
static int rows,overflow;
struct method {const char *cls,*name,*sig;uint64_t ns,calls;};
struct row {uint64_t start,total,vm,egl,trace,bytes,calls,gil_wait;int level,swaps;};
struct event {atomic_uint_fast64_t ticket,start,end;atomic_int kind;};
static struct event event_ring[EVENTS];
static atomic_uint_fast64_t event_seq;
static struct row samples[LIMIT];
static struct method methods[LIMIT][METHODS];
static _Thread_local struct {int in_frame,index,swaps;uint64_t start,vm,egl,trace,calls,bytes,gil_wait;} frame;
static _Thread_local struct perf_scope *top;
static _Thread_local struct perf_scope jni_scopes[128];
static _Thread_local unsigned jni_depth;
#include "long_frame_breakdown.inc"
static void play_frame_done(uint64_t,uint64_t);
static void play_controls(void);
uint64_t perf_now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000ull+t.tv_nsec;}
static uint64_t disk_bytes(void){
 const char *names[]={"engine-probe.txt","login-progress.txt","login-trace-roll.txt","unity-player-constructor.txt","gil-touch.txt"};
 uint64_t n=0;for(unsigned i=0;i<sizeof names/sizeof names[0];i++){char path[256];struct stat st;snprintf(path,sizeof path,DIR "%s",names[i]);if(!stat(path,&st))n+=st.st_size;}return n;
}
int perf_trace_level(void){return atomic_load(&level);}
int perf_frame_in_progress(void){return atomic_load(&active)>0;}
int perf_trace_keep(const char *s){
 if(!s)return 1;
 if(strstr(s,"FAIL")||strstr(s,"ERROR")||strstr(s,"error")||strstr(s,"abort")||strstr(s,"EXCEPTION")||strstr(s,"SELFTEST")||strstr(s,"BAD_")||strstr(s,"failed"))return 1;
 const char *hot[]={"JAVA_","GUEST_CALL","TOUCHQ","EGL_WINDOW","EGL_TRACE","NATIVE_CALL","NATIVE_RENDER_HOOK","[dvm]"," on @"};
 for(unsigned i=0;i<sizeof hot/sizeof hot[0];i++)if(!strncmp(s,hot[i],strlen(hot[i])))return perf_trace_level()>=2;
 return 1;
}
static void event_done(int kind,uint64_t start,uint64_t end){
 if(!atomic_load(&capture))return;
 uint64_t seq=atomic_fetch_add(&event_seq,1)+1;struct event *e=&event_ring[seq%EVENTS];
 atomic_store(&e->ticket,0);atomic_store(&e->start,start);atomic_store(&e->end,end);atomic_store(&e->kind,kind);atomic_store(&e->ticket,seq);
}
void perf_trace_done(uint64_t start){uint64_t end=perf_now();event_done(2,start,end);if(frame.in_frame){uint64_t elapsed=end-start;frame.trace+=elapsed;if(top)top->child+=elapsed;}}
void perf_egl_done(uint64_t start){uint64_t end=perf_now();perf_block_record(PERF_BLOCK_EGL,start,end);event_done(1,start,end);if(frame.in_frame){frame.egl+=end-start;frame.swaps++;}}
void perf_gil_wait_done(uint64_t start){if(frame.in_frame)frame.gil_wait+=perf_now()-start;}
struct perf_scope perf_vm_enter(const char *c,const char *m,const char *s){perf_ffrt_guest(c,m,s);int enabled=frame.in_frame&&(frame.index>=0||atomic_load(&jitter_observe));return (struct perf_scope){.parent=top,.start=enabled?perf_now():0,.cls=c,.method=m,.sig=s,.active=enabled};}
/* Caller installs its stack address only after the returned value is copied. */
void perf_vm_install(struct perf_scope *s){if(s->active)top=s;}
void perf_jni_enter(const char *name){unsigned i=jni_depth++;if(i>=128)return;jni_scopes[i]=perf_vm_enter("JNI",name,"");perf_vm_install(&jni_scopes[i]);}
void perf_jni_leave(void){if(!jni_depth)return;unsigned i=--jni_depth;if(i<128)perf_vm_leave(&jni_scopes[i]);}
void perf_vm_leave(struct perf_scope *s){
 if(!s->active)return;uint64_t elapsed=perf_now()-s->start;top=s->parent;
 if(top)top->child+=elapsed;
 uint64_t exclusive=elapsed>s->child?elapsed-s->child:0;
 if(!frame.in_frame)return;
 frame.vm+=exclusive;frame.calls++;
 if(frame.index<0)return;
 struct method *list=methods[frame.index];
 for(int i=0;i<METHODS;i++)if(!list[i].name||(list[i].name==s->method&&list[i].cls==s->cls&&list[i].sig==s->sig)){
  if(!list[i].name){list[i].name=s->method?s->method:"?";list[i].cls=s->cls?s->cls:"?";list[i].sig=s->sig?s->sig:"?";}list[i].calls++;list[i].ns+=exclusive;return;
 }overflow++;
}
struct perf_external perf_external_begin(void){struct perf_external e={top,perf_now()};top=NULL;return e;}
void perf_external_end(struct perf_external *e){top=e->parent;if(top)top->child+=perf_now()-e->start;}
static void flush(void){
 FILE *f=fopen(DIR "frame-profile.csv","w"),*m=fopen(DIR "frame-methods.csv","w");
 uint64_t seq=atomic_load(&event_seq);int event_overflow=0;
 if(seq>=EVENTS){struct event *e=&event_ring[(seq-EVENTS+1)%EVENTS];event_overflow=atomic_load(&e->start)>samples[0].start;}
 if(f){fputs("pid,frame,mono_ns,trace_level,frame_total_ms,unity_native_ms,vm_ms,egl_ms,trace_ms,vm_calls,swaps,log_file_growth_bytes,overlap_ms,egl_render_thread_ms,trace_render_thread_ms,vm_gil_wait_ms\n",f);
 for(int i=0;i<rows;i++){struct row *r=&samples[i];uint64_t own=r->vm+r->egl+r->trace,egl=0,trace=0;int swaps=0;
  uint64_t first=seq>=EVENTS?seq-EVENTS+1:1;
  for(uint64_t k=first;k<=seq;k++){struct event *e=&event_ring[k%EVENTS];if(atomic_load(&e->ticket)!=k)continue;uint64_t a=atomic_load(&e->start),b=atomic_load(&e->end);int kind=atomic_load(&e->kind);if(atomic_load(&e->ticket)!=k)continue;
   uint64_t lo=a>r->start?a:r->start,hi=b<r->start+r->total?b:r->start+r->total;if(hi<=lo)continue;if(kind==1){egl+=hi-lo;swaps++;}else trace+=hi-lo;
  }
  uint64_t native=r->total>own?r->total-own:0,sum=native+r->vm+egl+trace;
  fprintf(f,"%d,%d,%llu,%d,%.6f,%.6f,%.6f,%.6f,%.6f,%llu,%d,%llu,%.6f,%.6f,%.6f,%.6f\n",getpid(),i+1,(unsigned long long)r->start,r->level,r->total/1e6,native/1e6,r->vm/1e6,egl/1e6,trace/1e6,(unsigned long long)r->calls,swaps,(unsigned long long)r->bytes,(sum>r->total?sum-r->total:0)/1e6,r->egl/1e6,r->trace/1e6,r->gil_wait/1e6);
 }fclose(f);}
 if(m){fputs("frame,class,method,signature,calls,exclusive_ms\n",m);for(int i=0;i<rows;i++){
  /* Emit top ten exclusive methods for each frame, without allocating. */
  for(int rank=0;rank<10;rank++){int best=-1;for(int k=0;k<METHODS;k++)if(methods[i][k].calls&&(best<0||methods[i][k].ns>methods[i][best].ns))best=k;if(best<0)break;
   struct method *v=&methods[i][best];fprintf(m,"%d,%s,%s,%s,%llu,%.6f\n",i+1,v->cls,v->name,v->sig,(unsigned long long)v->calls,v->ns/1e6);v->calls=0;
  }
 }fclose(m);}
 FILE *s=fopen(DIR "frame-profile-status.txt","w");if(s){fprintf(s,"pid=%d rows=%d method_overflow=%d event_overflow=%d nativeRender_boundary=1 cross_thread_egl_trace=1 completed_events_only=1 moving_enabled=0\n",getpid(),rows,overflow,event_overflow);fclose(s);}
}
void perf_frame_begin(void){
 atomic_fetch_add(&active,1);atomic_store(&last_frame,perf_now());
 frame.in_frame=1;frame.start=perf_now();frame.vm=frame.egl=frame.trace=frame.calls=frame.gil_wait=0;frame.swaps=0;
 frame.index=capture&&rows<LIMIT?rows:-1;frame.bytes=frame.index>=0?disk_bytes():0;
}
void perf_window_enter(void){atomic_fetch_add(&active,1);atomic_store(&last_frame,perf_now());}
void perf_window_leave(void){atomic_store(&last_frame,perf_now());atomic_fetch_sub(&active,1);}
void perf_frame_end(void){
 uint64_t end=perf_now();frame.in_frame=0;atomic_store(&last_frame,end);atomic_fetch_sub(&active,1);
 perf_ffrt_frame(frame.start,end,frame.gil_wait);
 play_frame_done(frame.start,end);
 if(frame.index<0)return;
 uint64_t bytes=disk_bytes();samples[rows++]=(struct row){.start=frame.start,.total=end-frame.start,.vm=frame.vm,.egl=frame.egl,.trace=frame.trace,.bytes=bytes>=frame.bytes?bytes-frame.bytes:0,.calls=frame.calls,.gil_wait=frame.gil_wait,.level=perf_trace_level(),.swaps=frame.swaps};
 if(rows==LIMIT){flush();capture=0;}
}
static int read_flag(const char *name,int fallback){char path[256];snprintf(path,sizeof path,DIR "%s",name);FILE *f=fopen(path,"r");int n=fallback;if(f){if(fscanf(f,"%d",&n)!=1)n=fallback;fclose(f);}return n;}
#include "ffrt_stall.inc"
#include "play_stall.inc"
static void status(void){FILE *f=fopen(DIR "gc-frame-guard.csv","w");if(f){fprintf(f,"pid,guard,active,deferred_count,executed,in_frame_gc\n%d,%d,%d,%llu,%llu,%llu\n",getpid(),atomic_load(&guard),atomic_load(&active),(unsigned long long)atomic_load(&deferred),(unsigned long long)atomic_load(&executed),(unsigned long long)atomic_load(&frame_gc));fclose(f);}}
static void prepare_controls(void){
 static int prepared;if(prepared)return;prepared=1;
 perf_ffrt_start();
 egl_jitter_selftest();
 const char *jitter_names[]={"jitter-vsync-divisor","jitter-observe","jitter-quiet-probes"};
 for(unsigned i=0;i<3;i++){char path[256];snprintf(path,sizeof path,DIR "%s",jitter_names[i]);FILE *f=fopen(path,"wx");if(f){fputs("0\n",f);fclose(f);chmod(path,0666);}}
 const char *names[]={"trace.level","gc_frame_guard","perf-capture","play-stall-profile","play-touch-front","play-touch-preserve","play-touch-timestamps","perf-defer-frame-controls","play-stall-duration-ms"};const char *defaults[]={"0\n","1\n","0\n","0\n","0\n","0\n","0\n","0\n","120000\n"};
 for(unsigned i=0;i<sizeof names/sizeof names[0];i++){char path[256];snprintf(path,sizeof path,DIR "%s",names[i]);FILE *f=fopen(path,"wx");if(f){fputs(defaults[i],f);fclose(f);chmod(path,0666);}}
 /* Operator value 2 is an explicit next-launch recovery request, not enable.
  * Archive the prior dirty marker in-process; never silently clear evidence. */
 if(read_flag("gc-collection-enabled",0)==2){
  int present=access(DIR "gc-collection-dirty",F_OK)==0;
  int archived=!present||rename(DIR "gc-collection-dirty",DIR "perfattr-prior-gc-dirty")==0;
  FILE *f=fopen(DIR "PERF-ATTR-recovery.txt","w");if(f){fprintf(f,"pid=%d explicit_operator_request=2 prior_dirty=%d archived=%d collection_enabled=0\n",getpid(),present,archived);fclose(f);}
  f=fopen(DIR "gc-collection-enabled","w");if(f){fputs("0\n",f);fclose(f);}
 }
}
void perf_config_poll(void){
 prepare_controls();
 static uint64_t last;uint64_t now=perf_now();if(now-last<100000000ull)return;last=now;
 atomic_store(&level,read_flag("trace.level",0));atomic_store(&guard,read_flag("gc_frame_guard",1)==1);
 play_controls();
 atomic_store(&jitter_observe,read_flag("jitter-observe",0)==1);
 atomic_store(&jitter_quiet,read_flag("jitter-quiet-probes",0)==1);
 atomic_store(&defer_frame_controls,read_flag("perf-defer-frame-controls",0)==1);
 atomic_store(&leak_metadata_names,read_flag("leak-metadata-names",1)==1);
 atomic_store(&leak_small_boxing,read_flag("leak-small-boxing",1)==1);
 egl_jitter_config(read_flag("jitter-vsync-divisor",0),atomic_load(&background));
 /* Arm the NEXT render even when the current unsampled render is active.
  * capture=0 means no sampled frame can still be writing methods (flush
  * publishes zero only after completion); frame begins/config hold the GIL. */
 if(read_flag("perf-capture",0)==1&&!capture){rows=0;overflow=0;memset(methods,0,sizeof methods);capture=1;FILE *f=fopen(DIR "perf-capture","w");if(f){fputs("0\n",f);fclose(f);}}
 static unsigned count;if(++count%10==0)status();
 /* Fixed atomic ring only; the capture observer exports after recording. */
 perf_result_event(20,now,perf_now());
}
void perf_background(int value){atomic_store(&background,value);if(value)egl_jitter_config(0,1);}
static int allowed(int enabled,int frames,int bg,int manual,uint64_t now,uint64_t last){return !enabled||(!frames&&(manual||bg||now-last>200000000ull));}
int perf_gc_allowed(int manual){int ok=allowed(atomic_load(&guard),atomic_load(&active),atomic_load(&background),manual,perf_now(),atomic_load(&last_frame));if(!ok)atomic_fetch_add(&deferred,1);return ok;}
void perf_gc_executed(int completed){if(completed){atomic_fetch_add(&executed,1);if(atomic_load(&active))atomic_fetch_add(&frame_gc,1);}}
int perf_guard_selftest(void){
 int ok=allowed(1,1,0,0,300000000,0)==0&&allowed(1,1,1,1,300000000,0)==0&&allowed(1,0,0,0,100000000,0)==0&&allowed(1,0,0,0,300000000,0)==1&&allowed(1,0,0,1,100000000,0)==1&&allowed(1,0,1,0,100000000,0)==1&&allowed(0,1,0,0,100000000,0)==1;
 int saved=atomic_exchange(&level,0);
 ok=ok&&!perf_trace_keep("JAVA_API x")&&perf_trace_keep("SELFTEST PASS")&&perf_trace_keep("EGL_WINDOW failed ERROR")&&perf_trace_keep("abort");atomic_store(&level,saved);
 FILE *f=fopen(DIR "PERF-ATTR-selftest.txt","w");if(!f)return 0;fprintf(f,"PERF_ATTR_SELFTEST %s pid=%d policy_cases=7 trace_cases=4 HAP_process=1 moving_enabled=0\n",ok?"PASS":"FAIL",getpid());fclose(f);return ok;
}
