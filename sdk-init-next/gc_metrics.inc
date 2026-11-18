/* Included by dvm.c, independent of root layout and reclaim enablement. */
#include "gc_metrics.h"
#include "gc_runtime.h"
#include "gc_generation.h"
#include <fcntl.h>
#include <sys/stat.h>
#ifndef DVM_GC_METRICS_DIR
#define DVM_GC_METRICS_DIR "/data/storage/el2/base/haps/entry/files/sdkdex/"
#endif
static pthread_mutex_t gc_metrics_lock=PTHREAD_MUTEX_INITIALIZER;
static struct dvm_gc_metrics gc_metrics;
static _Atomic uint64_t gc_metrics_dropped;
static unsigned gc_metrics_reason(const char *s){
 if(s&&(!strcmp(s,"manual_debug")||!strcmp(s,"explicit")))return DVM_GC_EXPLICIT;
 if(s&&!strcmp(s,"idle"))return DVM_GC_IDLE;
 return DVM_GC_PRESSURE;
}
void dvm_gc_metrics_reset(void){
 pthread_mutex_lock(&gc_metrics_lock);memset(&gc_metrics,0,sizeof gc_metrics);
 atomic_store(&gc_metrics_dropped,0);pthread_mutex_unlock(&gc_metrics_lock);
}
void dvm_gc_metrics_attempt(const char *reason){
 if(pthread_mutex_trylock(&gc_metrics_lock)){atomic_fetch_add(&gc_metrics_dropped,1);return;}
 gc_metrics.last_reason=gc_metrics_reason(reason);gc_metrics.attempts[gc_metrics.last_reason]++;
 pthread_mutex_unlock(&gc_metrics_lock);
}
void dvm_gc_metrics_finish(const char *reason,bool ok,size_t objects,size_t bytes,size_t live,uint64_t pause){
 if(pthread_mutex_trylock(&gc_metrics_lock)){atomic_fetch_add(&gc_metrics_dropped,1);return;}
 gc_metrics.last_reason=gc_metrics_reason(reason);
 if(ok){gc_metrics.completed++;gc_metrics.last_live=live;gc_metrics.last_live_valid=true;}
 else gc_metrics.aborted++;
 /* An aborted sweep may already have freed proven-dead objects. */
 gc_metrics.reclaimed_objects+=objects;gc_metrics.reclaimed_bytes+=bytes;
 size_t bin=pause?(pause-1)/250000u:0;if(bin>128)bin=128;
 gc_metrics.pause_bins[bin]++;gc_metrics.pause_samples++;
 if(pause>gc_metrics.pause_max_ns)gc_metrics.pause_max_ns=pause;
 pthread_mutex_unlock(&gc_metrics_lock);
}
bool dvm_gc_metrics_snapshot(struct dvm_gc_metrics *out){
 if(!out||pthread_mutex_trylock(&gc_metrics_lock))return false;
 *out=gc_metrics;out->dropped=atomic_load(&gc_metrics_dropped);
 pthread_mutex_unlock(&gc_metrics_lock);return true;
}
static int64_t gc_metrics_percentile(const struct dvm_gc_metrics *s,unsigned p){
 if(!s->pause_samples)return -1;
 uint64_t target=(s->pause_samples/100)*p+((s->pause_samples%100)*p+99)/100,n=0;
 for(unsigned i=0;i<129;i++){n+=s->pause_bins[i];if(n>=target)return i==128?-1:(int64_t)(i+1)*250000;}
 return -1;
}
void dvm_gc_metrics_worker(void){
 static uint64_t last;uint64_t now=dvm__now_ms();if(now-last<5000)return;last=now;
 const char *dir=DVM_GC_METRICS_DIR;
 char path[512],backup[512],control[512];
 snprintf(control,sizeof control,"%sgc-moving-enabled",dir);
 int fd=open(control,O_WRONLY|O_CREAT|O_EXCL,0644);
 if(fd>=0){fchmod(fd,0644);(void)write(fd,"0\n",2);close(fd);}
 FILE *f=fopen(control,"r");int requested=0;if(f){(void)fscanf(f,"%d",&requested);fclose(f);}
 /* An explicit request cannot replace the missing native/raw-root proof.
  * No production relocation entry point exists until that audit passes. */
 bool rejected=requested!=0;static bool rejected_latched;
 if(rejected)rejected_latched=true;
 if(rejected){fd=open(control,O_WRONLY|O_TRUNC);if(fd>=0){fchmod(fd,0644);(void)write(fd,"0\n",2);close(fd);}}
 struct vm_arena_region_snapshot a={0};struct vm_external_snapshot x={0};struct dvm_gc_metrics m;
 if(!vm_arena_region_snapshot(&a)||!a.active||!dvm_gc_metrics_snapshot(&m))return;
 vm_external_snapshot(&x);
 /* Fixed-size latest snapshot, only when counts change; no hot-path IO. */
 uint64_t faults[GC_CONTRACT_REASONS];static uint64_t previous_faults[GC_CONTRACT_REASONS];static bool contract_written;
 dvm_gc_contract_snapshot(faults);
 if(!contract_written||memcmp(faults,previous_faults,sizeof faults)){
  snprintf(path,sizeof path,"%sgc-contract-status.txt",dir);
  fd=open(path,O_WRONLY|O_CREAT|O_TRUNC,0644);
  if(fd>=0){
   if(!fchmod(fd,0644)){
    dprintf(fd,"pid=%d mono_ms=%llu healthy=%d moving_enabled=0\n",getpid(),(unsigned long long)now,dvm_gc_contract_healthy());
    for(unsigned i=0;i<GC_CONTRACT_REASONS;i++)dprintf(fd,"reason_%u=%llu\n",i,(unsigned long long)faults[i]);
    memcpy(previous_faults,faults,sizeof faults);contract_written=true;
   }close(fd);
  }
 }
 /* Directory counters are independent of contract health and reclaim. */
 /* Shadow-generation observation only: never create/publish/check here. */
 struct gen_counters gs;gen_snapshot(&gs);
 snprintf(path,sizeof path,"%sgc-generation-status.txt",dir);
 fd=open(path,O_WRONLY|O_CREAT|O_TRUNC,0644);
 if(fd>=0){if(!fchmod(fd,0644)){
  dprintf(fd,"pid=%d mono_ms=%llu capacity_ids=%u gen_issued=%llu gen_reuse_bumps=%llu gen_check_ok=%llu gen_check_stale=%llu gen_overflow=%llu\n",getpid(),(unsigned long long)now,DVM_GEN_SLOT_CAPACITY,(unsigned long long)gs.gen_issued,(unsigned long long)gs.gen_reuse_bumps,(unsigned long long)gs.gen_check_ok,(unsigned long long)gs.gen_check_stale,(unsigned long long)gs.gen_overflow);
  dprintf(fd,"heap_alloc_attempts=%llu heap_published=%llu heap_reused=%llu heap_freed=%llu heap_gen_failures=%llu first_gen_error=%u scope_gen_failures=%llu scope_faults_lost=%llu\n",(unsigned long long)atomic_load(&gen_heap_attempts),(unsigned long long)atomic_load(&gen_heap_published),(unsigned long long)atomic_load(&gen_heap_reused),(unsigned long long)atomic_load(&gen_heap_freed),(unsigned long long)atomic_load(&gen_heap_failures),atomic_load(&gen_heap_first_error),(unsigned long long)atomic_load(&gc_scope_fault_count),(unsigned long long)atomic_load(&gc_scope_fault_lost));
  for(unsigned i=1;i<=GEN_ALLOCATION_FAILED;i++)dprintf(fd,"heap_gen_error_%u=%llu\n",i,(unsigned long long)atomic_load(&gen_heap_failure_reason[i]));
  if(atomic_load(&gc_scope_fault_count))gc_scope_fault_export(fd);
 }close(fd);}
 struct dvm_gc_directory_stats ds;dvm_gc_directory_snapshot(&ds);
 snprintf(path,sizeof path,"%sgc-directory-status.txt",dir);
 fd=open(path,O_WRONLY|O_CREAT|O_TRUNC,0644);
 if(fd>=0){if(!fchmod(fd,0644))dprintf(fd,"pid=%d mono_ms=%llu dual_path=%d capacity_ids=%llu queries=%llu hits=%llu misses=%llu mismatches=%llu overflows=%llu metadata_bytes=%llu mismatch_ref=%llu mismatch_legacy=%llu mismatch_directory=%llu rev_queries=%llu rev_hits=%llu rev_misses=%llu rev_mismatches=%llu rev_overflows=%llu rev_bad_address=%llu rev_bad_legacy=%llu rev_bad_directory=%llu rev_bad_reason=%llu\n",getpid(),(unsigned long long)now,DVM_ID_DIRECTORY_DUAL,(unsigned long long)DVM_ID_DIRECTORY_BLOCKS*DVM_HEAP_BLOCK,(unsigned long long)ds.queries,(unsigned long long)ds.hits,(unsigned long long)ds.misses,(unsigned long long)ds.mismatches,(unsigned long long)ds.overflows,(unsigned long long)ds.metadata_bytes,(unsigned long long)ds.mismatch_ref,(unsigned long long)ds.mismatch_legacy,(unsigned long long)ds.mismatch_directory,(unsigned long long)ds.rev_queries,(unsigned long long)ds.rev_hits,(unsigned long long)ds.rev_misses,(unsigned long long)ds.rev_mismatches,(unsigned long long)ds.rev_overflows,(unsigned long long)ds.rev_bad_address,(unsigned long long)ds.rev_bad_legacy,(unsigned long long)ds.rev_bad_directory,(unsigned long long)ds.rev_bad_reason);close(fd);}
 snprintf(path,sizeof path,"%sgc-status.csv",dir);snprintf(backup,sizeof backup,"%sgc-status.csv.1",dir);
 struct stat st;if(!stat(path,&st)&&st.st_size>=1024*1024){unlink(backup);if(rename(path,backup))return;}
 fd=open(path,O_WRONLY|O_CREAT|O_APPEND,0644);if(fd<0)return;if(fchmod(fd,0644)){close(fd);return;}
 if(lseek(fd,0,SEEK_END)==0)dprintf(fd,"pid,mono_ms,heap_committed,heap_used,live_after_gc,live_after_gc_valid,reclaimed_objects,reclaimed_bytes,gc_count,gc_aborted,gc_reason,pause_samples,pause_p50_ns_le,pause_p99_ns_le,pause_max_ns,fragmentation_estimate,pressure_requests,explicit_requests,idle_requests,metrics_dropped,external_bytes,moving_requested,moving_enabled,moving_reason\n");
 static const char *names[]={"pressure","explicit","idle"};
 double capacity=a.committed_bytes>=a.metadata_bytes?(double)(a.committed_bytes-a.metadata_bytes):0;
 dprintf(fd,"%d,%llu,%zu,%zu,%llu,%d,%llu,%llu,%llu,%llu,%s,%llu,%lld,%lld,%llu,%.8f,%llu,%llu,%llu,%llu,%zu,%d,0,%s\n",
 getpid(),(unsigned long long)now,a.committed_bytes,a.live_bytes,(unsigned long long)m.last_live,m.last_live_valid,
 (unsigned long long)m.reclaimed_objects,(unsigned long long)m.reclaimed_bytes,(unsigned long long)m.completed,
 (unsigned long long)m.aborted,m.pause_samples||m.attempts[0]||m.attempts[1]||m.attempts[2]?names[m.last_reason]:"none",
 (unsigned long long)m.pause_samples,(long long)gc_metrics_percentile(&m,50),(long long)gc_metrics_percentile(&m,99),
 (unsigned long long)m.pause_max_ns,capacity?(double)a.free_bytes/capacity:0,
 (unsigned long long)m.attempts[0],(unsigned long long)m.attempts[1],(unsigned long long)m.attempts[2],
 (unsigned long long)m.dropped,x.bytes,rejected,rejected_latched?"native_and_root_audit_incomplete":"default_off");close(fd);
}
