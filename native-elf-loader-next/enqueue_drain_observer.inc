/* No guest inspection. Only fixed native records on producers; all control
 * reads, epoch commands and disk writes belong to this independent worker. */
#include <sys/eventfd.h>
#include <poll.h>
#define ED_DIR "/data/storage/el2/base/haps/entry/files/sdkdex/"
#define ED_CAP 8192
static atomic_int ed_wake,ed_observe,ed_every_frame;
static int ed_fd=-1;
static atomic_uint_fast64_t ed_wake_sent,ed_wake_failed,ed_owner_wakes;
struct ed_row {uint64_t seq,host,enqueue,drain,construct_begin,construct,end;int ok,consumed;};
static struct ed_row ed_rows[ED_CAP];
static pthread_mutex_t ed_lock=PTHREAD_MUTEX_INITIALIZER;
static unsigned ed_head,ed_tail;
static atomic_uint_fast64_t ed_lost;
static void ed_notify(void){
 if(!atomic_load(&ed_wake)||ed_fd<0)return;
 uint64_t one=1;ssize_t rc;
 rc=write(ed_fd,&one,sizeof one);
 if(rc==sizeof one)atomic_fetch_add(&ed_wake_sent,1);
 else if(errno!=EAGAIN)atomic_fetch_add(&ed_wake_failed,1);
}
static void ed_wait_owner(void){
 if(!atomic_load(&ed_wake)||ed_fd<0){struct timespec t={0,100000000};nanosleep(&t,NULL);return;}
 /* Called only after dropping ALL GIL ownership. Timeout preserves the old
  * lifecycle/control poll; producer notification replaces the 100ms delay. */
 struct pollfd p={.fd=ed_fd,.events=POLLIN};
 if(poll(&p,1,100)>0&&(p.revents&POLLIN)){
  uint64_t count;if(read(ed_fd,&count,sizeof count)==sizeof count)atomic_fetch_add(&ed_owner_wakes,1);
 }
}
static void ed_record(struct ed_row row){
 if(!atomic_load(&ed_observe))return;
 if(pthread_mutex_trylock(&ed_lock)){atomic_fetch_add(&ed_lost,1);return;}
 unsigned next=(ed_tail+1)%ED_CAP;
 if(next==ed_head)atomic_fetch_add(&ed_lost,1);
 else{ed_rows[ed_tail]=row;ed_tail=next;}
 pthread_mutex_unlock(&ed_lock);
}
extern void sdk_input_play_snapshot(uint64_t *);
static int ed_flag(const char *name){
 char path[256];snprintf(path,sizeof path,ED_DIR "%s",name);FILE *f=fopen(path,"r");int value=0;
 if(f){(void)fscanf(f,"%d",&value);fclose(f);}return value==1;
}
static void *ed_worker(void *unused){
 (void)unused;unsigned long long command=0,epoch=0;int scene=4;
 const char *scenes[]={"home_menu","song_selection_loading","playing","result","other"};
 uint64_t previous=0;
 for(;;){
  struct timespec delay={1,0};nanosleep(&delay,NULL);
  uint64_t now=perf_now();if(previous&&now-previous<1000000000ull)continue;previous=now;
  multitouch_poll(); /* Control reads and trace exports: never under GIL. */
  atomic_store_explicit(&touch_worker_enabled,ed_flag("touch-drain-worker"),memory_order_release);
  atomic_store(&ed_wake,ed_flag("enqueue-drain-wake"));
  atomic_store(&ed_observe,ed_flag("enqueue-drain-observe"));
  atomic_store(&input_obs_enabled,atomic_load(&ed_observe));
  input_obs_flush();
  atomic_store(&ed_every_frame,ed_flag("perf-touch-every-frame"));
  extern void perf_audio_controls(void);perf_audio_controls();
  /* Explicit human-declared stage only. A stale PID or sequence is rejected.
   * No scene name traversal, input-density heuristic or nativeRender hook. */
  FILE *f=fopen(ED_DIR "scene-epoch-command","r");
  if(f){int pid=-1,value=-1;unsigned long long serial=0;char extra;
   if(fscanf(f,"%d %llu %d %c",&pid,&serial,&value,&extra)==3&&pid==getpid()&&serial>command&&value>=0&&value<5){command=serial;scene=value;epoch++;}
   fclose(f);
  }
  f=fopen(ED_DIR "scene-epochs.csv","a");
  if(f){fseek(f,0,SEEK_END);if(!ftell(f))fputs("pid,mono_ns,epoch,command,scene,source,automatic_scene_available\n",f);
   fprintf(f,"%d,%llu,%llu,%llu,%s,%s,0\n",getpid(),(unsigned long long)now,epoch,command,scenes[scene],command?"operator_declared":"unknown");fclose(f);}
  f=fopen(ED_DIR "enqueue-drain-events.csv","a");
  if(f){fseek(f,0,SEEK_END);if(!ftell(f))fputs("pid,seq,host_ns,enqueue_ns,drain_ns,construct_begin_ns,construct_ns,return_ns,ok,consumed\n",f);
   for(unsigned n=0;n<ED_CAP;n++){
    struct ed_row row;pthread_mutex_lock(&ed_lock);int have=ed_head!=ed_tail;
    if(have){row=ed_rows[ed_head];ed_head=(ed_head+1)%ED_CAP;}pthread_mutex_unlock(&ed_lock);
    if(!have)break;
    fprintf(f,"%d,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%d,%d\n",getpid(),(unsigned long long)row.seq,(unsigned long long)row.host,(unsigned long long)row.enqueue,(unsigned long long)row.drain,(unsigned long long)row.construct_begin,(unsigned long long)row.construct,(unsigned long long)row.end,row.ok,row.consumed);
   }fclose(f);}
  uint64_t values[8];sdk_input_play_snapshot(values);
  f=fopen(ED_DIR "enqueue-drain-counters.csv","a");
  if(f){fseek(f,0,SEEK_END);if(!ftell(f))fputs("pid,mono_ns,wake_enabled,observe_enabled,queued,injected,failed,dropped,depth,drain_calls,drained,unpaired,wake_sent,wake_failed,owner_wakes,trace_lost\n",f);
   fprintf(f,"%d,%llu,%d,%d,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n",getpid(),(unsigned long long)now,atomic_load(&ed_wake),atomic_load(&ed_observe),(unsigned long long)values[0],(unsigned long long)values[1],(unsigned long long)values[2],(unsigned long long)values[3],(unsigned long long)values[4],(unsigned long long)values[5],(unsigned long long)values[6],(unsigned long long)values[7],(unsigned long long)atomic_load(&ed_wake_sent),(unsigned long long)atomic_load(&ed_wake_failed),(unsigned long long)atomic_load(&ed_owner_wakes),(unsigned long long)atomic_load(&ed_lost));fclose(f);}
 }
 return NULL;
}
static void ed_start(void){
 static int started;if(started)return;started=1;
 ed_fd=eventfd(0,EFD_NONBLOCK|EFD_CLOEXEC);
 const char *names[]={"enqueue-drain-wake","enqueue-drain-observe","scene-epoch-command"};
 for(unsigned i=0;i<3;i++){char path[256];snprintf(path,sizeof path,ED_DIR "%s",names[i]);FILE *f=fopen(path,"wx");if(f){fputs(i==2?"0 0 4\n":"0\n",f);fclose(f);chmod(path,0666);}}
 pthread_t thread;if(pthread_create(&thread,NULL,ed_worker,NULL)==0)pthread_detach(thread);
}
