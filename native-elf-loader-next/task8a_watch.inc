/* Lightweight stall observer. Never resumes/terminates threads or changes VM
 * state. SIGUSR2 only snapshots registers into a nonblocking pipe; unwinding
 * and file IO run on the observer, not inside the target signal handler. */
#include <stdatomic.h>
#include <signal.h>
#include <ucontext.h>
#include <sys/uio.h>
#include <fcntl.h>
#include <dlfcn.h>
#include "wait_context.h"
static _Atomic uint64_t task8a_progress[3];
static _Atomic int task8a_tids[3];
static _Atomic(const char *) task8a_lifecycle_stage;
static atomic_int task8a_watch_started;
static _Atomic(const char *) task8a_native_method;
static atomic_int task8a_native_stage;
struct task8a_recent_event { _Atomic uint64_t stamp; _Atomic int tid,stage; _Atomic(const char *) name; };
static struct task8a_recent_event task8a_recent[32];
static atomic_uint task8a_recent_cursor;
static int task8a_pipe[2]={-1,-1};
struct task8a_registers {int tid;uintptr_t pc,lr,sp,fp;};
static uint64_t task8a_watch_ms(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000;}
static void task8a_beat(unsigned role){atomic_store(&task8a_tids[role],(int)syscall(SYS_gettid));atomic_store(&task8a_progress[role],task8a_watch_ms());}
void unity_player_probe_native_phase(const char *cls,const char *method,int stage){
 if(strcmp(cls,"com/unity3d/player/UnityPlayer"))return;
 unsigned slot=atomic_fetch_add(&task8a_recent_cursor,1)%32;
 atomic_store(&task8a_recent[slot].stamp,0);atomic_store(&task8a_recent[slot].tid,(int)syscall(SYS_gettid));atomic_store(&task8a_recent[slot].name,method);atomic_store(&task8a_recent[slot].stage,stage);atomic_store(&task8a_recent[slot].stamp,task8a_watch_ms());
 if(!strcmp(method,"nativeRender")){atomic_store(&task8a_native_method,method);atomic_store(&task8a_native_stage,stage);task8a_beat(1);}
}
__attribute__((visibility("default"))) void sdk_task8a_main_heartbeat(void){task8a_beat(0);}
static void task8a_stack_signal(int signo,siginfo_t *info,void *context){
 (void)signo;(void)info;int saved=errno;struct task8a_registers r={.tid=(int)syscall(SYS_gettid)};
#if defined(__aarch64__)
 ucontext_t *uc=context;r.pc=uc->uc_mcontext.pc;r.lr=uc->uc_mcontext.regs[30];r.sp=uc->uc_mcontext.sp;r.fp=uc->uc_mcontext.regs[29];
#elif defined(__x86_64__)
 ucontext_t *uc=context;r.pc=uc->uc_mcontext.gregs[REG_RIP];r.sp=uc->uc_mcontext.gregs[REG_RSP];r.fp=uc->uc_mcontext.gregs[REG_RBP];
#endif
 if(task8a_pipe[1]>=0)(void)write(task8a_pipe[1],&r,sizeof r);errno=saved;
}
static void task8a_frame(FILE *f,unsigned index,uintptr_t pc){
 /* PAC-bearing saved LRs are never executed here. Strip only for diagnostic
  * symbol lookup after confirming the candidate lies in an executable map. */
 uintptr_t raw_pc=pc,candidate=pc&UINT64_C(0x0000ffffffffffff);
 FILE *verify=fopen("/proc/self/maps","r");char verify_line[768];
 if(verify){while(fgets(verify_line,sizeof verify_line,verify)){unsigned long long lo=0,hi=0;char prot[8]={0};if(sscanf(verify_line,"%llx-%llx %7s",&lo,&hi,prot)==3&&strchr(prot,'x')&&candidate>=lo&&candidate<hi){pc=candidate;break;}}fclose(verify);}
 Dl_info d={0};int found=dladdr((void*)pc,&d);
 fprintf(f,"TASK8A_STACK frame=%u pc=%p raw_pc=%p symbol=%s symbol_delta=0x%llx",index,(void*)pc,(void*)raw_pc,found&&d.dli_sname?d.dli_sname:"unknown",(unsigned long long)(found&&d.dli_saddr?pc-(uintptr_t)d.dli_saddr:0));
 FILE *maps=fopen("/proc/self/maps","r");char line[768];
 if(maps){while(fgets(line,sizeof line,maps)){unsigned long long lo=0,hi=0,off=0;char prot[8]={0},name[512]={0};if(sscanf(line,"%llx-%llx %7s %llx %*s %*s %511[^\n]",&lo,&hi,prot,&off,name)>=4&&pc>=lo&&pc<hi){fprintf(f," module=%s file_offset=0x%llx map_delta=0x%llx",*name?name:"anonymous",off+pc-lo,(unsigned long long)pc-lo);break;}}fclose(maps);}fputc('\n',f);
}
static void task8a_capture_impl(FILE *f,int tid){
 /* Observer-side only: record the kernel wait channel before interrupting the
  * target for its register snapshot. A zero value can mean restricted access. */
 char wait_path[128],wait_channel[128]={0};
 snprintf(wait_path,sizeof wait_path,"/proc/self/task/%d/wchan",tid);
 FILE *wait_file=fopen(wait_path,"r");
 if(wait_file){if(!fgets(wait_channel,sizeof wait_channel,wait_file))wait_channel[0]=0;fclose(wait_file);}
 wait_channel[strcspn(wait_channel,"\r\n")]=0;
 fprintf(f,"TASK8A_WCHAN tid=%d value=%s zero_is_inconclusive=1\n",tid,*wait_channel?wait_channel:"unavailable");
 if(task8a_pipe[0]<0){fprintf(f,"TASK8A_STACK tid=%d unavailable=signal_in_use_or_pipe_failed\n",tid);return;}
 struct task8a_registers r;while(read(task8a_pipe[0],&r,sizeof r)>0){}
 char proc_path[128],status_line[256];
 snprintf(proc_path,sizeof proc_path,"/proc/self/task/%d/status",tid);
 FILE *status=fopen(proc_path,"r");int status_errno=status?0:errno;
 if(status){while(fgets(status_line,sizeof status_line,status))if(!strncmp(status_line,"State:",6)||!strncmp(status_line,"SigBlk:",7)||!strncmp(status_line,"SigPnd:",7))fprintf(f,"TASK8A_THREAD tid=%d %s",tid,status_line);fclose(status);}
 struct sigaction disposition={0};sigaction(SIGUSR2,NULL,&disposition);
 int owned=disposition.sa_sigaction==task8a_stack_signal;
 errno=0;int rc=owned?(int)syscall(SYS_tgkill,getpid(),tid,SIGUSR2):-1;int send_errno=owned?errno:EBUSY;int got=0;
 for(unsigned n=0;!rc&&n<20;n++){if(read(task8a_pipe[0],&r,sizeof r)==sizeof r&&r.tid==tid){got=1;break;}struct timespec wait={0,10000000};nanosleep(&wait,NULL);}
 struct perf_wait_context checkpoint={0};int fallback=0;
 if(!got){
  fprintf(f,"TASK8A_STACK tid=%d signal_received=0 send_rc=%d send_errno=%d proc_errno=%d signal_owned=%d reason=%s\n",tid,rc,send_errno,status_errno,owned,send_errno==ESRCH?"thread_exited_or_stale_tid":rc==0?"blocked_or_delivery_timeout":"send_failed");
  snprintf(proc_path,sizeof proc_path,"/proc/self/task/%d/stack",tid);FILE *kernel=fopen(proc_path,"r");
  if(kernel){while(fgets(status_line,sizeof status_line,kernel))fprintf(f,"TASK8A_KERNEL_STACK tid=%d %s",tid,status_line);if(ferror(kernel))fprintf(f,"TASK8A_KERNEL_STACK tid=%d read_errno=%d\n",tid,errno);fclose(kernel);}else fprintf(f,"TASK8A_KERNEL_STACK tid=%d open_errno=%d\n",tid,errno);
  if(send_errno==ESRCH||!perf_wait_get(tid,&checkpoint)){fprintf(f,"TASK8A_STACK tid=%d unavailable=no_live_context dead_threads_cannot_be_unwound=1\n",tid);return;}
  r=(struct task8a_registers){.tid=tid,.pc=checkpoint.pc,.lr=0,.fp=checkpoint.fp,.sp=checkpoint.sp};fallback=1;
  fprintf(f,"TASK8A_WAIT_CONTEXT tid=%d kind=%d cond=%p mutex=%p start_ns=%llu generation=%llu source=wait_entry_not_current_pc\n",tid,checkpoint.kind,(void*)checkpoint.condition,(void*)checkpoint.mutex,(unsigned long long)checkpoint.start_ns,(unsigned long long)checkpoint.generation);
 }
 fprintf(f,"TASK8A_STACK tid=%d fp=%p sp=%p current_context=%d wait_checkpoint=%d\n",tid,(void*)r.fp,(void*)r.sp,!fallback,fallback);task8a_frame(f,0,r.pc);if(r.lr)task8a_frame(f,1,r.lr);
 uintptr_t fp=r.fp;unsigned count=2;
 for(;count<20&&fp>=r.sp&&fp-r.sp<8*1024*1024&&(fp&15)==0;count++){
  uintptr_t pair[2];struct iovec a={pair,sizeof pair},b={(void*)fp,sizeof pair};
#ifdef SYS_process_vm_readv
  if(syscall(SYS_process_vm_readv,getpid(),&a,1UL,&b,1UL,0UL)!=(long)sizeof pair)break;
#else
  break;
#endif
  if(pair[0]<=fp||!pair[1])break;task8a_frame(f,count,pair[1]);fp=pair[0];
 }
 fprintf(f,"TASK8A_STACK tid=%d frames=%u bounded=20 no_unsafe_read_fallback=1\n",tid,count);
 if(fallback){struct perf_wait_context after={0};fprintf(f,"TASK8A_WAIT_CONTEXT tid=%d stable_during_unwind=%d\n",tid,perf_wait_get(tid,&after)&&after.generation==checkpoint.generation);}
}
static pthread_mutex_t task8a_capture_lock=PTHREAD_MUTEX_INITIALIZER;
static void task8a_capture(FILE *f,int tid){pthread_mutex_lock(&task8a_capture_lock);task8a_capture_impl(f,tid);pthread_mutex_unlock(&task8a_capture_lock);}
void unity_player_probe_gil_stack_dump(FILE *f,int tid){if(tid>0)task8a_capture(f,tid);}
static void *task8a_watch_thread(void *unused){
 (void)unused;extern int sdk_app_vm_should_destroy(void),sdk_app_vm_is_foreground(void);
 const char *roles[]={"ArkTS_main","UnityMain","VM_owner"};uint64_t reported[3]={0},foreground_since=0;int was_foreground=0;
 struct sigaction previous={0},action={0};int signal_owned=0;
 if(!sigaction(SIGUSR2,NULL,&previous)&&(previous.sa_handler==SIG_DFL||previous.sa_handler==SIG_IGN)&&!pipe2(task8a_pipe,O_NONBLOCK|O_CLOEXEC)){
  action.sa_sigaction=task8a_stack_signal;action.sa_flags=SA_SIGINFO|SA_RESTART;sigemptyset(&action.sa_mask);signal_owned=!sigaction(SIGUSR2,&action,NULL);
 }
 if(!signal_owned){if(task8a_pipe[0]>=0)close(task8a_pipe[0]);if(task8a_pipe[1]>=0)close(task8a_pipe[1]);task8a_pipe[0]=task8a_pipe[1]=-1;}
 while(!sdk_app_vm_should_destroy()){
  if(perf_ffrt_mode()==1){struct timespec pause={0,250000000};nanosleep(&pause,NULL);continue;}
  uint64_t now=task8a_watch_ms();
  int foreground=sdk_app_vm_is_foreground();
  if(foreground&&!was_foreground){foreground_since=now;memset(reported,0,sizeof reported);}
  was_foreground=foreground;
  if(foreground)for(unsigned role=0;role<3;role++){
   uint64_t progress=atomic_load(&task8a_progress[role]);int tid=atomic_load(&task8a_tids[role]);
   uint64_t baseline=progress>foreground_since?progress:foreground_since;
   /* Another thread can publish a heartbeat after this tick sampled now.
    * Do not turn that newer timestamp into an unsigned-wrap false stall. */
   if(progress&&tid&&now>=baseline&&now-baseline>2000&&reported[role]!=progress){
    reported[role]=progress;FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/task8a-watchdog.txt","a");
    if(f){const char *stage=atomic_load(&task8a_lifecycle_stage);fprintf(f,"TASK8A_WATCH pid=%d mono_ms=%llu wall=%lld role=%s tid=%d stalled_ms=%llu heartbeat_age_ms=%llu foreground_since_ms=%llu lifecycle=%s submissions=%llu one_dump_per_stall=1\n",getpid(),(unsigned long long)now,(long long)time(NULL),roles[role],tid,(unsigned long long)(now-baseline),(unsigned long long)(now-progress),(unsigned long long)foreground_since,stage?stage:"none",egl_window_runtime_successful_submissions());task8a_capture(f,tid);
     DIR *tasks=opendir("/proc/self/task");struct dirent *entry;unsigned dependencies=0;
     while(tasks&&(entry=readdir(tasks))&&dependencies<8){int dependency=atoi(entry->d_name);if(dependency<=0||dependency==tid)continue;char path[128],name[64]={0};snprintf(path,sizeof path,"/proc/self/task/%d/comm",dependency);FILE *comm=fopen(path,"r");if(comm){fgets(name,sizeof name,comm);fclose(comm);}if(!strncmp(name,"UnityGfxDeviceW",14)||!strncmp(name,"UnityMain",9)){fprintf(f,"TASK8A_DEPENDENCY tid=%d name=%s",dependency,name);task8a_capture(f,dependency);dependencies++;}}
     if(tasks)closedir(tasks);
     extern size_t vm_arena_used(void),vm_arena_limit(void),vm_arena_denied(void);
     fprintf(f,"TASK8A_ARENA used=%zu limit=%zu denied=%zu\n",vm_arena_used(),vm_arena_limit(),vm_arena_denied());
     fprintf(f,"TASK8A_NATIVE method=%s stage=%d legend=1:before_native,2:native_return_before_gil,3:gil_restored\n",atomic_load(&task8a_native_method)?atomic_load(&task8a_native_method):"none",atomic_load(&task8a_native_stage));
     unsigned cursor=atomic_load(&task8a_recent_cursor),count=cursor<32?cursor:32;for(unsigned n=0;n<count;n++){struct task8a_recent_event *e=&task8a_recent[(cursor-count+n)%32];uint64_t stamp=atomic_load(&e->stamp);const char *name=atomic_load(&e->name);if(stamp)fprintf(f,"TASK8A_RECENT mono_ms=%llu tid=%d method=%s stage=%d\n",(unsigned long long)stamp,atomic_load(&e->tid),name?name:"none",atomic_load(&e->stage));}
     extern void dvm_lifetime_signal_dump(int);fflush(f);dvm_lifetime_signal_dump(fileno(f));fclose(f);}
   }
  }
  struct timespec wait={0,250000000};nanosleep(&wait,NULL);
 }
 if(signal_owned)sigaction(SIGUSR2,&previous,NULL);return NULL;
}
static void task8a_watch_start(void){if(!atomic_exchange(&task8a_watch_started,1)){pthread_t t;if(!pthread_create(&t,NULL,task8a_watch_thread,NULL))pthread_detach(t);}}
