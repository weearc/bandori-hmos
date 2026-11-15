/* Numeric-only observation of REAL events. No VM access or file IO here.
 * Aggregate counters remain exact even if the bounded detail ring is full. */
#define INPUT_OBS_CAP 4096u
#define INPUT_OBS_STAGES 6u
enum { INPUT_HOST, INPUT_ENQUEUE, INPUT_REJECT, INPUT_GUEST, INPUT_NATIVE, INPUT_GIL };
struct input_obs_row { uint64_t seq,begin,end; int stage,action,rc; };
static struct input_obs_row input_obs_rows[INPUT_OBS_CAP];
static unsigned input_obs_head,input_obs_tail;
static pthread_mutex_t input_obs_lock=PTHREAD_MUTEX_INITIALIZER;
static atomic_int input_obs_enabled;
static atomic_uint_fast64_t input_obs_counts[INPUT_OBS_STAGES][8],input_obs_errors[INPUT_OBS_STAGES];
static atomic_uint_fast64_t input_obs_zero[INPUT_OBS_STAGES],input_obs_positive[INPUT_OBS_STAGES];
static atomic_uint_fast64_t input_obs_lost,input_obs_wait_hist[8],input_obs_gil_hist[8],input_obs_wait_max;
static _Thread_local uint64_t input_obs_gil_begin;
static void input_obs_queue_snapshot(uint64_t *);
static unsigned input_obs_bucket(uint64_t ns){
 static const uint64_t limits[]={100000,500000,1000000,5000000,16000000,33000000,100000000};
 unsigned i=0;while(i<7&&ns>limits[i])i++;return i;
}
static void input_obs_record(int stage,uint64_t seq,int action,int rc,uint64_t begin,uint64_t end){
 if(!atomic_load_explicit(&input_obs_enabled,memory_order_relaxed))return;
 int saved=errno;unsigned a=(unsigned)(action&255);if(a>6)a=7;
 atomic_fetch_add_explicit(&input_obs_counts[stage][a],1,memory_order_relaxed);
 if(rc<0)atomic_fetch_add_explicit(&input_obs_errors[stage],1,memory_order_relaxed);
 else if(rc==0)atomic_fetch_add_explicit(&input_obs_zero[stage],1,memory_order_relaxed);
 else atomic_fetch_add_explicit(&input_obs_positive[stage],1,memory_order_relaxed);
 if(pthread_mutex_trylock(&input_obs_lock)){atomic_fetch_add(&input_obs_lost,1);errno=saved;return;}
 unsigned next=(input_obs_tail+1)%INPUT_OBS_CAP;
 if(next==input_obs_head)atomic_fetch_add(&input_obs_lost,1);
 else {input_obs_rows[input_obs_tail]=(struct input_obs_row){seq,begin,end,stage,action,rc};input_obs_tail=next;}
 pthread_mutex_unlock(&input_obs_lock);errno=saved;
}
/* Called by XComponent after its REAL event read/queue result, including
 * fetch failures and pre-ABI rejections. No coordinates/pointer IDs stored. */
