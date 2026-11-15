/* Included once by diagnostic_log.c: only its existing IO worker exports.
 * Only numeric connect peer IP/port metadata is retained; no caller strings
 * or payload bytes are retained. Producers allocate nothing and never wait for this mutex. */
#include "socket_counters.h"
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/syscall.h>
#define SOCKET_COUNTER_CAPACITY 256u
#define SOCKET_COUNTER_SESSION_LIMIT 8192u
#define SOCKET_COUNTER_FILE_LIMIT (256u * 1024u)
struct socket_counter_row {
 uint64_t session, mono_ns, duration_ns;
 int64_t result;
 int tid, family, type, error;
 const char *operation; /* internal literals only */
 char peer_ip[INET6_ADDRSTRLEN];
 int peer_port;
};
static struct socket_counter_row socket_rows[SOCKET_COUNTER_CAPACITY];
static unsigned socket_head, socket_count, socket_admitted;
static uint64_t socket_session;
static atomic_uint_fast64_t socket_active, socket_dropped_full, socket_dropped_busy,
 socket_dropped_budget, socket_discarded, socket_stale;
static pthread_mutex_t socket_lock=PTHREAD_MUTEX_INITIALIZER;
static uint64_t socket_base_full, socket_base_busy, socket_base_budget, socket_base_stale;
static int socket_export_error;
#define SOCKET_PEER_CAPACITY 64u
static struct {int used,fd,port;char ip[INET6_ADDRSTRLEN];} socket_peers[SOCKET_PEER_CAPACITY];
static unsigned socket_peer_next;
/* The recorder and peer table share socket_lock. Producers only trylock. */
static void socket_peer_save(int fd,const struct sockaddr *addr,socklen_t len){
 if(fd<0||!atomic_load_explicit(&socket_active,memory_order_relaxed))return;
 struct sockaddr_storage ss;memset(&ss,0,sizeof ss);
 if(addr)memcpy(&ss,addr,len<sizeof ss?len:sizeof ss);
 char ip[INET6_ADDRSTRLEN]={0};int port=0;
 if(addr&&ss.ss_family==AF_INET&&len>=sizeof(struct sockaddr_in)){
  const struct sockaddr_in *a=(const void*)&ss;
  if(inet_ntop(AF_INET,&a->sin_addr,ip,sizeof ip))port=ntohs(a->sin_port);
 }else if(addr&&ss.ss_family==AF_INET6&&len>=sizeof(struct sockaddr_in6)){
  const struct sockaddr_in6 *a=(const void*)&ss;
  if(inet_ntop(AF_INET6,&a->sin6_addr,ip,sizeof ip))port=ntohs(a->sin6_port);
 }
 if(pthread_mutex_trylock(&socket_lock)){atomic_fetch_add(&socket_dropped_busy,1);return;}
 unsigned at=SOCKET_PEER_CAPACITY;
 for(unsigned i=0;i<SOCKET_PEER_CAPACITY;i++)if(socket_peers[i].used&&socket_peers[i].fd==fd){at=i;break;}
 if(at==SOCKET_PEER_CAPACITY)at=socket_peer_next++%SOCKET_PEER_CAPACITY;
 socket_peers[at].used=ip[0]!=0;socket_peers[at].fd=fd;socket_peers[at].port=port;
 memcpy(socket_peers[at].ip,ip,sizeof ip);
 pthread_mutex_unlock(&socket_lock);
}


struct socket_counter_stamp socket_counter_begin(void){
 int saved=errno;
 uint64_t session=atomic_load_explicit(&socket_active,memory_order_relaxed);
 struct socket_counter_stamp stamp={session?diag_now():0,session};
 errno=saved;return stamp;
}
/* The private recorder accepts only compile-time operation names from wrappers.
 * It is deliberately not a general-purpose logging/string API. */
