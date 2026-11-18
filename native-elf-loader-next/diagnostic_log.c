/* Diagnostic-only asynchronous writer. Business files and reads pass through.
 * No guest APIs, waits, disk IO or blocking mutex in producer writes. */
#define _GNU_SOURCE
#define PERF_DIAG_IMPLEMENTATION
#include "diagnostic_log.h"
#include "frame_block_probe.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <stdarg.h>
#include <dirent.h>
#include <ctype.h>
#ifdef LUNARIA_SOCKET_COUNTERS
#include "mbedtls/aes.h"
#endif
#define DIAG_FILES 256
#define DIAG_QUEUE 2048
#define DIAG_CHUNK 4096
struct diag_file {char path[512];atomic_size_t position;char header[512];size_t header_length;};
struct diag_row {unsigned file,length;int truncate;char bytes[DIAG_CHUNK];};
static struct diag_file files[DIAG_FILES];static unsigned file_count,head,tail;
static struct diag_row rows[DIAG_QUEUE];static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static atomic_int running,busy;static atomic_uint_fast64_t lost,max_producer_ns;
static atomic_uint_fast64_t producer_budget_ns=50000,budget_dropped,budget_ns_max;
static atomic_int noisy_enabled;
static atomic_int paused;
static atomic_int net_enabled;
static void net_meta(const char *fmt,...){va_list args;va_start(args,fmt);perf_diag_net_vlog(fmt,args);va_end(args);}
void perf_diag_pause(int yes){atomic_store(&paused,yes!=0);}
static atomic_flag start_once=ATOMIC_FLAG_INIT;
static char root[512];static size_t limit=1024*1024; /* worker-owned */
static int connectivity_diag(const char *path){const char *base=strrchr(path,'/');return base&&!strcmp(base+1,"connectivity-status.txt");}
static int task6j_diag(const char *path){const char *base=strrchr(path,'/');return base&&!strcmp(base+1,"task6j-diagnosis.txt");}
/* Heap accounting exports contain counters, not guest payloads. */
static int heap_accounting_diag(const char *path){const char *base=strrchr(path,'/');base=base?base+1:path;return !strcmp(base,"vm-memory.txt")||!strcmp(base,"mem-leak-curve.csv");}
static int shared_net_diag(const char *path){const char *base=strrchr(path,'/');return heap_accounting_diag(path)||task6j_diag(path)||connectivity_diag(path)||(base&&(!strcmp(base+1,"net-room-network.txt")||!strcmp(base+1,"unity-player-constructor.txt")||!strcmp(base+1,"login-trace-roll.txt")||!strcmp(base+1,"vm-lifetime.txt")||!strcmp(base+1,"sdk-init-state.txt")||!strcmp(base+1,"unity-native-fault.txt")||!strcmp(base+1,"config-network.txt")||!strcmp(base+1,"strncpy-trace.txt")));}
struct diag_cookie {unsigned file;off_t position;FILE *stream;struct diag_cookie *next;unsigned closing,close_seen,quarantined;};
static struct diag_cookie *cookies,*cookie_quarantine;
/* Independent registry lock; never held across libc or queue callbacks. */
static pthread_mutex_t cookie_lock=PTHREAD_MUTEX_INITIALIZER;
#define DIAG_COOKIE_CAP 256u
static atomic_uint cookie_live,cookie_quarantined,cookie_rejected,cookie_missing_close;
static int cookie_reserve(void){
 pthread_mutex_lock(&cookie_lock);unsigned n=atomic_load(&cookie_live);
 if(n>=DIAG_COOKIE_CAP){atomic_fetch_add(&cookie_rejected,1);pthread_mutex_unlock(&cookie_lock);atomic_fetch_add(&lost,1);return 0;}
 atomic_fetch_add(&cookie_live,1);pthread_mutex_unlock(&cookie_lock);return 1;
}
static void cookie_unlink(struct diag_cookie *c){struct diag_cookie **p=&cookies;while(*p&&*p!=c)p=&(*p)->next;if(*p)*p=c->next;}

static uint64_t diag_now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000+t.tv_nsec;}
static void diag_max(atomic_uint_fast64_t *value,uint64_t sample){uint_fast64_t previous=atomic_load_explicit(value,memory_order_relaxed);while(sample>previous&&!atomic_compare_exchange_weak_explicit(value,&previous,sample,memory_order_relaxed,memory_order_relaxed)){} }
/* Cooperative CPU-work deadline, not a real-time scheduling guarantee: a
 * preempted producer can resume after this wall deadline. Never wait for IO. */
static int diag_budget_expired(uint64_t start,size_t remaining){
 if(diag_now()-start<atomic_load_explicit(&producer_budget_ns,memory_order_relaxed))return 0;
 uint64_t dropped=remaining/DIAG_CHUNK+(remaining%DIAG_CHUNK!=0);if(!dropped)dropped=1;
 atomic_fetch_add_explicit(&budget_dropped,dropped,memory_order_relaxed);atomic_fetch_add_explicit(&lost,dropped,memory_order_relaxed);return 1;
}
#include "socket_counters.inc"
static void enqueue(unsigned file,const char*b,size_t n,int truncate){
 if(atomic_load(&paused))return;
 if(pthread_mutex_trylock(&lock)){atomic_fetch_add(&lost,1);return;}
 unsigned next=(tail+1)%DIAG_QUEUE;if(next==head){atomic_fetch_add(&lost,1);pthread_mutex_unlock(&lock);return;}
 rows[tail].file=file;rows[tail].length=(unsigned)n;rows[tail].truncate=truncate;if(n)memcpy(rows[tail].bytes,b,n);tail=next;
 pthread_mutex_unlock(&lock);
}
/* Default-on, bounded metadata only. No fopen/stat/chmod on the caller thread.
 * Uses the existing 2048-slot worker queue; contention drops diagnostics only. */
