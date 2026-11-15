/* Minimal A/B counters are common to every phase. No IO or waits in producers.
 * Detailed play recording is independent and disabled in A by the operator. */
#include <sys/syscall.h>
#include <pthread.h>
#define FFRT_ROWS 8192
#define FFRT_SECONDS 192
static atomic_int ffrt_mode;
struct ffrt_row {atomic_int ready;unsigned kind,point;long tid;uint64_t begin,end,wait;char guest[192];};
struct ffrt_bank {
 atomic_uint rows,lost,frames,long33,long50,long100,long200;
 atomic_uint_fast64_t start,end,frame_max,gil_max,tag_max;
 atomic_uint_fast64_t io[FFRT_SECONDS][4];
 struct ffrt_row data[FFRT_ROWS];
};
static struct ffrt_bank ffrt_banks[4];
static _Thread_local struct {unsigned calls,updates;uint64_t second;char guest[192];
 int acquired_mode,best_point;uint64_t acquired,best_ns;} ffrt_tls;
int perf_ffrt_mode(void){return atomic_load_explicit(&ffrt_mode,memory_order_relaxed);}
/* Old foreground snapshots are diagnostic-only, synchronous, and unsafe under
 * the GIL. Normal operation skips them; phase B alone reproduces the old path. */
int perf_ffrt_snapshots_enabled(void){return perf_ffrt_mode()==2;}
static void ffrt_max(atomic_uint_fast64_t *p,uint64_t n){uint64_t old=atomic_load(p);while(n>old&&!atomic_compare_exchange_weak(p,&old,n)){} }
static void ffrt_record(int mode,unsigned kind,unsigned point,uint64_t begin,uint64_t end,uint64_t wait){
 if(mode<1||mode>3||begin<atomic_load(&ffrt_banks[mode].start))return;
 struct ffrt_bank *b=&ffrt_banks[mode];unsigned i=atomic_fetch_add(&b->rows,1);
 if(i>=FFRT_ROWS){atomic_fetch_add(&b->lost,1);return;}
 struct ffrt_row *r=&b->data[i];r->kind=kind;r->point=point;r->begin=begin;r->end=end;r->wait=wait;r->tid=syscall(SYS_gettid);
 if(mode==3&&kind==2)memcpy(r->guest,ffrt_tls.guest,sizeof r->guest);
 atomic_store_explicit(&r->ready,1,memory_order_release);
}
void perf_ffrt_frame(uint64_t begin,uint64_t end,uint64_t wait){
 int mode=perf_ffrt_mode();if(!mode||begin<atomic_load(&ffrt_banks[mode].start))return;
 struct ffrt_bank *b=&ffrt_banks[mode];uint64_t n=end-begin;
 atomic_fetch_add(&b->frames,1);ffrt_max(&b->frame_max,n);atomic_store(&b->end,end);
 if(n>33000000){atomic_fetch_add(&b->long33,1);ffrt_record(mode,1,0,begin,end,wait);}
 if(n>50000000)atomic_fetch_add(&b->long50,1);
 if(n>100000000)atomic_fetch_add(&b->long100,1);
 if(n>200000000)atomic_fetch_add(&b->long200,1);
}
void perf_ffrt_gil_acquired(void){
 ffrt_tls.acquired_mode=perf_ffrt_mode();ffrt_tls.best_ns=0;ffrt_tls.best_point=0;
 ffrt_tls.acquired=ffrt_tls.acquired_mode?perf_now():0;
}
void perf_ffrt_gil_released(uint64_t n){
 int mode=perf_ffrt_mode();if(!mode||mode!=ffrt_tls.acquired_mode)return;
 ffrt_max(&ffrt_banks[mode].gil_max,n);
 if(n>=8000000){uint64_t end=perf_now();ffrt_record(mode,2,ffrt_tls.best_point,end-n,end,0);}
}
void perf_ffrt_guest(const char *cls,const char *method,const char *sig){
 if(perf_ffrt_mode()!=3||(++ffrt_tls.calls&63))return;
 uint64_t begin=perf_now(),second=begin/1000000000;
 if(second!=ffrt_tls.second){ffrt_tls.second=second;ffrt_tls.updates=0;}
 if(ffrt_tls.updates>=4)return;ffrt_tls.updates++;
 snprintf(ffrt_tls.guest,sizeof ffrt_tls.guest,"%.72s.%.64s%.48s",cls?cls:"?",method?method:"?",sig?sig:"");
 ffrt_max(&ffrt_banks[3].tag_max,perf_now()-begin);
}
uint64_t perf_ffrt_native_begin(int point){(void)point;return perf_ffrt_mode()?perf_now():0;}
void perf_ffrt_native_end(int point,uint64_t begin){
 if(!begin)return;uint64_t end=perf_now();int mode=perf_ffrt_mode();
 if(end-begin>ffrt_tls.best_ns){ffrt_tls.best_ns=end-begin;ffrt_tls.best_point=point;}
 ffrt_record(mode,3,point,begin,end,0);
}
/* Coverage: Java clear/TLS socket operations and guest libc file opens only;
 * never store paths, URLs, payloads or account data. */
