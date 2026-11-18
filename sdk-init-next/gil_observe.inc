/* Opt-in native-only diagnostics. Never inspect a running guest's mutable VM
 * stack from the observer; SIGUSR2 capture uses the existing bounded unwinder. */
#include <sys/syscall.h>
#define GIL_DIR "/data/storage/el2/base/haps/entry/files/sdkdex/"
struct gil_obs_row {long tid;char name[32];uint64_t acq,held,maxheld,wait,maxwait,since,hh[10],wh[10];};
static struct gil_obs_row gil_rows[128];
static _Atomic bool gil_obs_enabled,gil_obs_stop;
static bool gil_obs_started;
static pthread_t gil_obs_thread;
static long gil_holder,gil_handoff;
static atomic_uint play_method_seq;
static atomic_long play_method_tid;
static _Atomic(const char *) play_method_class,play_method_name,play_method_sig;
static uint64_t gil_epoch,gil_handoff_since;
static const double gil_bounds[9]={.1,.5,1,2,5,10,50,200,1000};
static unsigned gil_bucket(uint64_t ns){unsigned i=0;while(i<9&&(double)ns/1e6>gil_bounds[i])i++;return i;}
static long gil_tid(void){static __thread long tid;if(!tid)tid=syscall(SYS_gettid);return tid;}
static struct gil_obs_row *gil_row(long tid){for(unsigned i=0;i<128;i++){if(gil_rows[i].tid==tid)return &gil_rows[i];if(!gil_rows[i].tid){gil_rows[i].tid=tid;pthread_getname_np(pthread_self(),gil_rows[i].name,sizeof gil_rows[i].name);return &gil_rows[i];}}return NULL;}
static void gil_obs_acquired(void){
 perf_ffrt_gil_acquired();
 gil_deep_resume();
 gil_holder=gil_tid();gil_handoff=0;gil_epoch++;
 gil_site_resume();
 if(!atomic_load(&gil_obs_enabled))return;struct gil_obs_row *r=gil_row(gil_holder);if(r){r->acq++;if(r->since){uint64_t ns=gc_fast_ns()-r->since;r->wait+=ns;if(ns>r->maxwait)r->maxwait=ns;r->wh[gil_bucket(ns)]++;r->since=0;}else r->wh[0]++;}
}
static void gil_obs_released(uint64_t ns){
 perf_ffrt_gil_released(ns);
 gil_deep_pause();
 gil_site_pause();
 if(atomic_load(&gil_obs_enabled)){struct gil_obs_row *r=gil_row(gil_tid());if(r){r->held+=ns;if(ns>r->maxheld)r->maxheld=ns;r->hh[gil_bucket(ns)]++;}}
 gil_holder=0;
}
extern void unity_player_probe_gil_stack_dump(FILE *,int) __attribute__((weak));
/* Nonblocking producer query: the touch producer must never wait for GIL. */
void dvm_gil_consumer_status(long tid,char *out,size_t size){
 if(pthread_mutex_trylock(&g_gil.m)){snprintf(out,size,"metadata_busy=1 consumer_tid=%ld",tid);return;}
 uint64_t since=0;for(unsigned i=0;i<128;i++)if(gil_rows[i].tid==tid)since=gil_rows[i].since;
 snprintf(out,size,"consumer_tid=%ld gil_waiting=%d wait_ms=%.3f holder_tid=%ld handoff_tid=%ld",tid,since!=0,since?(double)(gc_fast_ns()-since)/1e6:0,gil_holder,gil_handoff);pthread_mutex_unlock(&g_gil.m);
}
void dvm_gil_play_status(char *out,size_t size){
 if(pthread_mutex_trylock(&g_gil.m)){snprintf(out,size,"metadata_busy=1");return;}
 uint64_t now=gc_fast_ns(),since=(uint64_t)g_gil_held_since.tv_sec*1000000000+g_gil_held_since.tv_nsec;
 unsigned seq=atomic_load(&play_method_seq);long tid=atomic_load(&play_method_tid);
 const char *c=atomic_load(&play_method_class),*m=atomic_load(&play_method_name),*sig=atomic_load(&play_method_sig);
 int valid=!(seq&1)&&seq==atomic_load(&play_method_seq)&&tid==gil_holder;
 snprintf(out,size,"holder_tid=%ld held_ms=%.3f waiters=%u method=%s.%s%s method_valid=%d handoff_tid=%ld handoff_ms=%.3f epoch=%llu",gil_holder,gil_holder&&now>=since?(now-since)/1e6:0,atomic_load(&g_gil.waiters),valid&&c?c:"unknown",valid&&m?m:"unknown",valid&&sig?sig:"",valid,gil_handoff,gil_handoff&&now>=gil_handoff_since?(now-gil_handoff_since)/1e6:0,(unsigned long long)gil_epoch);
 pthread_mutex_unlock(&g_gil.m);
}
static int gil_control(const char *name){char path[256];snprintf(path,sizeof path,GIL_DIR "%s",name);FILE *f=fopen(path,"r");int n=0;if(f){(void)fscanf(f,"%d",&n);fclose(f);}return n;}
static void gil_reset(const char *name){char path[256];snprintf(path,sizeof path,GIL_DIR "%s",name);FILE *f=fopen(path,"w");if(f){fputs("0\n",f);fclose(f);}}
struct gil_obs_snapshot {uint64_t stamp,epoch,age,handoff_age;long holder,handoff;unsigned waiters,n;struct gil_obs_row rows[128];};
static void gil_write_snapshot(FILE *f,const struct gil_obs_snapshot *s){
 fprintf(f,"GIL pid=%d mono_ms=%llu holder_tid=%ld holder_ms=%.3f handoff_tid=%ld handoff_ms=%.3f waiters=%u epoch=%llu\n",getpid(),(unsigned long long)(s->stamp/1000000),s->holder,(double)s->age/1e6,s->handoff,(double)s->handoff_age/1e6,s->waiters,(unsigned long long)s->epoch);
 fputs("hist_upper_ms=.1,.5,1,2,5,10,50,200,1000,inf (completed intervals; active wait separately)\n",f);
 for(unsigned i=0;i<s->n;i++){const struct gil_obs_row *r=&s->rows[i];fprintf(f,"tid=%ld name=%s acq=%llu held_ms=%.3f max_held_ms=%.3f wait_ms=%.3f max_wait_ms=%.3f active_wait_ms=%.3f held_hist=",r->tid,r->name,(unsigned long long)r->acq,(double)r->held/1e6,(double)r->maxheld/1e6,(double)r->wait/1e6,(double)r->maxwait/1e6,r->since?(double)(s->stamp-r->since)/1e6:0);for(unsigned j=0;j<10;j++)fprintf(f,"%s%llu",j?",":"",(unsigned long long)r->hh[j]);fputs(" wait_hist=",f);for(unsigned j=0;j<10;j++)fprintf(f,"%s%llu",j?",":"",(unsigned long long)r->wh[j]);fputs("\n",f);}
}
static void *gil_observer(void *unused){
 (void)unused;struct gil_obs_snapshot history[5];unsigned count=0;uint64_t last=0,warned=0;
 while(!atomic_load(&gil_obs_stop)){
  gil_perf_controls();
  atomic_store(&gil_obs_enabled,gil_control("gil-observe")==1);
  if(atomic_load(&gil_obs_enabled)&&!pthread_mutex_trylock(&g_gil.m)){
   struct gil_obs_snapshot s={.stamp=gc_fast_ns(),.holder=gil_holder,.handoff=gil_handoff,.epoch=gil_epoch,.waiters=atomic_load(&g_gil.waiters)};
   s.age=s.holder?s.stamp-((uint64_t)g_gil_held_since.tv_sec*1000000000+g_gil_held_since.tv_nsec):0;s.handoff_age=s.handoff?s.stamp-gil_handoff_since:0;
   while(s.n<128&&gil_rows[s.n].tid){s.rows[s.n]=gil_rows[s.n];s.n++;}pthread_mutex_unlock(&g_gil.m);
   uint64_t ages[128];unsigned n=0;uint64_t longest=0;for(unsigned i=0;i<s.n;i++)if(s.rows[i].since){uint64_t a=s.stamp-s.rows[i].since;unsigned j=n;while(j&&ages[j-1]>a){ages[j]=ages[j-1];j--;}ages[j]=a;n++;if(a>longest)longest=a;}
   if(longest>1000000000&&warned!=s.epoch){warned=s.epoch;FILE *f=fopen(GIL_DIR "gil-warnings.txt","a");if(f){fprintf(f,"WARN wait_over_1s holder_tid=%ld handoff_tid=%ld waiter_median_ms=%.3f longest_wait_ms=%.3f\n",s.holder,s.handoff,n?(double)ages[n/2]/1e6:0,(double)longest/1e6);gil_write_snapshot(f,&s);if(unity_player_probe_gil_stack_dump)unity_player_probe_gil_stack_dump(f,(int)(s.holder?s.holder:s.handoff));fclose(f);}}
   int flush=gil_control("gil-flush-now");if(!last||s.stamp-last>=60000000000ull||flush==1){if(count==5){memmove(history,history+1,4*sizeof *history);count--;}history[count++]=s;FILE *f=fopen(GIL_DIR "gil-stats.txt","w");if(f){for(unsigned i=0;i<count;i++)gil_write_snapshot(f,&history[i]);fclose(f);}last=s.stamp;if(flush==1)gil_reset("gil-flush-now");}
  }
  FILE *perf=fopen(GIL_DIR "gil-perf.txt","w");if(perf){gil_perf_write(perf);fclose(perf);}
  gil_deep_write();
  emu_profile_write();
  struct timespec delay={0,250000000};nanosleep(&delay,NULL);
 }return NULL;
}
extern void jni_gil_long_selftest(struct dvm *) __attribute__((weak));
static void gil_observe_poll(struct dvm *vm){
 static bool controls_ready;
 if(!controls_ready){const char *names[]={"gil-observe","gil-selftest","gil-flush-now"};for(unsigned i=0;i<3;i++){char path[256];snprintf(path,sizeof path,GIL_DIR "%s",names[i]);FILE *f=fopen(path,"wx");if(f){fputs("0\n",f);fclose(f);chmod(path,0666);}}controls_ready=true;}
 if(!gil_obs_started&&gil_control("gil-observe")==1){atomic_store(&gil_obs_enabled,true);atomic_store(&gil_obs_stop,false);if(!pthread_create(&gil_obs_thread,NULL,gil_observer,NULL))gil_obs_started=true;}
 if(gil_control("gil-selftest")==1&&jni_gil_long_selftest){gil_reset("gil-selftest");jni_gil_long_selftest(vm);}
}
static void gil_observe_shutdown(void){if(gil_obs_started){atomic_store(&gil_obs_stop,true);pthread_join(gil_obs_thread,NULL);gil_obs_started=false;}atomic_store(&gil_obs_enabled,false);}