static void socket_record_at(struct socket_counter_stamp stamp,const char *operation,
 int family,int type,int64_t result,int error,uint64_t end,int fd){
 if(!stamp.session)return;
 if(stamp.session!=atomic_load_explicit(&socket_active,memory_order_relaxed)){
  atomic_fetch_add(&socket_stale,1);return;
 }
 int tid=(int)syscall(SYS_gettid);
 if(pthread_mutex_trylock(&socket_lock)){atomic_fetch_add(&socket_dropped_busy,1);return;}
 if(stamp.session!=atomic_load(&socket_active)){atomic_fetch_add(&socket_stale,1);}
 else if(socket_admitted>=SOCKET_COUNTER_SESSION_LIMIT)atomic_fetch_add(&socket_dropped_budget,1);
 else if(socket_count==SOCKET_COUNTER_CAPACITY)atomic_fetch_add(&socket_dropped_full,1);
 else{
  unsigned at=(socket_head+socket_count)%SOCKET_COUNTER_CAPACITY;
  socket_rows[at]=(struct socket_counter_row){.session=stamp.session,.mono_ns=end,
   .duration_ns=end-stamp.ns,.result=result,.tid=tid,.family=family,.type=type,
   .error=error,.operation=operation};
  for(unsigned i=0;i<SOCKET_PEER_CAPACITY;i++)if(socket_peers[i].used&&socket_peers[i].fd==fd){
   memcpy(socket_rows[at].peer_ip,socket_peers[i].ip,sizeof socket_rows[at].peer_ip);
   socket_rows[at].peer_port=socket_peers[i].port;break;
  }
  socket_count++;socket_admitted++;
 }
 pthread_mutex_unlock(&socket_lock);
}
static void socket_record(struct socket_counter_stamp stamp,const char *operation,
 int family,int type,int64_t result,int error){
 if(stamp.session)socket_record_at(stamp,operation,family,type,result,error,diag_now(),-1);
}
void socket_counter_end(struct socket_counter_stamp s,const char *op,int family,int type,int64_t result,int error){
 int saved=errno;
 /* No arbitrary caller text is ever retained or exported. */
 static const char *const allowed[]={"dns_android","connect_result"};
 for(unsigned i=0;i<sizeof allowed/sizeof allowed[0];i++)if(!strcmp(op,allowed[i])){
  socket_record(s,allowed[i],family,type,result,error);break;
 }
 errno=saved;
}
static void socket_metadata(int fd,int *family,int *type){
 socklen_t n=sizeof *type;*family=AF_UNSPEC;*type=0;
 if(getsockopt(fd,SOL_SOCKET,SO_TYPE,type,&n))return;
 struct sockaddr_storage addr;n=sizeof addr;
 if(!getsockname(fd,(struct sockaddr *)&addr,&n))*family=addr.ss_family;
}
static void socket_fd_record(struct socket_counter_stamp s,const char *op,int fd,int64_t result,int error){
 if(!s.session)return;
 uint64_t end=diag_now();int saved=errno,family,type;socket_metadata(fd,&family,&type);
 /* read/write wrappers also serve ordinary files: never record those. Other
  * operations retain failed/unknown descriptors as family=0,type=0. */
 if(type||(strcmp(op,"read")&&strcmp(op,"write")&&strcmp(op,"readv")&&strcmp(op,"writev")))
  socket_record_at(s,op,family,type,result,error,end,fd);
 errno=saved;
}
int socket_counter_socket(int family,int type,int protocol){
 struct socket_counter_stamp s=socket_counter_begin();int rc=socket(family,type,protocol),saved=errno;
 if(rc>=0)socket_peer_save(rc,NULL,0); /* Clear a recycled descriptor. */
 socket_record(s,"socket",family,type&~(SOCK_NONBLOCK|SOCK_CLOEXEC),rc,rc<0?saved:0);
 errno=saved;return rc;
}
int socket_counter_connect(int fd,const struct sockaddr *addr,socklen_t len){
 struct socket_counter_stamp s=socket_counter_begin();int rc=connect(fd,addr,len),saved=errno;
 socket_peer_save(fd,addr,len);
 socket_fd_record(s,"connect",fd,rc,rc<0?saved:0);errno=saved;return rc;
}
int socket_counter_getsockopt(int fd,int level,int option,void *value,socklen_t *len){
 struct socket_counter_stamp s=socket_counter_begin();int rc=getsockopt(fd,level,option,value,len),saved=errno;
 if(level==SOL_SOCKET&&option==SO_ERROR){
  int result=rc<0?-1:0;
  if(!rc&&value&&len&&*len>=sizeof(int))memcpy(&result,value,sizeof result);
  /* return=SO_ERROR on successful query; errno is the syscall errno only.
   * This is completion observation, not proof that a connect was pending. */
  socket_fd_record(s,"connect_so_error",fd,result,rc<0?saved:0);
 }
 errno=saved;return rc;
}
void socket_counter_connect_result(struct socket_counter_stamp s,int fd,int error){
 int saved=errno;socket_fd_record(s,"connect_result",fd,error? -1:0,error);errno=saved;
}
int socket_counter_getaddrinfo(const char *node,const char *service,const struct addrinfo *hints,struct addrinfo **out){
 struct socket_counter_stamp s=socket_counter_begin();int rc=getaddrinfo(node,service,hints,out),saved=errno;
 socket_record(s,"dns",hints?hints->ai_family:0,hints?hints->ai_socktype:0,rc,rc==EAI_SYSTEM?saved:0);
 errno=saved;return rc;
}
#define SOCKET_IO(name,params,args) \
 ssize_t socket_counter_##name params { \
 struct socket_counter_stamp s=socket_counter_begin();ssize_t rc=name args;int saved=errno; \
 socket_fd_record(s,#name,fd,rc,rc<0?saved:0);errno=saved;return rc; }
SOCKET_IO(send,(int fd,const void *buf,size_t n,int flags),(fd,buf,n,flags))
SOCKET_IO(recv,(int fd,void *buf,size_t n,int flags),(fd,buf,n,flags))
SOCKET_IO(sendto,(int fd,const void *buf,size_t n,int flags,const struct sockaddr *addr,socklen_t len),(fd,buf,n,flags,addr,len))
SOCKET_IO(recvfrom,(int fd,void *buf,size_t n,int flags,struct sockaddr *addr,socklen_t *len),(fd,buf,n,flags,addr,len))
SOCKET_IO(sendmsg,(int fd,const struct msghdr *msg,int flags),(fd,msg,flags))
SOCKET_IO(recvmsg,(int fd,struct msghdr *msg,int flags),(fd,msg,flags))
SOCKET_IO(read,(int fd,void *buf,size_t n),(fd,buf,n))
SOCKET_IO(write,(int fd,const void *buf,size_t n),(fd,buf,n))
SOCKET_IO(readv,(int fd,const struct iovec *v,int n),(fd,v,n))
SOCKET_IO(writev,(int fd,const struct iovec *v,int n),(fd,v,n))
#undef SOCKET_IO

/* Worker-owned session transitions. Lifetime loss counters remain monotonic;
 * per-session deltas conservatively include concurrent boundary drops. */
static void socket_counter_enable(int enabled){
 if((atomic_load(&socket_active)!=0)==enabled)return;
 pthread_mutex_lock(&socket_lock);
 atomic_store(&socket_active,0);
 if(enabled){
  atomic_fetch_add(&socket_discarded,socket_count);socket_head=socket_count=socket_admitted=0;
  socket_base_full=atomic_load(&socket_dropped_full);socket_base_busy=atomic_load(&socket_dropped_busy);
  socket_base_budget=atomic_load(&socket_dropped_budget);socket_base_stale=atomic_load(&socket_stale);
  socket_export_error=0;
  memset(socket_peers,0,sizeof socket_peers);socket_peer_next=0;
  socket_session++;atomic_store(&socket_active,socket_session);
 }
 pthread_mutex_unlock(&socket_lock);
}
static void socket_counter_controls(void){
 char path[544];snprintf(path,sizeof path,"%s/socket-counter-observe",root);
 FILE *f=fopen(path,"r");int enabled=0;if(f){fscanf(f,"%d",&enabled);fclose(f);}
 socket_counter_enable(enabled==1);
}
/* Rotation is independent of diagnostic-log-large, with a strict pre-write
 * cap. One backup only; a full disk drops diagnostics without touching IO. */
static int socket_export_line(const char *line){
 char path[544],backup[560];snprintf(path,sizeof path,"%s/socket-counters.csv",root);
 struct stat st;size_t n=strlen(line);
 if(!lstat(path,&st)&&(size_t)st.st_size+n>SOCKET_COUNTER_FILE_LIMIT){
  snprintf(backup,sizeof backup,"%s.1",path);unlink(backup);
  if(rename(path,backup))return errno;
 }
 int fd=open(path,O_WRONLY|O_CREAT|O_APPEND|O_NOFOLLOW|O_CLOEXEC,0644);if(fd<0)return errno;
 int error=0;if(fchmod(fd,0644))error=errno;
 for(size_t at=0;!error&&at<n;){ssize_t rc=write(fd,line+at,n-at);if(rc<0&&errno==EINTR)continue;if(rc<=0){error=rc<0?errno:EIO;break;}at+=(size_t)rc;}
 close(fd);return error;
}
static void socket_counter_export(void){
 struct socket_counter_row batch[SOCKET_COUNTER_CAPACITY];unsigned n,admitted;
 pthread_mutex_lock(&socket_lock);n=socket_count;admitted=socket_admitted;
 for(unsigned i=0;i<n;i++)batch[i]=socket_rows[(socket_head+i)%SOCKET_COUNTER_CAPACITY];
 socket_head=(socket_head+n)%SOCKET_COUNTER_CAPACITY;socket_count=0;pthread_mutex_unlock(&socket_lock);
 for(unsigned i=0;i<n;i++){
  struct socket_counter_row *r=&batch[i];char line[320];
  if(r->peer_ip[0])snprintf(line,sizeof line,"%d,%llu,%llu,%d,%s,%d,%d,%lld,%d,%llu,%s,%d\n",getpid(),
   (unsigned long long)r->session,(unsigned long long)r->mono_ns,r->tid,r->operation,
   r->family,r->type,(long long)r->result,r->error,(unsigned long long)r->duration_ns,r->peer_ip,r->peer_port);
  else snprintf(line,sizeof line,"%d,%llu,%llu,%d,%s,%d,%d,%lld,%d,%llu,,\n",getpid(),
   (unsigned long long)r->session,(unsigned long long)r->mono_ns,r->tid,r->operation,
   r->family,r->type,(long long)r->result,r->error,(unsigned long long)r->duration_ns);
  int error=socket_export_line(line);if(error){socket_export_error=error;atomic_fetch_add(&socket_discarded,1);}
 }
 if(!socket_session)return;
 char path[544];snprintf(path,sizeof path,"%s/socket-counter-status.txt",root);
 int fd=open(path,O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW|O_CLOEXEC,0644);
 if(fd<0)return;
 if(!fchmod(fd,0644))dprintf(fd,"pid=%d session=%llu enabled=%d capacity=%u session_limit=%u admitted=%u dropped_full=%llu dropped_busy=%llu dropped_budget=%llu stale=%llu discarded_total=%llu export_errno=%d columns=pid,session,mono_ns,tid,operation,family,type,return,errno,duration_ns,peer_ip,peer_port\n",
  getpid(),(unsigned long long)socket_session,atomic_load(&socket_active)!=0,
  SOCKET_COUNTER_CAPACITY,SOCKET_COUNTER_SESSION_LIMIT,admitted,
  (unsigned long long)(atomic_load(&socket_dropped_full)-socket_base_full),
  (unsigned long long)(atomic_load(&socket_dropped_busy)-socket_base_busy),
  (unsigned long long)(atomic_load(&socket_dropped_budget)-socket_base_budget),
  (unsigned long long)(atomic_load(&socket_stale)-socket_base_stale),
  (unsigned long long)atomic_load(&socket_discarded),socket_export_error);
 close(fd);
}