void perf_diag_connectivity(int value,int registered,const char *reason){FRAME_DIAG_SCOPE;
 int saved=errno;uint64_t start=diag_now();
 if(!atomic_load(&running)||pthread_mutex_trylock(&lock)){errno=saved;return;}
 char path[512];snprintf(path,sizeof path,"%s/connectivity-status.txt",root);
 unsigned i;for(i=0;i<file_count;i++)if(!strcmp(files[i].path,path))break;
 if(i==file_count&&file_count<DIAG_FILES){snprintf(files[i].path,sizeof files[i].path,"%s",path);file_count++;}
 unsigned next=(tail+1)%DIAG_QUEUE;
 if(i>=DIAG_FILES||next==head){atomic_fetch_add(&lost,1);pthread_mutex_unlock(&lock);errno=saved;return;}
 struct diag_row *r=&rows[tail];r->file=i;r->truncate=0;
 r->length=(unsigned)snprintf(r->bytes,sizeof r->bytes,
  "pid=%d mono_ms=%llu tryGetNetworkConnectivity=%d registered=%d snapshot_source=Index.replyNetwork/NetworkSnapshot reason=%s\n",
  getpid(),(unsigned long long)(diag_now()/1000000),value,registered,reason);
 if(!diag_budget_expired(start,r->length))tail=next;
 pthread_mutex_unlock(&lock);diag_max(&budget_ns_max,diag_now()-start);errno=saved;
}
void perf_diag_connectivity_detail(const char *stage,const char *detail){FRAME_DIAG_SCOPE;
 int saved=errno;uint64_t start=diag_now();
 if(!atomic_load(&running)||pthread_mutex_trylock(&lock)){errno=saved;return;}
 char path[512];snprintf(path,sizeof path,"%s/connectivity-status.txt",root);
 unsigned i;for(i=0;i<file_count;i++)if(!strcmp(files[i].path,path))break;
 if(i==file_count&&file_count<DIAG_FILES){snprintf(files[i].path,sizeof files[i].path,"%s",path);file_count++;}
 unsigned next=(tail+1)%DIAG_QUEUE;
 if(i>=DIAG_FILES||next==head){atomic_fetch_add(&lost,1);pthread_mutex_unlock(&lock);errno=saved;return;}
 struct diag_row *r=&rows[tail];r->file=i;r->truncate=0;
 r->length=(unsigned)snprintf(r->bytes,sizeof r->bytes,"pid=%d mono_ms=%llu stage=%.64s detail=%.768s\n",getpid(),(unsigned long long)(diag_now()/1000000),stage?stage:"unknown",detail?detail:"-");
 /* Line structure cannot be injected by a diagnostic message. */
 for(unsigned n=0;n+1<r->length;n++)if(r->bytes[n]=='\n'||r->bytes[n]=='\r')r->bytes[n]=' ';
 if(!diag_budget_expired(start,r->length))tail=next;
 pthread_mutex_unlock(&lock);diag_max(&budget_ns_max,diag_now()-start);errno=saved;
}
static ssize_t diag_write(void*data,const char*b,size_t n){FRAME_DIAG_SCOPE;
 int saved=errno;uint64_t start=diag_now();struct diag_cookie*c=data;size_t original=n;
 while(n){if(diag_budget_expired(start,n))break;size_t chunk=n>DIAG_CHUNK?DIAG_CHUNK:n;enqueue(c->file,b,chunk,0);b+=chunk;n-=chunk;}
 c->position+=(off_t)original;atomic_fetch_add(&files[c->file].position,original);
 uint64_t duration=diag_now()-start;diag_max(&max_producer_ns,duration);diag_max(&budget_ns_max,duration);errno=saved;
 return (ssize_t)original; /* dropped diagnostics never fail business work */
}
static int diag_seek(void*data,off_t*off,int whence){struct diag_cookie*c=data;off_t base=whence==SEEK_SET?0:whence==SEEK_CUR?c->position:(off_t)atomic_load(&files[c->file].position);if(base+*off<0){errno=EINVAL;return -1;}c->position=base+*off;*off=c->position;return 0;}
static int diag_close(void*data){
 struct diag_cookie*c=data;int release=0;pthread_mutex_lock(&cookie_lock);
 if(!c->close_seen){c->close_seen=1;cookie_unlink(c);
  /* Quarantined cookies stay alive even if libc calls back much later. */
  if(!c->closing&&!c->quarantined){atomic_fetch_sub(&cookie_live,1);release=1;}}
 pthread_mutex_unlock(&cookie_lock);if(release)free(c);return 0;
}
int perf_diag_fclose(FILE*f){
 pthread_mutex_lock(&cookie_lock);struct diag_cookie*c=cookies;while(c&&c->stream!=f)c=c->next;
 if(c){cookie_unlink(c);c->closing=1;}pthread_mutex_unlock(&cookie_lock);
 if(!c)return fclose(f);
 /* The stream is removed before libc can recycle its FILE address. */
 int rc=fclose(f),saved=errno,release=0;pthread_mutex_lock(&cookie_lock);
 c->closing=0;
 if(c->close_seen){atomic_fetch_sub(&cookie_live,1);release=1;}
 else{c->quarantined=1;c->next=cookie_quarantine;cookie_quarantine=c;atomic_fetch_add(&cookie_quarantined,1);atomic_fetch_add(&cookie_missing_close,1);}
 pthread_mutex_unlock(&cookie_lock);if(release)free(c);errno=saved;return rc;
}
int perf_diag_fileno(FILE*f){
 pthread_mutex_lock(&cookie_lock);struct diag_cookie*c=cookies;while(c&&c->stream!=f)c=c->next;
 int fd=c?-(int)c->file-10000:0;pthread_mutex_unlock(&cookie_lock);
 return c?fd:fileno(f); /* No FILE operation under registry/queue lock. */
}
ssize_t perf_diag_write(int fd,const void*b,size_t n){if(fd>-10000||fd<=-10000-DIAG_FILES)return write(fd,b,n);unsigned i=(unsigned)(-fd-10000);struct diag_cookie c={.file=i};return diag_write(&c,b,n);}
int perf_diag_vdprintf(int fd,const char*fmt,va_list ap){FRAME_PROBE_SCOPE(fd<=-10000&&fd>-10000-DIAG_FILES?PERF_BLOCK_DIAG:0);int n;if(fd<=-10000&&fd>-10000-DIAG_FILES){char b[8192];n=vsnprintf(b,sizeof b,fmt,ap);if(n>0)perf_diag_write(fd,b,(size_t)n<sizeof b?(size_t)n:sizeof b-1);}else n=vdprintf(fd,fmt,ap);return n;}
int perf_diag_dprintf(int fd,const char*fmt,...){va_list ap;va_start(ap,fmt);int n=perf_diag_vdprintf(fd,fmt,ap);va_end(ap);return n;}
int perf_diag_fsync(int fd){FRAME_PROBE_SCOPE(fd<=-10000&&fd>-10000-DIAG_FILES?PERF_BLOCK_DIAG:PERF_BLOCK_FILE);if(fd>-10000||fd<=-10000-DIAG_FILES)return fsync(fd);int saved=errno;enqueue((unsigned)(-fd-10000),NULL,0,-1);errno=saved;return 0;}
static int selected(const char*path){
 if(!*root)return 0;size_t prefix=strlen(root);if(strncmp(path,root,prefix)||path[prefix]!='/')return 0;
 const char*name=path+prefix+1;if(strchr(name,'/'))return 0;
 /* Generated allowlist: only diagnostic filenames actually used by our code. */
 static const char*names[]={
#include "diagnostic_log_names.inc"
 };
 for(unsigned i=0;i<sizeof names/sizeof names[0];i++)if(!strcmp(name,names[i]))return 1;return 0;
}
/* Bounded capture exports are written once by the observer, after recording.
 * Keep full CSVs out of both the lossy cookie queue and generic tail rotation. */
