#define _POSIX_C_SOURCE 200809L
#include "gc_isolated_verify.h"
#include "gc_isolated_satb.h"
#include <EGL/egl.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define DIR "/data/storage/el2/base/haps/entry/files/sdkdex"
#define FRAME_CAP 65536u
struct sample {uint64_t stamp,interval,swap;unsigned phase;int success;atomic_bool ready;};
static struct sample frames[FRAME_CAP];
static atomic_uint frame_count,phase;
static atomic_ullong last_stamp,last_thread;
static uint64_t ns(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000u+(uint64_t)t.tv_nsec;}
extern EGLBoolean __real_eglSwapBuffers(EGLDisplay,EGLSurface);
EGLBoolean __wrap_eglSwapBuffers(EGLDisplay d,EGLSurface s){
 uint64_t begin=ns();EGLBoolean ok=__real_eglSwapBuffers(d,s);uint64_t end=ns();
 unsigned p=atomic_load_explicit(&phase,memory_order_acquire);
 if(p==1||p==2){
  unsigned i=atomic_fetch_add(&frame_count,1);
  uint64_t thread=(uint64_t)(uintptr_t)pthread_self();
  uint64_t previous_thread=atomic_exchange(&last_thread,thread),previous=atomic_exchange(&last_stamp,end);
  if(i<FRAME_CAP){frames[i].stamp=end;frames[i].interval=previous&&previous_thread==thread?end-previous:0;
   frames[i].swap=end-begin;frames[i].phase=p;frames[i].success=ok==EGL_TRUE;
   atomic_store_explicit(&frames[i].ready,true,memory_order_release);}
 }
 return ok;
}
static void wait_seconds(unsigned sec){struct timespec t={sec,0};while(nanosleep(&t,&t)&&errno==EINTR){} }
static void *observe(void *unused){
 (void)unused;FILE *log=fopen(DIR "/gc6-observer.txt","w");if(!log)return NULL;
 fprintf(log,"pid=%d start_mono_ns=%llu real_vm_collection_enabled=0 real_vm_moving_enabled=0\n",getpid(),(unsigned long long)ns());fflush(log);
 unsigned waited=0;
 while(access(DIR "/gc6-start-verify",F_OK)&&waited<360){wait_seconds(1);waited++;}
 if(waited==360){fputs("not_run: no explicit authorization marker\n",log);fclose(log);return NULL;}
 /* The orchestrator may write this only for an explicitly authorized task.
  * Empty/title-only markers never start a run; no timeout starts one either. */
 unsigned seconds=0;char task[32]={0};FILE *authorization=fopen(DIR "/gc6-start-verify","r");
 int fields=authorization?fscanf(authorization,"%31s %u",task,&seconds):0;if(authorization)fclose(authorization);
 if(fields!=2||!seconds||seconds>86400||(strcmp(task,"E2-STAB")&&strcmp(task,"FINAL"))){
  fputs("not_run: explicit E2-STAB/FINAL task and duration required\n",log);fclose(log);return NULL;}
 bool title=true;
 atomic_store(&last_stamp,0);atomic_store(&phase,1);
 fprintf(log,"phase=baseline start_ns=%llu title_confirmed_marker=%d interval_metric=EGL_successful_submission_proxy_not_Unity_CPU_or_GPU_present\n",(unsigned long long)ns(),title);fflush(log);
 wait_seconds(60);atomic_store(&phase,0);
 fprintf(log,"phase=baseline end_ns=%llu observed_calls=%u\n",(unsigned long long)ns(),atomic_load(&frame_count));fflush(log);
 struct gciv_config config={DIR,seconds};int result=gc_isolated_verify_start(&config);
 atomic_store(&last_stamp,0);atomic_store(&phase,2);
 fprintf(log,"phase=isolated_on start_ns=%llu start_result=%d\n",(unsigned long long)ns(),result);fflush(log);
 wait_seconds(60);atomic_store(&phase,0);
 FILE *csv=fopen(DIR "/gc6-submission-frames.csv","w");
 unsigned count=atomic_load(&frame_count);
 if(csv){fprintf(csv,"pid,phase,mono_ns,submission_interval_ns,egl_call_ns,success,scope\n");
  for(unsigned i=0;i<count&&i<FRAME_CAP;i++)if(atomic_load_explicit(&frames[i].ready,memory_order_acquire))
   fprintf(csv,"%d,%u,%llu,%llu,%llu,%d,EGL_submission_proxy\n",getpid(),frames[i].phase,(unsigned long long)frames[i].stamp,(unsigned long long)frames[i].interval,(unsigned long long)frames[i].swap,frames[i].success);
  fclose(csv);}
 fprintf(log,"phase=isolated_on end_ns=%llu total_calls=%u overflow=%d\n",(unsigned long long)ns(),count,count>FRAME_CAP);fclose(log);return NULL;
}
void gc_verify_carrier_start(void){
 /* Opt-in verification carrier only. No hooks installed in canonical VM. */
 pthread_t t;if(!pthread_create(&t,NULL,observe,NULL))pthread_detach(t);
}