__attribute__((visibility("default"))) void sdk_input_observe_host(uint64_t seq,int action,int rc,uint64_t begin,uint64_t end){
 input_obs_record(INPUT_HOST,seq,action,rc,begin,end);
}
static void input_obs_queue_wait(uint64_t begin,uint64_t end){
 if(!atomic_load(&input_obs_enabled)||end<begin)return;uint64_t ns=end-begin;
 atomic_fetch_add(&input_obs_wait_hist[input_obs_bucket(ns)],1);
 uint64_t old=atomic_load(&input_obs_wait_max);
 while(old<ns&&!atomic_compare_exchange_weak(&input_obs_wait_max,&old,ns)){}
}
static void input_obs_gil(int acquired){
 if(!atomic_load(&input_obs_enabled)){input_obs_gil_begin=0;return;}
 uint64_t now=perf_now();
 if(!acquired){if(!input_obs_gil_begin)input_obs_gil_begin=now;return;}
 if(input_obs_gil_begin){uint64_t begin=input_obs_gil_begin;input_obs_gil_begin=0;
  atomic_fetch_add(&input_obs_gil_hist[input_obs_bucket(now-begin)],1);
  input_obs_record(INPUT_GIL,0,-1,0,begin,now);
 }
}
static FILE *input_obs_file(const char *name,const char *mode){
 char path[512],backup[520];snprintf(path,sizeof path,"/data/storage/el2/base/haps/entry/files/sdkdex/%s",name);
 struct stat st;if(!stat(path,&st)&&st.st_size>=1024*1024){snprintf(backup,sizeof backup,"%s.1",path);unlink(backup);rename(path,backup);}
 FILE *f=fopen(path,mode);if(f&&fchmod(fileno(f),0644)){fclose(f);return NULL;}return f;
}
/* Existing independent native worker only; no touch_lock held for IO. */
static void input_obs_flush(void){
 static int was_enabled;int on=atomic_load(&input_obs_enabled);
 pthread_mutex_lock(&input_obs_lock);int pending=input_obs_head!=input_obs_tail;pthread_mutex_unlock(&input_obs_lock);
 if(!on&&!was_enabled&&!pending)return;was_enabled=on;
 FILE *f=input_obs_file("input-pipeline.csv","a");
 if(f){fseek(f,0,SEEK_END);if(!ftell(f))fputs("pid,seq,stage,action,return,begin_ns,end_ns\n",f);
  for(unsigned n=0;n<2048;n++){
   struct input_obs_row row;pthread_mutex_lock(&input_obs_lock);int have=input_obs_head!=input_obs_tail;
   if(have){row=input_obs_rows[input_obs_head];input_obs_head=(input_obs_head+1)%INPUT_OBS_CAP;}
   pthread_mutex_unlock(&input_obs_lock);if(!have)break;
   fprintf(f,"%d,%llu,%d,%d,%d,%llu,%llu\n",getpid(),(unsigned long long)row.seq,row.stage,row.action,row.rc,(unsigned long long)row.begin,(unsigned long long)row.end);
  }fclose(f);
 }
 f=input_obs_file("input-pipeline-status.txt","w");if(!f)return;
 uint64_t q[8];input_obs_queue_snapshot(q);
 fprintf(f,"queued=%llu injected=%llu failed=%llu dropped=%llu depth=%llu capacity=%llu full=%llu gil_try_failures=%llu\n",(unsigned long long)q[0],(unsigned long long)q[1],(unsigned long long)q[2],(unsigned long long)q[3],(unsigned long long)q[4],(unsigned long long)q[5],(unsigned long long)q[6],(unsigned long long)q[7]);
 fprintf(f,"pid=%d mono_ns=%llu enabled=%d detail_capacity=%u detail_lost=%llu queue_wait_max_ns=%llu coalesced=0 gil_hist_scope=failed_try_to_next_acquire_not_exclusive_wait\n",getpid(),(unsigned long long)perf_now(),on,INPUT_OBS_CAP,(unsigned long long)atomic_load(&input_obs_lost),(unsigned long long)atomic_load(&input_obs_wait_max));
 for(unsigned s=0;s<INPUT_OBS_STAGES;s++){
  for(unsigned a=0;a<8;a++)fprintf(f,"stage_%u_action_%u=%llu\n",s,a,(unsigned long long)atomic_load(&input_obs_counts[s][a]));
  fprintf(f,"stage_%u_errors=%llu\n",s,(unsigned long long)atomic_load(&input_obs_errors[s]));
  fprintf(f,"stage_%u_return_zero=%llu stage_%u_return_positive=%llu\n",s,(unsigned long long)atomic_load(&input_obs_zero[s]),s,(unsigned long long)atomic_load(&input_obs_positive[s]));
 }
 for(unsigned i=0;i<8;i++)fprintf(f,"queue_wait_bucket_%u=%llu gil_retry_span_bucket_%u=%llu\n",i,(unsigned long long)atomic_load(&input_obs_wait_hist[i]),i,(unsigned long long)atomic_load(&input_obs_gil_hist[i]));
 fclose(f);
}