static int capture_export(const char *path){
 if(!*root||strncmp(path,root,strlen(root))||path[strlen(root)]!='/')return 0;
 const char *name=path+strlen(root)+1;
 return !strcmp(name,"long-frame-breakdown.csv")||!strcmp(name,"play-stall-frames.csv")||!strcmp(name,"play-stall-samples.csv")||
        !strcmp(name,"play-stall-status.txt")||!strcmp(name,"result-black-events.csv")||
        !strcmp(name,"ffrt-stall-summary.csv")||!strcmp(name,"ffrt-stall-attribution.csv")||!strcmp(name,"ffrt-stall-io.csv");
}
static int noisy_diagnostic(const char *path){
 const char *name=strrchr(path,'/');name=name?name+1:path;
 /* Startup acceptance tests are required even when their feature trace is off. */
 if(strstr(name,"selftest"))return 0;
 return !strcmp(name,"engine-probe.txt")||!strcmp(name,"login-trace-roll.txt")||!strcmp(name,"strncpy-trace.txt")||
        !strncmp(name,"task6j-",7)||!strncmp(name,"jitter-",7)||!strncmp(name,"mem-escape",10)||
        !strncmp(name,"enqueue-drain",13)||!strncmp(name,"freeze180",9)||!strncmp(name,"gil-deep-trace",14);
}
FILE *perf_diag_fopen(const char*path,const char*mode){FRAME_PROBE_SCOPE(path&&mode&&mode[0]!='r'&&(selected(path)||capture_export(path))?PERF_BLOCK_DIAG:PERF_BLOCK_FILE);
 if(path&&mode&&capture_export(path)&&(!strcmp(mode,"w")||!strcmp(mode,"a"))){
  int fd=open(path,O_WRONLY|O_CREAT|(mode[0]=='w'?O_TRUNC:O_APPEND)|O_CLOEXEC|O_NOFOLLOW,0644);
  if(fd<0)return NULL;
  if(fchmod(fd,0644)){int saved=errno;close(fd);errno=saved;return NULL;}
  FILE *f=fdopen(fd,mode);if(!f){int saved=errno;close(fd);errno=saved;}return f;
 }
 if(path&&mode&&mode[0]!='r'&&selected(path)&&noisy_diagnostic(path)&&!atomic_load_explicit(&noisy_enabled,memory_order_relaxed))return NULL;
 if(path&&mode&&mode[0]!='r'&&selected(path)&&atomic_load(&paused))return NULL;
 if(!atomic_load(&running)||!path||!mode||strchr(mode,'+')||(mode[0]!='a'&&mode[0]!='w')||!selected(path)){
  FILE *f=fopen(path,mode);
  if(f&&path&&mode&&mode[0]!='r'&&heap_accounting_diag(path)&&fchmod(fileno(f),0644)){int saved=errno;fclose(f);errno=saved;return NULL;}
  return f;
 }
 /* Bounded startup acceptance records must not be lossy cookies. HarmonyOS
  * can return -EBADF from fclose(cookie) despite successful writes/flush.
  * These files are written by explicit startup tests, not frame callbacks;
  * the worker still enforces the normal size cap via selected()/trim_legacy. */
 const char*base=strrchr(path,'/');base=base?base+1:path;
 if(strstr(base,"selftest")||!strcmp(base,"gc-perf.csv"))return fopen(path,mode);
 /* Fixed registry lookup, no file IO. A contended diagnostic open is dropped. */
 if(pthread_mutex_trylock(&lock)){atomic_fetch_add(&lost,1);return NULL;}
 unsigned i;for(i=0;i<file_count;i++)if(!strcmp(files[i].path,path))break;
 if(i==file_count&&file_count<DIAG_FILES){snprintf(files[i].path,sizeof files[i].path,"%s",path);file_count++;}
 pthread_mutex_unlock(&lock);if(i>=DIAG_FILES){atomic_fetch_add(&lost,1);return NULL;}
 if(!cookie_reserve())return NULL;struct diag_cookie*c=calloc(1,sizeof*c);if(!c){atomic_fetch_sub(&cookie_live,1);return NULL;}c->file=i;c->position=(off_t)atomic_load(&files[i].position);
 if(mode[0]=='w'){enqueue(i,NULL,0,1);atomic_store(&files[i].position,0);c->position=0;}
 FILE*f=fopencookie(c,"w",(cookie_io_functions_t){.write=diag_write,.seek=diag_seek,.close=diag_close});if(!f){atomic_fetch_sub(&cookie_live,1);free(c);}else{setvbuf(f,NULL,_IONBF,0);c->stream=f;pthread_mutex_lock(&cookie_lock);c->next=cookies;cookies=c;pthread_mutex_unlock(&cookie_lock);}return f;
}
static void apply_row(const struct diag_row*r){FRAME_DIAG_SCOPE;
 uint64_t started=diag_now();
 const char*path=files[r->file].path;struct stat st={0};stat(path,&st);
 if(r->truncate==-1){int fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(fd>=0){(void)fsync(fd);close(fd);}return;}
 size_t file_limit=connectivity_diag(path)?1024u*1024u:limit;
 struct diag_file*entry=&files[r->file];int rotated=0;
 if(r->truncate)entry->header_length=0;
 if(!entry->header_length&&strstr(path,".csv")&&r->length&&((r->bytes[0]>='A'&&r->bytes[0]<='Z')||(r->bytes[0]>='a'&&r->bytes[0]<='z'))){const char*nl=memchr(r->bytes,'\n',r->length);size_t n=nl?(size_t)(nl-r->bytes)+1:0;if(n&&n<sizeof entry->header&&memchr(r->bytes,',',n)){memcpy(entry->header,r->bytes,n);entry->header_length=n;}}
 if((size_t)st.st_size+r->length>file_limit){char backup[544];snprintf(backup,sizeof backup,"%s.1",path);unlink(backup);rename(path,backup);if(shared_net_diag(path)){int saved=open(backup,O_WRONLY|O_CLOEXEC|O_NOFOLLOW);if(saved>=0){(void)fchmod(saved,0644);close(saved);}}st.st_size=0;rotated=1;}
 /* Oversized legacy diagnostics are removed, never retained as a huge .1. */
 char backup[544];snprintf(backup,sizeof backup,"%s.1",path);struct stat old;if(!stat(backup,&old)){if((size_t)old.st_size>file_limit)unlink(backup);else if((task6j_diag(path)||heap_accounting_diag(path))&&(old.st_mode&0777)!=0644){int saved=open(backup,O_WRONLY|O_CLOEXEC|O_NOFOLLOW);if(saved<0)return;int error=fchmod(saved,0644);close(saved);if(error)return;}}
 int fd=open(path,O_WRONLY|O_CREAT|(r->truncate?O_TRUNC:O_APPEND)|O_CLOEXEC|O_NOFOLLOW,0600);if(fd<0)return;
 if(shared_net_diag(path)&&fchmod(fd,0644)){close(fd);return;}
 if(rotated&&entry->header_length)(void)write(fd,entry->header,entry->header_length);
 size_t at=0;while(at<r->length){ssize_t n=write(fd,r->bytes+at,r->length-at);if(n<0&&errno==EINTR)continue;if(n<=0)break;at+=(size_t)n;}close(fd);
 extern void perf_result_event(int,uint64_t,uint64_t) __attribute__((weak));
 if(perf_result_event)perf_result_event(21,started,diag_now());
}
static void controls(void){char path[544];snprintf(path,sizeof path,"%s/diagnostic-log-large",root);FILE*f=fopen(path,"r");int large=0;if(f){fscanf(f,"%d",&large);fclose(f);}limit=large==1?16*1024*1024:1024*1024;
 snprintf(path,sizeof path,"%s/diagnostic-producer-budget-us",root);f=fopen(path,"r");unsigned budget_us=50;if(f){unsigned configured=0;if(fscanf(f,"%u",&configured)==1&&configured>=1&&configured<=1000)budget_us=configured;fclose(f);}atomic_store_explicit(&producer_budget_ns,(uint64_t)budget_us*1000,memory_order_relaxed);
 snprintf(path,sizeof path,"%s/diagnostic-noisy-observe",root);f=fopen(path,"r");int noisy=0;if(f){fscanf(f,"%d",&noisy);fclose(f);}atomic_store_explicit(&noisy_enabled,noisy==1,memory_order_relaxed);
 snprintf(path,sizeof path,"%s/net-room-observe",root);f=fopen(path,"r");int enabled=0;if(f){fscanf(f,"%d",&enabled);fclose(f);}int before=atomic_exchange(&net_enabled,enabled==1);
 if(enabled==1&&!before)net_meta("stage=observer_enabled");
}
/* Export only fresh metadata from legacy producers on the IO worker. Their
 * private files remain private; no guest calls or payload reads are needed. */
