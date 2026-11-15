#define _POSIX_C_SOURCE 200809L
#include "gc_isolated_verify.h"
#include "gc_isolated_satb.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>

#define GCIV_GRAPH 32u
#define GCIV_TICK_NS UINT64_C(250000000)
#define GCIV_CYCLE_NS UINT64_C(5000000000)
#define GCIV_ARENA (16u*1024u*1024u)
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static struct gciv_report report;
static atomic_bool stop_requested;
static unsigned duration;

static bool monotonic(uint64_t *ns){
 struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))return false;
 *ns=(uint64_t)t.tv_sec*UINT64_C(1000000000)+(uint64_t)t.tv_nsec;return true;
}
static bool path(char *out,size_t cap,const char *dir,const char *name){
 int n=snprintf(out,cap,"%s/%s",dir,name);return n>0&&(size_t)n<cap;
}
static size_t process_rss(void){
 FILE *f=fopen("/proc/self/statm","r");if(!f)return SIZE_MAX;
 unsigned long long virtual_pages=0,rss=0;int n=fscanf(f,"%llu %llu",&virtual_pages,&rss);fclose(f);
 long page=sysconf(_SC_PAGESIZE);if(n!=2||page<=0||rss>SIZE_MAX/(size_t)page)return SIZE_MAX;
 return (size_t)rss*(size_t)page;
}
static void publish(unsigned elapsed,size_t cycles,uint64_t max_pause){
 pthread_mutex_lock(&lock);report.elapsed_seconds=elapsed;report.collections=cycles;report.max_isolated_pause_ns=max_pause;pthread_mutex_unlock(&lock);
}
static void memory_row(FILE *f,gcs_heap *h,uint64_t elapsed,const char *point){
 struct gcs_evac_stats s={0};gcs_evac_stats(h,&s);size_t rss=process_rss();
 fprintf(f,"isolated-only,%llu,%s,%zu,%zu,%zu,%zu,%zu,%zu,",(unsigned long long)(elapsed/1000000),point,s.committed_bytes,s.reserved_bytes,s.retained_forwarding_bytes,s.logical_dead_bytes,s.physical_reclaimed_bytes,s.reuse_hits);
 if(rss==SIZE_MAX)fputs("NA",f);else fprintf(f,"%zu",rss);
 fputs(",0,0,NA\n",f);
}
static bool ring_ok(gcs_heap *h,gcs_object *root){
 gcs_object *p=root;for(unsigned i=0;i<GCIV_GRAPH;i++){if(!p)return false;p=gcs_get_ref(h,p,0);if(i+1<GCIV_GRAPH&&p==root)return false;}return p==root;
}
static void write_report(int state,int error){
 struct gciv_report r;pthread_mutex_lock(&lock);report.state=state;report.error=error;r=report;pthread_mutex_unlock(&lock);
 FILE *f=fopen(r.report_json,"w");if(!f){pthread_mutex_lock(&lock);report.state=-1;report.error=errno?errno:EIO;pthread_mutex_unlock(&lock);return;}
 fprintf(f,"{\"scope\":\"isolated-only\",\"state\":%d,\"error\":%d,\"requested_seconds\":%u,\"elapsed_seconds\":%u,\"collections\":%zu,\"max_isolated_pause_ns\":%llu,\"clock\":\"CLOCK_MONOTONIC\",\"pause_boundary\":\"complete gcs_evacuate call while isolated mutator is quiescent\",\"hard_pause_limit_verified\":false,\"real_vm_moving_enabled\":0,\"real_vm_collection_enabled\":0,\"real_vm_pause_samples\":\"NA\",\"process_rss_scope\":\"whole carrier process; not isolated heap\"}\n",r.state,r.error,duration,r.elapsed_seconds,r.collections,r.max_isolated_pause_ns);
 if(fclose(f)){pthread_mutex_lock(&lock);report.state=-1;report.error=EIO;pthread_mutex_unlock(&lock);}
}
static void *worker(void *unused){
 (void)unused;int error=0;FILE *pauses=NULL,*memory=NULL;gcs_heap *h=NULL;uint64_t start=0,now=0,max_pause=0;size_t cycles=0;
 pauses=fopen(report.pause_csv,"w");memory=fopen(report.memory_csv,"w");if(!pauses||!memory){error=errno?errno:EIO;goto done;}
 fputs("scope,cycle,elapsed_ms,isolated_pause_ns,status,committed_before,committed_after,physical_reclaimed_delta,logical_dead_delta,retained_forwarding_bytes,real_vm_collection_enabled,real_vm_pause_ns\n",pauses);
 fputs("scope,elapsed_ms,point,isolated_committed_bytes,isolated_reserved_bytes,retained_forwarding_bytes,logical_dead_bytes,physical_reclaimed_bytes,reuse_hits,whole_process_rss_bytes,real_vm_moving_enabled,real_vm_collection_enabled,real_vm_pause_ns\n",memory);
 h=gcs_create_evacuating(GCIV_ARENA,4096);if(!h){error=ENOMEM;goto done;}
 uint32_t edge[]={0};struct dvm_gc_layout node={.instance_slots=edge,.instance_count=1},leaf={0};
 gcs_object *root=NULL,*previous=NULL;
 for(unsigned i=0;i<GCIV_GRAPH;i++){gcs_object *o=gcs_alloc(h,&node,2);if(!o){error=GCS_OOM;goto done;}if(previous&&gcs_put_ref(h,previous,0,o)){error=GCS_INVALID;goto done;}if(!root)root=o;previous=o;}
 if(gcs_put_ref(h,previous,0,root)||!ring_ok(h,root)){error=GCS_INVALID;goto done;}
 if(!monotonic(&start)){error=errno?errno:EIO;goto done;}
 now=start;uint64_t next_tick=start,next_cycle=start+GCIV_CYCLE_NS,next_memory=start;bool memory_complete=false;
 memory_row(memory,h,0,"start");write_report(1,0);
 while(now-start<(uint64_t)duration*UINT64_C(1000000000)&&!atomic_load_explicit(&stop_requested,memory_order_acquire)){
  /* This sole mutator allocates outside the measured collection boundary.
   * Every garbage object has a real page-backed block and scalar write. */
  for(unsigned i=0;i<16;i++){gcs_object *garbage=gcs_alloc(h,&leaf,16);if(!garbage||gcs_put_scalar(h,garbage,0,(uintptr_t)(cycles+1))){error=GCS_OOM;goto done;}}
  if(!monotonic(&now)){error=EIO;goto done;}
  if(now>=next_memory&&!memory_complete){memory_row(memory,h,now-start,now-start>=UINT64_C(300000000000)?"300s-endpoint":"sample");if(now-start>=UINT64_C(300000000000))memory_complete=true;next_memory=now+UINT64_C(1000000000);}
  if(now>=next_cycle){
   struct gcs_evac_stats before={0},after={0};gcs_evac_stats(h,&before);
   if(now-start<=UINT64_C(300000000000))memory_row(memory,h,now-start,"before-collection");
   uint64_t begin,end;if(!monotonic(&begin)){error=EIO;goto done;}
   /* ACTUAL isolated safepoint: this thread owns all mutator access. Include
    * mark-worker creation/join, root/field repair and page decommit. */
   int status=gcs_evacuate(h,&root,1);
   if(!monotonic(&end)){error=EIO;goto done;}
   uint64_t pause=end-begin;if(pause>max_pause)max_pause=pause;cycles++;
   gcs_evac_stats(h,&after);
   fprintf(pauses,"isolated-only,%zu,%llu,%llu,%d,%zu,%zu,%zu,%zu,%zu,0,NA\n",cycles,(unsigned long long)((end-start)/1000000),(unsigned long long)pause,status,before.committed_bytes,after.committed_bytes,after.physical_reclaimed_bytes-before.physical_reclaimed_bytes,after.logical_dead_bytes-before.logical_dead_bytes,after.retained_forwarding_bytes);
   if(end-start<=UINT64_C(300000000000))memory_row(memory,h,end-start,"after-collection");
   publish((unsigned)((end-start)/UINT64_C(1000000000)),cycles,max_pause);
   if(status||!ring_ok(h,root)){error=status?status:GCS_INVALID;goto done;}
   if(fflush(pauses)||fflush(memory)){error=EIO;goto done;}
   write_report(1,0);
   now=end;next_cycle=now+GCIV_CYCLE_NS;
  }
  /* Short absolute sleeps keep Unity's thread free. No busy-spin/catch-up
   * allocation if collection or scheduling exceeded the nominal period. */
  next_tick=now+GCIV_TICK_NS;struct timespec target={(time_t)(next_tick/UINT64_C(1000000000)),(long)(next_tick%UINT64_C(1000000000))};
  int sleep_error;do{sleep_error=clock_nanosleep(CLOCK_MONOTONIC,TIMER_ABSTIME,&target,NULL);}while(sleep_error==EINTR&&!atomic_load_explicit(&stop_requested,memory_order_acquire));
  if(sleep_error&&sleep_error!=EINTR){error=sleep_error;goto done;}
  if(!monotonic(&now)){error=EIO;goto done;}
  publish((unsigned)((now-start)/UINT64_C(1000000000)),cycles,max_pause);
 }
done:
 if(h&&memory&&start&&monotonic(&now)&&now>=start&&now-start<=UINT64_C(300500000000))memory_row(memory,h,now-start,"run-end");
 if(h)gcs_destroy(h);
 if(pauses&&fclose(pauses)&&!error)error=EIO;if(memory&&fclose(memory)&&!error)error=EIO;
 write_report(error?-1:(atomic_load_explicit(&stop_requested,memory_order_acquire)?3:2),error);return NULL;
}
int gc_isolated_verify_start(const struct gciv_config *config){
 if(!config||!config->directory||!config->directory[0]||!config->duration_seconds||config->duration_seconds>86400)return -1;
 pthread_mutex_lock(&lock);if(report.state){pthread_mutex_unlock(&lock);return -2;}
 if(!path(report.pause_csv,sizeof report.pause_csv,config->directory,"gc6-isolated-pauses.csv")||!path(report.memory_csv,sizeof report.memory_csv,config->directory,"gc6-isolated-memory-300s.csv")||!path(report.report_json,sizeof report.report_json,config->directory,"gc6-isolated-report.json")){memset(&report,0,sizeof report);pthread_mutex_unlock(&lock);return -1;}
 duration=config->duration_seconds;atomic_store(&stop_requested,false);report.state=1;
 pthread_attr_t attr;int rc=pthread_attr_init(&attr);if(!rc){rc=pthread_attr_setdetachstate(&attr,PTHREAD_CREATE_DETACHED);if(!rc){pthread_t t;rc=pthread_create(&t,&attr,worker,NULL);}pthread_attr_destroy(&attr);}
 if(rc){report.state=-1;report.error=rc;}pthread_mutex_unlock(&lock);return rc?-1:0;
}
void gc_isolated_verify_get_report(struct gciv_report *out){if(!out)return;pthread_mutex_lock(&lock);*out=report;pthread_mutex_unlock(&lock);}
void gc_isolated_verify_request_stop(void){atomic_store_explicit(&stop_requested,true,memory_order_release);}