void perf_ffrt_io(unsigned kind){
 int mode=perf_ffrt_mode();if(!mode||kind>=4)return;
 uint64_t start=atomic_load(&ffrt_banks[mode].start),now=perf_now();
 if(now>=start&&(now-start)/1000000000<FFRT_SECONDS)atomic_fetch_add(&ffrt_banks[mode].io[(now-start)/1000000000][kind],1);
}
static void ffrt_export(int mode){
 if(mode<1||mode>3)return;struct ffrt_bank *b=&ffrt_banks[mode];
 FILE *f=fopen(DIR "ffrt-stall-summary.csv","a");if(f){
  fseek(f,0,SEEK_END);if(!ftell(f))fputs("pid,phase,start_ns,end_ns,frames,gt33,gt50,gt100,gt200,frame_max_ns,gil_max_ns,rows,lost,tag_max_ns\n",f);
  fprintf(f,"%d,%d,%llu,%llu,%u,%u,%u,%u,%u,%llu,%llu,%u,%u,%llu\n",getpid(),mode,(unsigned long long)atomic_load(&b->start),(unsigned long long)atomic_load(&b->end),atomic_load(&b->frames),atomic_load(&b->long33),atomic_load(&b->long50),atomic_load(&b->long100),atomic_load(&b->long200),(unsigned long long)atomic_load(&b->frame_max),(unsigned long long)atomic_load(&b->gil_max),atomic_load(&b->rows),atomic_load(&b->lost),(unsigned long long)atomic_load(&b->tag_max));fclose(f);
 }
 f=fopen(DIR "ffrt-stall-attribution.csv","a");if(f){
  fseek(f,0,SEEK_END);if(!ftell(f))fputs("pid,phase,kind,point,tid,begin_ns,end_ns,wait_ns,recent_guest_rate_limited\n",f);
  unsigned count=atomic_load(&b->rows);for(unsigned i=0;i<count&&i<FFRT_ROWS;i++){struct ffrt_row *r=&b->data[i];if(!atomic_load_explicit(&r->ready,memory_order_acquire))continue;
   fprintf(f,"%d,%d,%u,%u,%ld,%llu,%llu,%llu,%s\n",getpid(),mode,r->kind,r->point,r->tid,(unsigned long long)r->begin,(unsigned long long)r->end,(unsigned long long)r->wait,*r->guest?r->guest:"unknown");}fclose(f);
 }
 f=fopen(DIR "ffrt-stall-io.csv","a");if(f){
  fseek(f,0,SEEK_END);if(!ftell(f))fputs("pid,phase,second,mono_ns,connect,receive,send,file_open\n",f);
  for(unsigned i=0;i<FFRT_SECONDS;i++){uint64_t n[4];for(unsigned k=0;k<4;k++)n[k]=atomic_load(&b->io[i][k]);
   if(n[0]||n[1]||n[2]||n[3])fprintf(f,"%d,%d,%u,%llu,%llu,%llu,%llu,%llu\n",getpid(),mode,i,(unsigned long long)(atomic_load(&b->start)+(uint64_t)i*1000000000),(unsigned long long)n[0],(unsigned long long)n[1],(unsigned long long)n[2],(unsigned long long)n[3]);}fclose(f);
 }
}
static void *ffrt_worker(void *unused){
 (void)unused;int previous=0;unsigned used=0;
 for(;;){
  int next=read_flag("ffrt-stall-phase",0);if(next<0||next>3)next=0;
  /* A phase is single-use per PID, so resetting never races a producer. */
  if(next!=previous&&next&&(used&(1u<<next)))next=previous;
  if(next!=previous){
   if(next){used|=1u<<next;atomic_store(&ffrt_banks[next].start,perf_now());}
   atomic_store(&ffrt_mode,next);
   extern void perf_diag_pause(int);perf_diag_pause(next==1);
   if(previous){struct timespec settle={0,10000000};nanosleep(&settle,NULL);ffrt_export(previous);}
   previous=next;
  }
  struct timespec pause={0,250000000};nanosleep(&pause,NULL);
 }
 return NULL;
}
static pthread_once_t ffrt_once=PTHREAD_ONCE_INIT;
static void ffrt_launch(void){
 FILE *f=fopen(DIR "ffrt-stall-phase","wx");if(f){fputs("0\n",f);fclose(f);chmod(DIR "ffrt-stall-phase",0666);}
 const char *names[]={"ffrt-stall-summary.csv","ffrt-stall-attribution.csv","ffrt-stall-io.csv"};
 for(unsigned i=0;i<3;i++){char path[256];snprintf(path,sizeof path,DIR "%s",names[i]);f=fopen(path,"w");if(f)fclose(f);}
 pthread_t t;if(!pthread_create(&t,NULL,ffrt_worker,NULL))pthread_detach(t);
}
void perf_ffrt_start(void){pthread_once(&ffrt_once,ffrt_launch);}