static void net_legacy_export(void){
 static const char *names[]={"config-network.txt","task6k-network.txt"};
 static off_t positions[2];static int initialized;
 int enabled=atomic_load(&net_enabled);
 for(unsigned i=0;i<2;i++){
  char path[544];snprintf(path,sizeof path,"%s/%s",root,names[i]);
  int fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);int error=fd<0?errno:0;
  struct stat st={0};if(fd>=0&&fstat(fd,&st))error=errno;
  char status[544];snprintf(status,sizeof status,"%s/net-room-export-status.txt",root);
  int report=open(status,O_WRONLY|O_CREAT|(i?O_APPEND:O_TRUNC)|O_CLOEXEC|O_NOFOLLOW,0644);
  if(report>=0){fchmod(report,0644);dprintf(report,"pid=%d mono_ms=%llu enabled=%d source=%u errno=%d size=%lld offset=%lld\n",getpid(),(unsigned long long)(diag_now()/1000000),enabled,i,error,(long long)st.st_size,(long long)positions[i]);close(report);}
  if(error){if(fd>=0)close(fd);continue;}
  if(!initialized||!enabled){positions[i]=st.st_size;close(fd);continue;}
  if(st.st_size<positions[i])positions[i]=0;
  if(lseek(fd,positions[i],SEEK_SET)<0){close(fd);continue;}
  char buffer[65537];ssize_t n=read(fd,buffer,sizeof buffer-1);
  if(n>0){buffer[n]=0;char *line=buffer,*end;
   while((end=strchr(line,'\n'))){*end=0;positions[i]+=(off_t)(end-line)+1;
    if(strstr(line,"stage="))net_meta("%s",line);
    line=end+1;
   }
   if(line==buffer&&n==(ssize_t)sizeof buffer-1)positions[i]+=n;
  }
  close(fd);
 }
 initialized=1;
}
/* Worker-only, opt-in exception attribution. Read at most 64 KiB/tick from
 * private source, export only structural class/method names and source time.
 * Never copy message_utf16, formatted traces, payloads or object addresses. */
static void net_exception_export(void){
 static off_t position;static unsigned long long source_time;static int source_pid;
 char path[544];snprintf(path,sizeof path,"%s/jni-gaps-il2cpp-exceptions.txt",root);
 int fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(fd<0)return;
 struct stat st;if(fstat(fd,&st)){close(fd);return;}
 if(!atomic_load(&net_enabled)){position=st.st_size;close(fd);return;}
 if(st.st_size<position)position=0;
 if(lseek(fd,position,SEEK_SET)<0){close(fd);return;}
 char buffer[65537];ssize_t n=read(fd,buffer,sizeof buffer-1);close(fd);if(n<=0)return;
 buffer[n]=0;char *line=buffer,*end;
 while((end=strchr(line,'\n'))){*end=0;position+=(off_t)(end-line)+1;
  if(!strncmp(line,"IL2CPP_THROW ",13)){
   const char *t=strstr(line,"mono_ns="),*p=strstr(line,"pid=");
   source_time=t?strtoull(t+8,NULL,10):0;source_pid=p?atoi(p+4):0;
  }else if(source_pid==getpid()&&source_time){
   char klass[181]={0},method[181]={0};
   if(!strncmp(line,"IL2CPP_EXCEPTION ",17)){
    const char *p=strstr(line,"class=");if(p&&sscanf(p+6,"%180s",klass)==1)
     net_meta("stage=managed_exception source_mono_ns=%llu exception_class=%s",source_time,klass);
   }else if(!strncmp(line,"IL2CPP_CURRENT_FRAME ",21)){
    const char *p=strstr(line,"class="),*m=strstr(line,"method=");
    if(p&&m&&sscanf(p+6,"%180s",klass)==1&&sscanf(m+7,"%180s",method)==1)
     net_meta("stage=managed_frame source_mono_ns=%llu guest_class=%s guest_method=%s",source_time,klass,method);
   }
  }
  line=end+1;
 }
 if(line==buffer&&n==(ssize_t)sizeof buffer-1)position+=n;
}
/* Worker-only fixed-marker scan of already-private engine diagnostics. It
 * never exports message text, addresses, identifiers, tokens or path values. */
static void net_engine_export(void){
 static off_t position;static int initialized;char path[544];snprintf(path,sizeof path,"%s/engine-probe.txt",root);
 int fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(fd<0)return;struct stat st;if(fstat(fd,&st)){close(fd);return;}
 if(!initialized||!atomic_load(&net_enabled)){position=st.st_size;initialized=1;close(fd);return;}
 if(position>st.st_size)position=0;
 char buffer[65537];ssize_t n=pread(fd,buffer,sizeof buffer-1,position);close(fd);if(n<=0)return;buffer[n]=0;
 static const char *const markers[]={"SocketException","NotSupportedException","PlatformNotSupportedException","NullReferenceException","InvalidOperationException","CryptographicException","SerializationException","NebulaCore.SocketClientAsync","SocketClientAsync.Connect","SocketClientAsync.OnConnect","SocketClientAsync.LogError","ConnectToServer","OnFindMatchServer","FindMatchServerResult","TcpClient.Connect","TcpClient..ctor","SocketClient","connection refused","Network is unreachable","Name or service not known"};
 char *line=buffer;for(char *end;(end=strchr(line,'\n'));line=end+1){*end=0;for(unsigned i=0;i<sizeof markers/sizeof markers[0];i++)if(strstr(line,markers[i]))net_meta("stage=engine_marker guest_method=%s",markers[i]);position+=end-line+1;}
 if(line==buffer&&n==(ssize_t)sizeof buffer-1)position+=n;
}
/* Bounded response snapshots stay in memory only. The worker exports format
 * and explicitly allowlisted integral business codes; never any body text. */
#define NET_RESPONSE_MAX 8192u
static pthread_mutex_t response_lock=PTHREAD_MUTEX_INITIALIZER;
static struct {unsigned char bytes[NET_RESPONSE_MAX];size_t length,total;uint64_t ns;} response_rows[8];
static unsigned response_head,response_count;
void perf_diag_response_summary(const unsigned char *bytes,size_t length){
 if(!atomic_load_explicit(&net_enabled,memory_order_relaxed)||!bytes||!length)return;
 int saved=errno;if(pthread_mutex_trylock(&response_lock)){errno=saved;return;}
 if(response_count<8){unsigned n=(response_head+response_count++)%8;response_rows[n].length=length<NET_RESPONSE_MAX?length:NET_RESPONSE_MAX;response_rows[n].total=length;response_rows[n].ns=diag_now();memcpy(response_rows[n].bytes,bytes,response_rows[n].length);}
 pthread_mutex_unlock(&response_lock);errno=saved;
}
#ifdef LUNARIA_SOCKET_COUNTERS
/* Read-only diagnostic decoder for the verified v31 APK. Constants are read
 * from the original extracted metadata, never duplicated into logs/config.
 * No guest functions, user credentials, or protocol requests are involved. */
static int net_protocol_key(unsigned index,unsigned char out[16]){
 char path[640];snprintf(path,sizeof path,"%s/native-assets/bin/Data/Managed/Metadata/global-metadata.dat",root);
 FILE *f=fopen(path,"rb");if(!f)return 0;uint32_t h[6]={0},entry[2]={0};int ok=fread(h,sizeof h,1,f)==1&&h[0]==0xfab11baf&&h[1]==31;
 if(ok)ok=h[3]>=8u*(index+1)&&!fseek(f,(long)h[2]+8L*index,SEEK_SET)&&fread(entry,sizeof entry,1,f)==1&&entry[0]==16&&h[5]>=16&&entry[1]<=h[5]-16;
 if(ok)ok=!fseek(f,(long)h[4]+entry[1],SEEK_SET)&&fread(out,16,1,f)==1;
 fclose(f);return ok;
}
static size_t net_protocol_decode(unsigned char *bytes,size_t n){
 if(!n||n>NET_RESPONSE_MAX||n%16)return 0;unsigned char key[16]={0},iv[16]={0};
 if(!net_protocol_key(25292,key)||!net_protocol_key(16831,iv)){memset(key,0,16);return 0;}
 unsigned char plain[NET_RESPONSE_MAX];mbedtls_aes_context aes;mbedtls_aes_init(&aes);
 int rc=mbedtls_aes_setkey_dec(&aes,key,128);if(!rc)rc=mbedtls_aes_crypt_cbc(&aes,MBEDTLS_AES_DECRYPT,n,iv,bytes,plain);
 mbedtls_aes_free(&aes);memset(key,0,sizeof key);memset(iv,0,sizeof iv);
 unsigned pad=rc?0:plain[n-1];int ok=!rc&&pad&&pad<=16&&pad<=n;for(unsigned i=0;ok&&i<pad;i++)if(plain[n-1-i]!=pad)ok=0;
 size_t result=ok?n-pad:0;if(result)memcpy(bytes,plain,result);memset(plain,0,sizeof plain);return result;
}
#endif
struct net_mp_value {int kind;int64_t number;const unsigned char *text;size_t length;};
struct net_mp {const unsigned char *p,*end;unsigned nodes,fields;int has_code;int64_t code;};
static int net_mp_take(struct net_mp *m,unsigned n,uint64_t *v){if((size_t)(m->end-m->p)<n)return 0;*v=0;while(n--)*v=(*v<<8)|*m->p++;return 1;}
static int net_mp_key(struct net_mp_value *k,const char *s){return k->kind==2&&k->length==strlen(s)&&!memcmp(k->text,s,k->length);}
static int net_mp_parse(struct net_mp *m,struct net_mp_value *v,unsigned depth){
 if(depth>16||++m->nodes>4096||m->p>=m->end)return 0;memset(v,0,sizeof *v);unsigned c=*m->p++;uint64_t n=0;int map=0,array=0,string=0,binary=0;
 if(c<128){v->kind=1;v->number=c;return 1;}if(c>=224){v->kind=1;v->number=(int)c-256;return 1;}
 if((c&0xe0)==0xa0){n=c&31;string=1;}else if((c&0xf0)==0x80){n=c&15;map=1;}else if((c&0xf0)==0x90){n=c&15;array=1;}
 else switch(c){
 case 0xc0:case 0xc2:case 0xc3:return 1;
 case 0xcc:case 0xcd:case 0xce:case 0xcf:if(!net_mp_take(m,1u<<(c-0xcc),&n))return 0;v->kind=1;v->number=(int64_t)n;return 1;
 case 0xd0:case 0xd1:case 0xd2:case 0xd3:{unsigned bits=8u<<(c-0xd0);if(!net_mp_take(m,bits/8,&n))return 0;if(bits<64&&(n&(1ull<<(bits-1))))n|=~((1ull<<bits)-1);v->kind=1;v->number=(int64_t)n;return 1;}
 case 0xd9:case 0xda:case 0xdb:if(!net_mp_take(m,1u<<(c-0xd9),&n))return 0;string=1;break;
 case 0xc4:case 0xc5:case 0xc6:if(!net_mp_take(m,1u<<(c-0xc4),&n))return 0;binary=1;break;
 case 0xdc:case 0xdd:if(!net_mp_take(m,c==0xdc?2:4,&n))return 0;array=1;break;
 case 0xde:case 0xdf:if(!net_mp_take(m,c==0xde?2:4,&n))return 0;map=1;break;
 case 0xca:case 0xcb:n=c==0xca?4:8;binary=1;break;
 default:return 0;
 }
 if(string||binary){if(n>(uint64_t)(m->end-m->p))return 0;v->kind=string?2:0;v->text=m->p;v->length=(size_t)n;m->p+=n;return 1;}
 if(n>4096)return 0;
 if(map)for(uint64_t i=0;i<n;i++){struct net_mp_value key,value;if(!net_mp_parse(m,&key,depth+1)||!net_mp_parse(m,&value,depth+1))return 0;
  if(value.kind==1&&value.number>=-100000&&value.number<=100000&&(net_mp_key(&key,"code")||net_mp_key(&key,"resultCode")||net_mp_key(&key,"errorCode"))){m->has_code=1;m->code=value.number;}
  if((net_mp_key(&key,"url")||net_mp_key(&key,"host"))&&value.kind==2&&value.length)m->fields|=1;
  if(net_mp_key(&key,"port")&&value.kind==1&&value.number>0&&value.number<=65535)m->fields|=2;
  if(net_mp_key(&key,"token")&&value.kind==2&&value.length)m->fields|=4;
 }
 if(array)for(uint64_t i=0;i<n;i++){struct net_mp_value item;if(!net_mp_parse(m,&item,depth+1))return 0;}
 return 1;
}
/* Verified original CEProtoSerializer.Read 0x3b6f2d8 maps field1=url,
 * field2=uint32 port, field3=token. Only presence bits leave this worker. */
static int net_pb_varint(const unsigned char **p,const unsigned char *end,uint64_t *value){
 *value=0;for(unsigned i=0;i<10;i++){if(*p==end)return 0;unsigned c=*(*p)++;if(i==9&&c>1)return 0;*value|=(uint64_t)(c&127)<<(7*i);if(!(c&128))return 1;}return 0;
}
static int net_pb_server(const unsigned char *p,size_t length,unsigned *fields){
 const unsigned char *end=p+length;unsigned count=0;*fields=0;
 while(p<end){uint64_t tag,value;if(++count>4096||!net_pb_varint(&p,end,&tag)||!(tag>>3)||tag>>3>0x1fffffff)return 0;unsigned wire=tag&7,field=tag>>3;
  switch(wire){
  case 0:if(!net_pb_varint(&p,end,&value))return 0;if(field==2&&value>0&&value<=65535)*fields|=2;break;
  case 1:if(end-p<8)return 0;p+=8;break;
  case 2:if(!net_pb_varint(&p,end,&value)||value>(uint64_t)(end-p))return 0;if(value&&field==1)*fields|=1;if(value&&field==3)*fields|=4;p+=(size_t)value;break;
  case 5:if(end-p<4)return 0;p+=4;break;
  default:return 0;
  }
 }
 return count>0;
}
static void net_response_export(void){
 unsigned char bytes[NET_RESPONSE_MAX+1];size_t length,total;uint64_t ns;
 for(unsigned i=0;i<8;i++){
  pthread_mutex_lock(&response_lock);if(!response_count){pthread_mutex_unlock(&response_lock);break;}
  unsigned n=response_head;length=response_rows[n].length;total=response_rows[n].total;ns=response_rows[n].ns;memcpy(bytes,response_rows[n].bytes,length);memset(&response_rows[n],0,sizeof response_rows[n]);response_head=(n+1)%8;response_count--;pthread_mutex_unlock(&response_lock);bytes[length]=0;
  int decoded=0;
#ifdef LUNARIA_SOCKET_COUNTERS
  if(total==length){size_t plain=net_protocol_decode(bytes,length);if(plain){length=plain;bytes[length]=0;decoded=1;}}
#endif
  size_t first=0;while(first<length&&isspace(bytes[first]))first++;
  const char *format=first<length&&(bytes[first]=='{'||bytes[first]=='[')?"json_candidate":length>=2&&bytes[0]==31&&bytes[1]==139?"gzip":"binary";
  if(decoded&&!strcmp(format,"binary")&&length&&((bytes[0]&0xf0)==0x80||bytes[0]==0xde||bytes[0]==0xdf))format="msgpack_candidate";
  net_meta("stage=response_shape source_mono_ns=%llu bytes=%zu response_format=%s truncated=%d decoded=%d",(unsigned long long)ns,total,format,total>NET_RESPONSE_MAX,decoded);
  if(!strcmp(format,"msgpack_candidate")){
   struct net_mp m={.p=bytes,.end=bytes+length};struct net_mp_value value;int valid=net_mp_parse(&m,&value,0)&&m.p==m.end;
   net_meta("stage=response_schema source_mono_ns=%llu valid=%d server_fields=%u has_code=%d business_code=%lld",(unsigned long long)ns,valid,valid?m.fields:0,valid&&m.has_code,(long long)(valid&&m.has_code?m.code:0));
  }
  if(decoded&&!strcmp(format,"binary")){
   unsigned fields=0;int valid=net_pb_server(bytes,length,&fields);
   net_meta("stage=response_proto_schema source_mono_ns=%llu valid=%d server_fields=%u",(unsigned long long)ns,valid,valid?fields:0);
  }
  if(!strcmp(format,"json_candidate")){
   static const char *keys[]={"resultCode","errorCode","statusCode","code","result"};
   /* Only exact unescaped JSON keys followed by an integral value. No strings,
    * tokens, IDs or unknown field names can pass this output allowlist. */
   for(unsigned k=0;k<sizeof keys/sizeof keys[0];k++){
    char needle[32];snprintf(needle,sizeof needle,"\"%s\"",keys[k]);char *p=strstr((char*)bytes,needle);if(!p)continue;p+=strlen(needle);while(isspace((unsigned char)*p))p++;if(*p++!=':')continue;while(isspace((unsigned char)*p))p++;char *end;errno=0;long value=strtol(p,&end,10);if(end==p||errno||value< -100000||value>100000)continue;while(isspace((unsigned char)*end))end++;if(*end!=','&&*end!='}'&&*end!=']')continue;
    net_meta("stage=response_code source_mono_ns=%llu code_field=%s business_code=%ld",(unsigned long long)ns,keys[k],value);
   }
  }
  memset(bytes,0,sizeof bytes);
 }
}
static void trim_legacy(void){
 DIR*d=opendir(root);if(!d)return;struct dirent*e;
 while((e=readdir(d))){char p[544],base[544];snprintf(p,sizeof p,"%s/%s",root,e->d_name);snprintf(base,sizeof base,"%s",p);
  size_t n=strlen(base);if(n>2&&!strcmp(base+n-2,".1"))base[n-2]=0;
  struct stat st;if(capture_export(base)||!selected(base)||lstat(p,&st)||!S_ISREG(st.st_mode)||(size_t)st.st_size<=limit)continue;
  /* Preserve the last bounded tail. Never copy a multi-GB diagnostic. */
  char temp[560];snprintf(temp,sizeof temp,"%s.trim",p);int in=open(p,O_RDONLY|O_CLOEXEC|O_NOFOLLOW),out=open(temp,O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC|O_NOFOLLOW,0600);
  if(out>=0&&shared_net_diag(base))(void)fchmod(out,0644);
  if(in>=0&&out>=0&&lseek(in,st.st_size-(off_t)limit,SEEK_SET)>=0){char b[4096];size_t remaining=limit;int ok=1;while(remaining){ssize_t got=read(in,b,remaining<sizeof b?remaining:sizeof b);if(got<=0){ok=0;break;}ssize_t at=0;while(at<got){ssize_t written=write(out,b+at,(size_t)(got-at));if(written<=0){ok=0;break;}at+=written;}if(!ok)break;remaining-=(size_t)got;}if(ok)rename(temp,p);}
  if(in>=0)close(in);if(out>=0)close(out);unlink(temp);
 }closedir(d);
}
static void shared_diagnostic_start(void){
 const char *names[]={"login-trace-roll.txt","vm-lifetime.txt","sdk-init-state.txt","unity-native-fault.txt","config-network.txt","strncpy-trace.txt"};
 for(unsigned i=0;i<sizeof names/sizeof names[0];i++){
  char path[544];snprintf(path,sizeof path,"%s/%s",root,names[i]);
  int fd=open(path,O_WRONLY|O_CREAT|O_APPEND|O_CLOEXEC|O_NOFOLLOW,0644);
  if(fd>=0){(void)fchmod(fd,0644);close(fd);}
 }
}
static void*worker(void*unused){(void)unused;shared_diagnostic_start();uint64_t last=0;for(;;){
 if(atomic_load(&paused)){pthread_mutex_lock(&lock);head=tail;pthread_mutex_unlock(&lock);struct timespec delay={0,20000000};nanosleep(&delay,NULL);continue;}
 struct diag_row r;int have;pthread_mutex_lock(&lock);have=head!=tail;if(have){atomic_store(&busy,1);r=rows[head];head=(head+1)%DIAG_QUEUE;}pthread_mutex_unlock(&lock);
 if(have){apply_row(&r);atomic_store(&busy,0);}else{struct timespec delay={0,10000000};nanosleep(&delay,NULL);}
 uint64_t now=diag_now();if(now-last>=1000000000){last=now;controls();socket_counter_controls();socket_counter_export();net_legacy_export();net_exception_export();net_engine_export();net_response_export();trim_legacy();char p[544];snprintf(p,sizeof p,"%s/diagnostic-log-status.txt",root);FILE*f=fopen(p,"w");if(f){(void)fchmod(fileno(f),0644);fprintf(f,"limit_bytes=%zu backups=1 queue_slots=%d chunk_bytes=%d diagnostic_records_lost=%llu producer_write_max_ns=%llu producer_budget_ns=%llu budget_dropped=%llu budget_ns_max=%llu noisy_observe=%d cookie_live=%u cookie_quarantined=%u cookie_rejected=%u cookie_missing_close=%u cookie_cap=256\n",limit,DIAG_QUEUE,DIAG_CHUNK,(unsigned long long)atomic_load(&lost),(unsigned long long)atomic_load(&max_producer_ns),(unsigned long long)atomic_load(&producer_budget_ns),(unsigned long long)atomic_load(&budget_dropped),(unsigned long long)atomic_load(&budget_ns_max),atomic_load(&noisy_enabled),atomic_load(&cookie_live),atomic_load(&cookie_quarantined),atomic_load(&cookie_rejected),atomic_load(&cookie_missing_close));fclose(f);}}
 }return NULL;}
void perf_diag_start(const char*directory){if(!directory||atomic_flag_test_and_set(&start_once))return;snprintf(root,sizeof root,"%s",directory);
 char p[544];snprintf(p,sizeof p,"%s/net-room-observe",root);int fd=open(p,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0666);if(fd>=0){(void)write(fd,"0\n",2);(void)fchmod(fd,0666);close(fd);}
 snprintf(p,sizeof p,"%s/socket-counter-observe",root);fd=open(p,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0666);if(fd>=0){(void)write(fd,"0\n",2);(void)fchmod(fd,0666);close(fd);}
 pthread_t t;if(!pthread_create(&t,NULL,worker,NULL)){pthread_detach(t);atomic_store(&running,1);}else atomic_flag_clear(&start_once);}
void perf_diag_flush(void){
 extern uint64_t perf_ffrt_native_begin(int) __attribute__((weak));
 extern void perf_ffrt_native_end(int,uint64_t) __attribute__((weak));
 uint64_t begin=perf_ffrt_native_begin?perf_ffrt_native_begin(3):0;
 for(unsigned i=0;i<500;i++){pthread_mutex_lock(&lock);int empty=head==tail&&!atomic_load(&busy);pthread_mutex_unlock(&lock);if(empty)break;struct timespec t={0,10000000};nanosleep(&t,NULL);}
 if(begin&&perf_ffrt_native_end)perf_ffrt_native_end(3,begin);
}

/* Only request metadata is copied; never headers, bodies or arbitrary errors.
 * The existing async diagnostic worker owns disk IO, permissions and rotation. */
static void net_safe_url(const char *url,char *out,size_t cap){
 out[0]=0;const char *scheme=strstr(url,"://");if(!scheme)return;
 const char *host=scheme+3,*end=host;while(*end&&*end!='/'&&*end!='?'&&*end!='#')end++;
 size_t hostlen=(size_t)(end-host);if(!hostlen||hostlen>180||memchr(host,'@',hostlen))return;
 for(size_t i=0;i<hostlen;i++)if(!isalnum((unsigned char)host[i])&&host[i]!='.'&&host[i]!='-'&&host[i]!=':')return;
 size_t at=(size_t)snprintf(out,cap," host=%.*s path=",(int)hostlen,host);int private_next=0;
 const char *p=end;
 while(*p&&*p!='?'&&*p!='#'&&at+32<cap){
  if(*p=='/'){out[at++]='/';p++;continue;}
  const char *start=p;unsigned digits=0;int invalid=0;
  while(*p&&*p!='/'&&*p!='?'&&*p!='#'){if(isdigit((unsigned char)*p))digits++;if(!isalnum((unsigned char)*p)&&*p!='_'&&*p!='-'&&*p!='.')invalid=1;p++;}
  size_t n=(size_t)(p-start);char part[80];if(n>=sizeof part)n=sizeof part-1;memcpy(part,start,n);part[n]=0;
  static const char *const operations[]={"list","get","search","login","info","status","request","requests","pending","accept","remove","reject","all","find","check","update","recommend"};
  int operation=0;for(unsigned i=0;i<sizeof operations/sizeof operations[0];i++)if(!strcmp(part,operations[i]))operation=1;
  int redact=invalid||digits>=4||(size_t)(p-start)>24||(private_next&&!operation);
  const char *value=redact?"<redacted>":part;size_t length=strlen(value);if(at+length>=cap)break;memcpy(out+at,value,length);at+=length;
  private_next=!strcmp(part,"user")||!strcmp(part,"users")||!strcmp(part,"friend")||!strcmp(part,"friends")||!strcmp(part,"profile")||!strcmp(part,"avatar");
 }
 out[at]=0;
}
/* Known route positions only. Private segments are neither copied into a
 * label nor used to create one; unrecognized operations stay unknown. */
static const char *net_endpoint_operation(const char *url){
 const char *scheme=strstr(url,"://");if(!scheme)return "unknown";
 const char *p=scheme+3;p+=strcspn(p,"/?#");
 const char *part[8];size_t length[8];unsigned n=0;
 while(*p=='/'&&n<8){p++;part[n]=p;length[n]=strcspn(p,"/?#");p+=length[n++];}
 if(*p&&*p!='?'&&*p!='#')return "unknown";
#define NET_SEG(i,s) ((i)<n&&length[i]==sizeof(s)-1&&!memcmp(part[i],s,sizeof(s)-1))
 if(NET_SEG(0,"api")&&NET_SEG(1,"user")&&n==4&&length[2]&&NET_SEG(3,"multiroomfriendrecruitment"))return "room_friend_recruitment";
 if(n>=5&&NET_SEG(0,"api")&&NET_SEG(1,"suite")&&NET_SEG(2,"user")&&length[3]&&NET_SEG(4,"friend"))return "friend_endpoint";
 if(NET_SEG(0,"api")&&NET_SEG(1,"tcpServer")&&n==3)return "room_server_allocation";
 if(NET_SEG(0,"api")&&NET_SEG(1,"application")&&n==2)return "application";
 if(NET_SEG(0,"api")&&NET_SEG(1,"system")&&NET_SEG(2,"application")&&n==3)return "system_application";
 return "unknown";
#undef NET_SEG
}
void perf_diag_net_vlog(const char *fmt,va_list args){
 if(!atomic_load_explicit(&net_enabled,memory_order_relaxed))return;
 int saved=errno;char input[2048],safe[2048];vsnprintf(input,sizeof input,fmt,args);size_t at=0;
 static const char *const keys[]={"stage","request","rc","http","error_enum","method_enum","result_enum","elapsed_ms","connect_elapsed_ms","duration_ms","host","service","errno","so_error","curl_code","certificate_related","tls_related","ca_verification_failed","handshake_alert","fd","generation","nonblocking_pending","source_mono_ms","source_tid","source_mono_ns","exception_class","guest_class","guest_method","bytes","response_format","truncated","code_field","business_code","decoded","valid","server_fields","has_code"};
 for(char *p=input;*p&&at+320<sizeof safe;){
  while(*p==' ')p++;char *token=p;while(*p&&*p!=' ')p++;if(*p)*p++=0;char *eq=strchr(token,'=');if(!eq)continue;*eq++=0;
  if(!strcmp(token,"mono_ms"))token="source_mono_ms";
  if(!strcmp(token,"tid"))token="source_tid";
  eq[strcspn(eq,"\r\n")]=0;
  if(!strcmp(token,"url")){if(sizeof safe-at<600)continue;char address[512];net_safe_url(eq,address,sizeof address);size_t n=strlen(address);memcpy(safe+at,address,n);at+=n;if(*address)at+=(size_t)snprintf(safe+at,sizeof safe-at," endpoint_op=%s",net_endpoint_operation(eq));continue;}
  int keep=0;for(unsigned i=0;i<sizeof keys/sizeof keys[0];i++)if(!strcmp(token,keys[i]))keep=1;
  if(!keep||strlen(eq)>180)continue;int valid=1;for(const char *s=eq;*s;s++)if(!isalnum((unsigned char)*s)&&!strchr("_:.<>/-",*s))valid=0;
  if(valid)at+=(size_t)snprintf(safe+at,sizeof safe-at," %s=%s",token,eq);
 }
 /* Called only by the diagnostic worker: avoid platform cookie-stdio loss. */
 safe[at]=0;char path[544],record[2304];snprintf(path,sizeof path,"%s/net-room-network.txt",root);
 struct stat st;if(!lstat(path,&st)&&(size_t)st.st_size>=limit){char backup[560];snprintf(backup,sizeof backup,"%s.1",path);unlink(backup);rename(path,backup);}
 int fd=open(path,O_WRONLY|O_CREAT|O_APPEND|O_CLOEXEC|O_NOFOLLOW,0644);
 if(fd>=0){if(!fchmod(fd,0644)){int n=snprintf(record,sizeof record,"NET_META pid=%d mono_ms=%llu%s\n",getpid(),(unsigned long long)(diag_now()/1000000),safe);size_t length=(size_t)n<sizeof record?(size_t)n:sizeof record-1;size_t sent=0;while(sent<length){ssize_t rc=write(fd,record+sent,length-sent);if(rc<0&&errno==EINTR)continue;if(rc<=0)break;sent+=(size_t)rc;}}close(fd);}errno=saved;
}
