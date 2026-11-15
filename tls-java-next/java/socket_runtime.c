#include "socket_runtime.h"
#include "socket_shared.h"
#include "perf_attr.h"
#include "socket_counters.h"
#include <arpa/inet.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <unicode/uidna.h>

#define IO_CHUNK 65536
extern int sdk_trace_fprintf(FILE *,const char *,...);
extern void sdk_trace_missing_api(const char *,const char *,const char *);

/* Shared by a Socket, its two streams and any layered TLS socket. The
 * descriptor is closed only after close() and once no call is blocked on it,
 * so a concurrent close never races a recycled descriptor number. Fields are
 * guarded by the interpreter lock; blocking calls copy what they need first. */
struct socket_state {
 int fd; unsigned owners,io_active;
 bool connected,closed,input_shutdown,output_shutdown,nodelay,keepalive;
 int timeout_ms,port,local_port; char *host;
 struct sockaddr_storage peer; socklen_t peer_len;
 dvm_ref input,output;
};
struct stream_ref { struct socket_state *s; };
struct inet_state { int family; unsigned char bytes[16]; char *host; };
struct address_state { dvm_ref inet; char *host; int port; };
static struct dvm *active;
static bool external_allowed;

static void state_release(struct socket_state *s){if(!s||--s->owners)return;if(s->fd>=0)close(s->fd);free(s->host);free(s);}
static void socket_drop(void *p){state_release(p);}
static void input_drop(void *p){struct stream_ref *r=p;if(r){state_release(r->s);free(r);}}
static void output_drop(void *p){struct stream_ref *r=p;if(r){state_release(r->s);free(r);}}
static void inet_drop(void *p){struct inet_state *i=p;if(i){free(i->host);free(i);}}
static void address_drop(void *p){struct address_state *a=p;if(a){free(a->host);free(a);}}
static bool fail(struct dvm *v,const char *type,const char *message){dvm__throw(v,type,"%s",message);return true;}
static void *payload(struct dvm *v,dvm_ref r,void (*kind)(void *)){struct dvm_object *o=dvm__obj(v,r);return o&&o->data_destroy==kind?o->data:NULL;}
static struct socket_state *state(struct dvm *v,dvm_ref r){return payload(v,r,socket_drop);}
static struct inet_state *inet(struct dvm *v,dvm_ref r){return payload(v,r,inet_drop);}
static struct address_state *address(struct dvm *v,dvm_ref r){return payload(v,r,address_drop);}
static bool attach(struct dvm *v,dvm_ref r,void *data,void (*kind)(void *)){struct dvm_object *o=dvm__obj(v,r);if(!o||o->data){kind(data);return false;}o->data=data;o->data_destroy=kind;return true;}

static void state_close(struct socket_state *s){if(s->closed)return;s->closed=true;if(s->fd>=0){shutdown(s->fd,SHUT_RDWR);if(!s->io_active){close(s->fd);s->fd=-1;}}}
static int io_begin(struct socket_state *s){if(s->closed||s->fd<0)return -1;s->io_active++;return s->fd;}
static void io_end(struct socket_state *s){if(s->io_active)s->io_active--;if(s->closed&&!s->io_active&&s->fd>=0){close(s->fd);s->fd=-1;}}
struct socket_state *socket_runtime_retain(struct dvm *v,dvm_ref r){struct socket_state *s=state(v,r);if(!s||!s->connected||s->closed)return NULL;s->owners++;return s;}
struct socket_state *socket_runtime_retain_for_connect(struct dvm *v,dvm_ref r){struct socket_state *s=state(v,r);if(!s||s->connected||s->fd>=0||s->closed)return NULL;s->owners++;return s;}
void socket_state_release(struct socket_state *s){state_release(s);}
int socket_state_io_begin(struct socket_state *s){return io_begin(s);}
void socket_state_io_end(struct socket_state *s){io_end(s);}
void socket_state_close(struct socket_state *s){state_close(s);}
bool socket_state_closed(const struct socket_state *s){return s->closed;}
bool socket_state_connected(const struct socket_state *s){return s&&s->connected&&!s->closed;}
int socket_state_timeout(const struct socket_state *s){return s->timeout_ms;}
void socket_state_set_timeout(struct socket_state *s,int t){s->timeout_ms=t;}
int socket_state_port(const struct socket_state *s){return s->port;}
int socket_state_local_port(const struct socket_state *s){return s->local_port;}
void socket_runtime_set_external(bool allowed){external_allowed=allowed;}
bool socket_runtime_external_allowed(void){return external_allowed;}

static int remaining_ms(const struct timespec *deadline){
 struct timespec now;clock_gettime(CLOCK_MONOTONIC,&now);
 long long ms=(long long)(deadline->tv_sec-now.tv_sec)*1000+(deadline->tv_nsec-now.tv_nsec)/1000000;
 return ms<0?0:ms>0x7fffffff?0x7fffffff:(int)ms;
}
static void deadline_after(struct timespec *d,int ms){clock_gettime(CLOCK_MONOTONIC,d);d->tv_sec+=ms/1000;d->tv_nsec+=(long)(ms%1000)*1000000;if(d->tv_nsec>=1000000000){d->tv_sec++;d->tv_nsec-=1000000000;}}
/* Waits for `events`; 1 ready, 0 timed out, -1 error (errno). timeout<=0 waits indefinitely. */
static int wait_fd(int fd,short events,int timeout,const struct timespec *deadline){
 for(;;){struct pollfd p={.fd=fd,.events=events};int rc=poll(&p,1,timeout>0?remaining_ms(deadline):-1);
  if(rc<0&&errno==EINTR)continue;return rc>0?1:rc;}
}

static bool loopback(const struct sockaddr *a){
 if(a->sa_family==AF_INET)return (ntohl(((const struct sockaddr_in *)a)->sin_addr.s_addr)>>24)==127;
 if(a->sa_family==AF_INET6){const struct in6_addr *x=&((const struct sockaddr_in6 *)a)->sin6_addr;return IN6_IS_ADDR_LOOPBACK(x)||(IN6_IS_ADDR_V4MAPPED(x)&&x->s6_addr[12]==127);}
 return false;
}
static socklen_t to_sockaddr(const struct inet_state *in,int port,struct sockaddr_storage *sa){
 memset(sa,0,sizeof *sa);
 if(in->family==AF_INET){struct sockaddr_in *a=(struct sockaddr_in *)sa;a->sin_family=AF_INET;a->sin_port=htons((uint16_t)port);memcpy(&a->sin_addr,in->bytes,4);return sizeof *a;}
 struct sockaddr_in6 *a=(struct sockaddr_in6 *)sa;a->sin6_family=AF_INET6;a->sin6_port=htons((uint16_t)port);memcpy(&a->sin6_addr,in->bytes,16);return sizeof *a;
}
static void from_sockaddr(const struct sockaddr_storage *sa,struct inet_state *in){
 memset(in,0,sizeof *in);
 if(sa->ss_family==AF_INET){in->family=AF_INET;memcpy(in->bytes,&((const struct sockaddr_in *)sa)->sin_addr,4);}
 else{in->family=AF_INET6;memcpy(in->bytes,&((const struct sockaddr_in6 *)sa)->sin6_addr,16);}
}
static void literal_text(const struct inet_state *in,char *text,size_t size){if(!inet_ntop(in->family,in->bytes,text,(socklen_t)size))snprintf(text,size,"?");}
/* Java converts IPv4-mapped IPv6 addresses to Inet4Address. */
static void normalize(struct inet_state *in){if(in->family==AF_INET6&&IN6_IS_ADDR_V4MAPPED((struct in6_addr *)in->bytes)){memmove(in->bytes,in->bytes+12,4);memset(in->bytes+4,0,12);in->family=AF_INET;}}
static bool parse_literal(const char *host,struct inet_state *out){
 memset(out,0,sizeof *out);
 if(inet_pton(AF_INET,host,out->bytes)==1){out->family=AF_INET;return true;}
 char buffer[64];size_t n=strlen(host);const char *h=host;
 if(n>2&&host[0]=='['&&host[n-1]==']'&&n-2<sizeof buffer){memcpy(buffer,host+1,n-2);buffer[n-2]=0;h=buffer;}
 if(inet_pton(AF_INET6,h,out->bytes)==1){out->family=AF_INET6;normalize(out);return true;}
 return false;
}
/* Resolver order, duplicates removed. Count, or -1 with *error set. Lock released by caller. */
static int resolve(const char *host,struct inet_state *out,int max,int *error){
 struct addrinfo hints={0},*all=NULL;hints.ai_family=AF_UNSPEC;hints.ai_socktype=SOCK_STREAM;hints.ai_flags=AI_ADDRCONFIG;
 int rc=socket_counter_getaddrinfo(host,NULL,&hints,&all);if(rc){*error=rc;return -1;}
 int n=0;
 for(struct addrinfo *it=all;it&&n<max;it=it->ai_next){
  if(it->ai_family!=AF_INET&&it->ai_family!=AF_INET6)continue;
  struct inet_state cur;from_sockaddr((const struct sockaddr_storage *)it->ai_addr,&cur);normalize(&cur);
  bool seen=false;for(int i=0;i<n;i++)if(out[i].family==cur.family&&!memcmp(out[i].bytes,cur.bytes,16))seen=true;
  if(!seen)out[n++]=cur;
 }
 freeaddrinfo(all);if(!n){*error=EAI_NONAME;return -1;}return n;
}
static dvm_ref new_inet(struct dvm *v,const struct inet_state *src,const char *host){
 struct inet_state *st=calloc(1,sizeof *st);if(!st){fail(v,"java/lang/OutOfMemoryError","InetAddress");return 0;}
 st->family=src->family;memcpy(st->bytes,src->bytes,16);
 if(host&&!(st->host=strdup(host))){free(st);fail(v,"java/lang/OutOfMemoryError","InetAddress host");return 0;}
 dvm_ref r=dvm_new_object(v,dvm_find_class_or_external(v,st->family==AF_INET?"java/net/Inet4Address":"java/net/Inet6Address"));
 if(!r){inet_drop(st);fail(v,"java/lang/OutOfMemoryError","InetAddress object");return 0;}
 attach(v,r,st,inet_drop);return r;
}
static dvm_ref new_address(struct dvm *v,dvm_ref in,const char *host,int port){
 struct address_state *ad=calloc(1,sizeof *ad);if(!ad||(host&&!(ad->host=strdup(host)))){free(ad);fail(v,"java/lang/OutOfMemoryError","InetSocketAddress");return 0;}
 ad->inet=in;ad->port=port;dvm_ref r=dvm_new_object(v,dvm_find_class_or_external(v,"java/net/InetSocketAddress"));
 if(!r){address_drop(ad);fail(v,"java/lang/OutOfMemoryError","InetSocketAddress object");return 0;}
 attach(v,r,ad,address_drop);return r;
}
static const char *utf8_arg(struct dvm *v,dvm_ref r,bool *bad){const char *s=dvm_string_utf8(v,r);*bad=s&&strlen(s)!=dvm_string_utf8_length(v,r);return s;}
static dvm_ref new_bytes(struct dvm *v,const void *data,size_t n){dvm_ref r=dvm_new_array(v,'B',"B",(uint32_t)n);if(!r){fail(v,"java/lang/OutOfMemoryError","byte array");return 0;}memcpy(dvm_array_data(v,r),data,n);return r;}

/* InetAddress.getAllByName semantics: null/empty is loopback, literals are
 * parsed without a lookup and carry no host name, names are resolved. */
static bool all_by_name(struct dvm *v,dvm_ref name,union dvm_value *out,bool first_only){
 bool bad=false;const char *host=name?utf8_arg(v,name,&bad):NULL;
 if(bad)return fail(v,"java/net/UnknownHostException","NUL in host name");
 struct inet_state found[16];int n;const char *label=NULL;
 if(!host||!*host){memset(found,0,sizeof found);found[0].family=AF_INET6;found[0].bytes[15]=1;found[1].family=AF_INET;found[1].bytes[0]=127;found[1].bytes[3]=1;n=2;label="localhost";}
 else if(parse_literal(host,&found[0]))n=1;
 else{
  char *copy=strdup(host);if(!copy)return fail(v,"java/lang/OutOfMemoryError","host");
  int error=0;unsigned depth=dvm_gil_unlock_all(v);n=resolve(copy,found,16,&error);dvm_gil_relock(v,depth);
  {static unsigned logged;if(logged++<32){int saved=errno;FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt","a");if(f){dprintf(fileno(f),"NET_JAVA_DNS host=%s count=%d error=%d steps=%llu thread=%u\n",copy,n,error,(unsigned long long)v->call_steps,v->cur_thread);fclose(f);}errno=saved;}}
  if(n<0){dvm__throw(v,"java/net/UnknownHostException","Unable to resolve host \"%s\": %s",copy,gai_strerror(error));free(copy);return true;}
  free(copy);label=host;
 }
 if(first_only){out->l=new_inet(v,&found[0],label);return true;}
 dvm_ref array=dvm_new_array(v,'L',"Ljava/net/InetAddress;",(uint32_t)n);if(!array)return fail(v,"java/lang/OutOfMemoryError","InetAddress array");
 for(int i=0;i<n;i++){dvm_ref item=new_inet(v,&found[i],label);if(!item)return true;((dvm_ref *)dvm_array_data(v,array))[i]=item;}
 out->l=array;return true;
}
static int inet_hash(const struct inet_state *in){
 if(in->family==AF_INET)return (int)((uint32_t)in->bytes[0]<<24|(uint32_t)in->bytes[1]<<16|(uint32_t)in->bytes[2]<<8|in->bytes[3]);
 /* OpenJDK Inet6Address: sum of four big-endian words built from signed bytes. */
 int hash=0;for(int i=0;i<16;){int component=0;for(int j=0;j<4&&i<16;j++,i++)component=(int)((unsigned)component<<8)+(signed char)in->bytes[i];hash+=component;}
 return hash;
}
static bool reverse_name(struct dvm *v,const struct inet_state *in,char *name,size_t size){
 struct sockaddr_storage sa;socklen_t len=to_sockaddr(in,0,&sa);unsigned depth=dvm_gil_unlock_all(v);
 int rc=getnameinfo((struct sockaddr *)&sa,len,name,(socklen_t)size,NULL,0,NI_NAMEREQD);dvm_gil_relock(v,depth);return rc==0;
}
#define IS(x,y) (!strcmp(m,x)&&!strcmp(sig,y))
static bool inet_call(struct dvm *v,struct inet_state *in,const char *m,const char *sig,const union dvm_value *a,int n,union dvm_value *out){
 char text[INET6_ADDRSTRLEN];literal_text(in,text,sizeof text);
 if(IS("getHostAddress","()Ljava/lang/String;")){out->l=dvm_new_string(v,text);return true;}
 if(IS("getHostName","()Ljava/lang/String;")||IS("getCanonicalHostName","()Ljava/lang/String;")){
  char name[NI_MAXHOST];
  if(!strcmp(m,"getHostName")&&in->host){out->l=dvm_new_string(v,in->host);return true;}
  if(!reverse_name(v,in,name,sizeof name))snprintf(name,sizeof name,"%s",text);
  if(!strcmp(m,"getHostName")&&!in->host)in->host=strdup(name);
  out->l=dvm_new_string(v,name);return true;
 }
 if(IS("getAddress","()[B")){out->l=new_bytes(v,in->bytes,in->family==AF_INET?4:16);return true;}
 if(IS("hashCode","()I")){out->i=inet_hash(in);return true;}
 if(IS("equals","(Ljava/lang/Object;)Z")&&n==1){struct inet_state *other=inet(v,a[0].l);out->i=other&&other->family==in->family&&!memcmp(other->bytes,in->bytes,16);return true;}
 if(IS("toString","()Ljava/lang/String;")){char line[NI_MAXHOST+INET6_ADDRSTRLEN+2];snprintf(line,sizeof line,"%s/%s",in->host?in->host:"",text);out->l=dvm_new_string(v,line);return true;}
 const unsigned char *b=in->bytes;bool v4=in->family==AF_INET;
 if(IS("isLoopbackAddress","()Z")){out->i=v4?b[0]==127:IN6_IS_ADDR_LOOPBACK((const struct in6_addr *)b);return true;}
 if(IS("isAnyLocalAddress","()Z")){out->i=v4?!(b[0]|b[1]|b[2]|b[3]):IN6_IS_ADDR_UNSPECIFIED((const struct in6_addr *)b);return true;}
 if(IS("isLinkLocalAddress","()Z")){out->i=v4?b[0]==169&&b[1]==254:IN6_IS_ADDR_LINKLOCAL((const struct in6_addr *)b);return true;}
 if(IS("isSiteLocalAddress","()Z")){out->i=v4?b[0]==10||(b[0]==172&&(b[1]&0xf0)==16)||(b[0]==192&&b[1]==168):IN6_IS_ADDR_SITELOCAL((const struct in6_addr *)b);return true;}
 if(IS("isMulticastAddress","()Z")){out->i=v4?(b[0]&0xf0)==224:b[0]==0xff;return true;}
 return false;
}
static bool address_call(struct dvm *v,struct address_state *ad,const char *m,const char *sig,const union dvm_value *a,int n,union dvm_value *out){
 struct inet_state *in=inet(v,ad->inet);
 if(IS("getPort","()I")){out->i=ad->port;return true;}
 if(IS("getAddress","()Ljava/net/InetAddress;")){out->l=ad->inet;return true;}
 if(IS("isUnresolved","()Z")){out->i=!in;return true;}
 if(IS("getHostString","()Ljava/lang/String;")){char text[INET6_ADDRSTRLEN];if(ad->host){out->l=dvm_new_string(v,ad->host);return true;}if(in->host){out->l=dvm_new_string(v,in->host);return true;}literal_text(in,text,sizeof text);out->l=dvm_new_string(v,text);return true;}
 if(IS("getHostName","()Ljava/lang/String;")){if(ad->host){out->l=dvm_new_string(v,ad->host);return true;}return inet_call(v,in,"getHostName","()Ljava/lang/String;",NULL,0,out);}
 if(IS("hashCode","()I")){if(in){out->i=inet_hash(in)+ad->port;return true;}
  /* Unresolved: hostname.toLowerCase().hashCode() + port, over UTF-16 units of an ASCII-lowered name. */
  int h=0;for(const unsigned char *p=(const unsigned char *)ad->host;p&&*p;p++)h=31*h+(*p>='A'&&*p<='Z'?*p+32:*p);out->i=h+ad->port;return true;}
 if(IS("equals","(Ljava/lang/Object;)Z")&&n==1){struct address_state *o=address(v,a[0].l);bool eq=false;
  if(o&&o->port==ad->port){struct inet_state *oi=inet(v,o->inet);if(in&&oi)eq=in->family==oi->family&&!memcmp(in->bytes,oi->bytes,16);else if(!in&&!oi)eq=ad->host&&o->host&&!strcasecmp(ad->host,o->host);}
  out->i=eq;return true;}
 if(IS("toString","()Ljava/lang/String;")){char line[1024],text[INET6_ADDRSTRLEN];
  if(in){literal_text(in,text,sizeof text);snprintf(line,sizeof line,"%s/%s:%d",in->host?in->host:(ad->host?ad->host:""),text,ad->port);}
  else snprintf(line,sizeof line,"%s/<unresolved>:%d",ad->host?ad->host:"",ad->port);
  out->l=dvm_new_string(v,line);return true;}
 return false;
}

/* ---- java.net.IDN -------------------------------------------------------
 * Java IDN.toASCII(String) uses flags 0.  Pure-ASCII labels are returned
 * verbatim (case preserved; an empty label or a label longer than 63 code
 * units is rejected), while non-ASCII labels become their real ACE form.
 * ASCII spaces/control characters and other non-LDH bytes are NOT rejected
 * by Java's IDN itself; OkHttp's canonicalizeHost immediately calls
 * Util.containsInvalidHostnameAsciiCodes() after IDN and rejects them, so
 * returning them verbatim here matches the platform and keeps that guard in
 * charge.  The OHOS NDK ships ICU UTS #46 as unicode/uidna.h + libicu.so.
 * UTS #46 with UIDNA_DEFAULT is transitional, matching IDNA2003 mappings
 * such as "faß" -> "fass".  ICU is loaded lazily: ASCII hosts never depend
 * on it, and if libicu.so is unavailable a non-ASCII host fails closed with
 * UnsupportedOperationException instead of returning unconverted input. */
typedef UIDNA *(*idn_open_fn)(uint32_t,UErrorCode *);
typedef void (*idn_close_fn)(UIDNA *);
typedef int32_t (*idn_label_fn)(const UIDNA *,const char *,int32_t,char *,int32_t,UIDNAInfo *,UErrorCode *);
static void *idn_library;
static UIDNA *idn_converter;
static idn_open_fn idn_open;
static idn_close_fn idn_close;
static idn_label_fn idn_label_ascii;

static bool idn_native_ready(void){
 if(idn_converter)return true;
 if(!idn_library){
  idn_library=dlopen("libicu.so",RTLD_NOW|RTLD_LOCAL);
  if(!idn_library)return false;
  idn_open=(idn_open_fn)dlsym(idn_library,"uidna_openUTS46");
  idn_close=(idn_close_fn)dlsym(idn_library,"uidna_close");
  idn_label_ascii=(idn_label_fn)dlsym(idn_library,"uidna_labelToASCII_UTF8");
 }
 if(!idn_open||!idn_close||!idn_label_ascii)return false;
 UErrorCode error=U_ZERO_ERROR;
 UIDNA *converter=idn_open(0,&error); /* UIDNA_DEFAULT: transitional UTS #46 */
 if(U_FAILURE(error)||!converter){sdk_trace_fprintf(stderr,"IDN_NATIVE_OPEN_FAILED code=%d\n",(int)error);return false;}
 idn_converter=converter;return true;
}
/* U+3002, U+FF0E and U+FF61 are IDNA label separators, exactly as Java's
 * nameprep maps them to '.'.  Returns the separator width in bytes, or 0. */
static size_t idn_separator(const char *text,size_t length){
 if(length&&(unsigned char)text[0]=='.')return 1;
 if(length>=3&&(unsigned char)text[0]==0xE3&&(unsigned char)text[1]==0x80&&(unsigned char)text[2]==0x82)return 3;
 if(length>=3&&(unsigned char)text[0]==0xEF&&(unsigned char)text[1]==0xBC&&(unsigned char)text[2]==0x8E)return 3;
 if(length>=3&&(unsigned char)text[0]==0xEF&&(unsigned char)text[1]==0xBD&&(unsigned char)text[2]==0xA1)return 3;
 return 0;
}
static bool idn_append(char **buffer,size_t *capacity,size_t *used,const char *text,size_t length){
 if(length>(size_t)-1-*used-1)return false;
 size_t need=*used+length+1;
 if(need>*capacity){
  size_t next=*capacity?*capacity:64;
  while(next<need){if(next>(size_t)-1/2)return false;next*=2;}
  char *grown=realloc(*buffer,next);if(!grown)return false;
  *buffer=grown;*capacity=next;
 }
 memcpy(*buffer+*used,text,length);*used+=length;(*buffer)[*used]=0;return true;
}
/* 0 converted, 1 invalid IDN, 2 out of memory. */
static int idn_convert_label(const char *label,size_t length,char **text,size_t *text_length){
 if(length>(size_t)0x7fffffff)return 1;
 char stack[512];
 UErrorCode error=U_ZERO_ERROR;
 UIDNAInfo info;memset(&info,0,sizeof info);info.size=(int16_t)sizeof info;
 int32_t written=idn_label_ascii(idn_converter,label,(int32_t)length,stack,(int32_t)sizeof stack,&info,&error);
 char *result=stack;size_t capacity=sizeof stack;bool owned=false;
 if(error==U_BUFFER_OVERFLOW_ERROR){
  if(written<0||(size_t)written>=(size_t)0x7fffffff)return 1;
  capacity=(size_t)written+1;result=malloc(capacity);if(!result)return 2;owned=true;
  error=U_ZERO_ERROR;memset(&info,0,sizeof info);info.size=(int16_t)sizeof info;
  written=idn_label_ascii(idn_converter,label,(int32_t)length,result,(int32_t)capacity,&info,&error);
 }
 if(U_FAILURE(error)||info.errors||written<0||(size_t)written>=capacity){
  if(owned)free(result);return 1;
 }
 for(int32_t i=0;i<written;i++)if((unsigned char)result[i]>=0x80){if(owned)free(result);return 1;}
 if(!owned){
  result=malloc((size_t)written+1);if(!result)return 2;
  memcpy(result,stack,(size_t)written);
 }
 result[written]=0;*text=result;*text_length=(size_t)written;return 0;
}
static bool idn_to_ascii(struct dvm *v,dvm_ref input,union dvm_value *out){
 if(!input)return fail(v,"java/lang/NullPointerException","input == null");
 const char *text=dvm_string_utf8(v,input);size_t length=dvm_string_utf8_length(v,input);
 if(!text)return fail(v,"java/lang/IllegalArgumentException","input is not a string");
 if(!length){out->l=dvm_new_string_n(v,"",0);return out->l?true:fail(v,"java/lang/OutOfMemoryError","IDN result");}
 if(idn_separator(text,length)==length){out->l=dvm_new_string(v,".");return out->l?true:fail(v,"java/lang/OutOfMemoryError","IDN result");}
 size_t capacity=length+64;
 if(capacity<length)return fail(v,"java/lang/OutOfMemoryError","IDN result");
 char *result=malloc(capacity);if(!result)return fail(v,"java/lang/OutOfMemoryError","IDN result");
 size_t used=0,at=0;bool first=true,trailing=false;
 while(at<length){
  size_t separator=idn_separator(text+at,length-at);
  if(separator)goto invalid_empty;
  size_t start=at;
  while(at<length&&!idn_separator(text+at,length-at))at++;
  size_t label_length=at-start;
  if(!first&&!idn_append(&result,&capacity,&used,".",1))goto oom_free;
  first=false;
  bool ascii=true;
  for(size_t i=start;i<at;i++)if((unsigned char)text[i]>=0x80){ascii=false;break;}
  if(ascii){
   if(label_length>63)goto invalid_long;
   if(!idn_append(&result,&capacity,&used,text+start,label_length))goto oom_free;
  }else{
   if(!idn_native_ready())goto unavailable;
   char *converted=NULL;size_t converted_length=0;
   int conversion=idn_convert_label(text+start,label_length,&converted,&converted_length);
   if(conversion==2)goto oom_free;
   if(conversion){free(converted);goto invalid_idn;}
   if(!idn_append(&result,&capacity,&used,converted,converted_length)){free(converted);goto oom_free;}
   free(converted);
  }
  if(at<length){at+=idn_separator(text+at,length-at);trailing=at==length;}
  else trailing=false;
 }
 if(trailing&&!idn_append(&result,&capacity,&used,".",1))goto oom_free;
 out->l=dvm_new_string_n(v,result,used);free(result);
 return out->l?true:fail(v,"java/lang/OutOfMemoryError","IDN result");
invalid_empty:
 free(result);
 sdk_trace_fprintf(stderr,"IDN_INVALID input=\"%.80s\" reason=empty_label\n",text);
 dvm__throw(v,"java/lang/IllegalArgumentException","Empty label is not a legal name");return true;
invalid_long:
 free(result);
 sdk_trace_fprintf(stderr,"IDN_INVALID input=\"%.80s\" reason=label_too_long\n",text);
 dvm__throw(v,"java/lang/IllegalArgumentException","The label in the input is too long");return true;
invalid_idn:
 free(result);
 sdk_trace_fprintf(stderr,"IDN_INVALID input=\"%.80s\" reason=conversion_failed\n",text);
 dvm__throw(v,"java/lang/IllegalArgumentException","Invalid IDN: %.160s",text);return true;
unavailable:
 free(result);
 sdk_trace_missing_api("java/net/IDN","toASCII","(Ljava/lang/String;)Ljava/lang/String;");
 sdk_trace_fprintf(stderr,"IDN_NATIVE_UNAVAILABLE input=\"%.80s\" lib=libicu.so result=UnsupportedOperationException\n",text);
 dvm__throw(v,"java/lang/UnsupportedOperationException","java.net.IDN.toASCII: non-ASCII input requires native ICU IDN (libicu.so)");return true;
oom_free:
 free(result);
 return fail(v,"java/lang/OutOfMemoryError","IDN result");
}

/* ---- java.net.URLEncoder -------------------------------------------------
 * HttpUtils.appendUrlParams passes a literal "UTF-8" from inside a broad
 * try/catch (Exception), so an encoding failure would surface as the original
 * URL rather than an app-visible exception.  The encoder still has to follow
 * the JDK contract.  The DVM stores String data as UTF-8 (embedded U+0000 is
 * kept as its MUTF-8 pair C0 80), so percent-encoding those UTF-8 bytes is
 * exactly what String.getBytes("UTF-8") produces for every scalar value.
 * Java's application/x-www-form-urlencoded rules: space becomes '+', the
 * bytes A-Z a-z 0-9 . - * _ stay literal, and every other byte becomes %XX
 * with upper-case hex.  '+' is therefore %2B, never a space.  Names are
 * compared with an ASCII-only case fold (no strcasecmp/toupper, which depend
 * on the host locale).  Only the UTF-8 alias set is implemented; any other
 * non-empty name fails closed with java/io/UnsupportedEncodingException
 * instead of returning the input unchanged. */
static bool urlencoder_name(const char *name,const char *want){
 for(;;name++,want++){
  unsigned char a=(unsigned char)*name,b=(unsigned char)*want;
  if(a>='A'&&a<='Z')a=(unsigned char)(a+32);
  if(b>='A'&&b<='Z')b=(unsigned char)(b+32);
  if(a!=b)return false;
  if(!a)return true;
 }
}
static bool urlencoder_utf8_name(const char *name){
 /* OpenJDK UTF-8 aliases: UTF8 and unicode-1-1-utf-8, case-insensitive. */
 return urlencoder_name(name,"UTF-8")||urlencoder_name(name,"UTF8")||urlencoder_name(name,"unicode-1-1-utf-8");
}
static void urlencoder_hex(char *text,size_t *used,unsigned char byte){
 static const char digits[]="0123456789ABCDEF";
 text[(*used)++]='%';text[(*used)++]=digits[byte>>4];text[(*used)++]=digits[byte&15];
}
static bool urlencoder_encode(struct dvm *v,dvm_ref input,dvm_ref charset,union dvm_value *out){
 if(!input)return fail(v,"java/lang/NullPointerException","url");
 if(!charset)return fail(v,"java/lang/NullPointerException","charset");
 const char *name=dvm_string_utf8(v,charset);
 size_t name_length=dvm_string_utf8_length(v,charset);
 if(!name)return fail(v,"java/lang/NullPointerException","charset");
 if(!name_length){dvm__throw(v,"java/io/UnsupportedEncodingException","URLEncoder.encode: empty charset name");return true;}
 /* An embedded NUL cannot be a legal charset name; reject it rather than
  * silently matching on the prefix before it. */
 if(strlen(name)!=name_length||!urlencoder_utf8_name(name)){dvm__throw(v,"java/io/UnsupportedEncodingException","Unsupported encoding: %s",name);return true;}
 const char *text=dvm_string_utf8(v,input);
 size_t length=dvm_string_utf8_length(v,input);
 if(!text)return fail(v,"java/lang/NullPointerException","url");
 if(length>(((size_t)-1)-1)/3)return fail(v,"java/lang/OutOfMemoryError","URLEncoder result");
 char *result=malloc(length*3+1);
 if(!result)return fail(v,"java/lang/OutOfMemoryError","URLEncoder result");
 size_t used=0;
 for(size_t i=0;i<length;i++){
  unsigned char byte=(unsigned char)text[i];
  if(byte==' '){result[used++]='+';continue;}
  if((byte>='A'&&byte<='Z')||(byte>='a'&&byte<='z')||(byte>='0'&&byte<='9')||byte=='.'||byte=='-'||byte=='*'||byte=='_'){result[used++]=(char)byte;continue;}
  /* The loader keeps U+0000 as the MUTF-8 pair C0 80; Java sees one char
   * and emits the single byte 00, not the two bytes C0 80. */
  if(byte==0xc0&&i+1<length&&(unsigned char)text[i+1]==0x80){urlencoder_hex(result,&used,0);i++;continue;}
  urlencoder_hex(result,&used,byte);
 }
 out->l=dvm_new_string_n(v,result,used);
 free(result);
 return out->l?true:fail(v,"java/lang/OutOfMemoryError","URLEncoder result");
}

/* ---- java.net.URL -------------------------------------------------------
 * OkHttp's HttpUrl.url() only performs `new URL(HttpUrl.url)`, and every DEX
 * caller then treats the result as a value: toString(), a field store, or an
 * argument to java.net.Authenticator.requestPasswordAuthentication.  No DEX
 * caller of HttpUrl.url() invokes URL.openConnection/openStream on that
 * object, so this type is implemented as an immutable value parser rather
 * than a connection factory.  The stored fields are the ones Java exposes:
 * protocol/host/port (explicit port only; -1 when absent), authority,
 * userinfo, path, query, ref and the scheme's default port (80, 443, -1).
 * getFile() and toExternalForm() are built from those fields exactly like
 * java.net.URLStreamHandler.toExternalForm().  Only http, https and file are
 * accepted; any other scheme, a host-less http(s) URL, a missing "//", an
 * invalid port or an invalid IPv6 literal fails closed with
 * MalformedURLException.  openConnection/openStream/toURI would require a
 * real URLConnection/URI and therefore throw UnsupportedOperationException
 * instead of returning a fake object. */
struct url_state {
 char *protocol,*authority,*userinfo,*host,*path,*query,*ref;
 int port,default_port;
};
static void url_drop(void *p){struct url_state *u=p;if(!u)return;free(u->protocol);free(u->authority);free(u->userinfo);free(u->host);free(u->path);free(u->query);free(u->ref);free(u);}
static struct url_state *url_state_of(struct dvm *v,dvm_ref r){return payload(v,r,url_drop);}
static char *url_piece(const char *text,size_t length){char *copy=malloc(length+1);if(!copy)return NULL;memcpy(copy,text,length);copy[length]=0;return copy;}
/* java.net.URL keeps -1 for an absent port and accepts 0..INT_MAX; signed or
 * otherwise non-decimal forms fail closed instead of being reinterpreted. */
static bool url_port(const char *text,size_t length,int *port){
 if(!length){*port=-1;return true;}
 long long value=0;
 for(size_t i=0;i<length;i++){unsigned char c=(unsigned char)text[i];if(c<'0'||c>'9')return false;value=value*10+(c-'0');if(value>0x7fffffff)return false;}
 *port=(int)value;return true;
}
static bool url_host_byte(unsigned char c){return c>0x20&&c!=0x7f&&c!='/'&&c!='?'&&c!='#'&&c!='@';}
/* RFC 2732 bracket contents must be a real IPv6 literal; Java also accepts a
 * zone identifier after '%' and keeps it verbatim in getHost(). */
static bool url_ipv6_literal(const char *text,size_t length){
 if(!length)return false;
 size_t zone=length;
 for(size_t i=0;i<length;i++)if(text[i]=='%'){zone=i;break;}
 if(zone==0||(zone<length&&zone+1==length)||zone>45)return false;
 if(zone<length)for(size_t i=zone+1;i<length;i++){unsigned char c=(unsigned char)text[i];if(c<0x20||c==0x7f||c=='/'||c=='?'||c=='#'||c=='@'||c=='['||c==']')return false;}
 char address[46];memcpy(address,text,zone);address[zone]=0;
 unsigned char bytes[16];return inet_pton(AF_INET6,address,bytes)==1;
}
static bool url_authority_error(struct dvm *v,const char *text){dvm__throw(v,"java/net/MalformedURLException","Invalid authority field: %.200s",text);return false;}
/* Splits [userinfo@]host[:port].  IPv6 literals keep their brackets in
 * getHost(), as java.net.URL documents (RFC 2732).  host_required is false
 * for file URLs, where an empty host is legal (file:///path). */
static bool url_parse_authority(struct dvm *v,const char *text,size_t length,struct url_state *u,bool host_required){
 size_t at=(size_t)-1;bool brackets=false;
 for(size_t i=0;i<length;i++){
  if(text[i]=='[')brackets=true;
  else if(text[i]==']')brackets=false;
  else if(text[i]=='@'&&!brackets)at=i;
 }
 size_t start=0;
 if(at!=(size_t)-1){u->userinfo=url_piece(text,at);if(!u->userinfo)goto oom;start=at+1;}
 u->authority=url_piece(text,length);if(!u->authority)goto oom;
 if(start<length&&text[start]=='['){
  size_t close=start+1;while(close<length&&text[close]!=']')close++;
  if(close==length)return url_authority_error(v,text);
  if(!url_ipv6_literal(text+start+1,close-start-1))return url_authority_error(v,text);
  u->host=url_piece(text+start,close+1-start);if(!u->host)goto oom;
  size_t after=close+1;
  if(after<length&&(text[after]!=':'||!url_port(text+after+1,length-after-1,&u->port)))return url_authority_error(v,text);
  if(after==length)u->port=-1;
  return true;
 }
 size_t colon=(size_t)-1;
 for(size_t i=start;i<length;i++)if(text[i]==':'){colon=i;break;}
 size_t host_end=colon==(size_t)-1?length:colon;
 if(host_end==start&&host_required)return url_authority_error(v,text);
 for(size_t i=start;i<host_end;i++)if(!url_host_byte((unsigned char)text[i]))return url_authority_error(v,text);
 u->host=url_piece(text+start,host_end-start);if(!u->host)goto oom;
 if(colon!=(size_t)-1&&!url_port(text+colon+1,length-colon-1,&u->port))return url_authority_error(v,text);
 if(colon==(size_t)-1)u->port=-1;
 return true;
oom:
 fail(v,"java/lang/OutOfMemoryError","URL");
 return false;
}
static bool url_parse(struct dvm *v,const char *text,size_t length,struct url_state *u){
 size_t colon=0;while(colon<length&&text[colon]!=':')colon++;
 if(colon==0||colon==length){dvm__throw(v,"java/net/MalformedURLException","no protocol: %.200s",text);return false;}
 char scheme[16];
 if(colon>=sizeof scheme){dvm__throw(v,"java/net/MalformedURLException","unknown protocol: %.16s",text);return false;}
 for(size_t i=0;i<colon;i++){
  unsigned char c=(unsigned char)text[i];
  bool ok=i?((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='.'||c=='+'||c=='-'):((c>='A'&&c<='Z')||(c>='a'&&c<='z'));
  if(!ok){dvm__throw(v,"java/net/MalformedURLException","invalid protocol: %.16s",text);return false;}
  scheme[i]=(char)(c>='A'&&c<='Z'?c+32:c);
 }
 scheme[colon]=0;
 bool http=!strcmp(scheme,"http"),https=!strcmp(scheme,"https"),file=!strcmp(scheme,"file");
 if(!http&&!https&&!file){dvm__throw(v,"java/net/MalformedURLException","unknown protocol: %s",scheme);return false;}
 u->protocol=url_piece(scheme,colon);if(!u->protocol)goto oom;
 u->default_port=https?443:http?80:-1;u->port=-1;
 size_t start=colon+1;
 if(start+1<length&&text[start]=='/'&&text[start+1]=='/'){
  start+=2;
  size_t end=start;while(end<length&&text[end]!='/'&&text[end]!='?'&&text[end]!='#')end++;
  if(!url_parse_authority(v,text+start,end-start,u,http||https))return false;
  start=end;
 }else{
  /* http(s) is only meaningful in hierarchical form.  Rejecting http:foo is
   * deliberate fail-closed behavior instead of inventing an empty host. */
  if(http||https){dvm__throw(v,"java/net/MalformedURLException","Invalid URL, expected //: %.200s",text);return false;}
  u->host=url_piece("",0);if(!u->host)goto oom;
 }
 size_t path_end=start;while(path_end<length&&text[path_end]!='?'&&text[path_end]!='#')path_end++;
 u->path=url_piece(text+start,path_end-start);if(!u->path)goto oom;
 size_t at=path_end;
 if(at<length&&text[at]=='?'){
  size_t query_end=at+1;while(query_end<length&&text[query_end]!='#')query_end++;
  u->query=url_piece(text+at+1,query_end-at-1);if(!u->query)goto oom;
  at=query_end;
 }
 if(at<length&&text[at]=='#'){u->ref=url_piece(text+at+1,length-at-1);if(!u->ref)goto oom;}
 return true;
oom:
 fail(v,"java/lang/OutOfMemoryError","URL");
 return false;
}
static bool url_string(struct dvm *v,const char *text,union dvm_value *out){
 if(!text){out->l=0;return true;}
 out->l=dvm_new_string(v,text);
 return out->l?true:fail(v,"java/lang/OutOfMemoryError","URL");
}
static dvm_ref url_external(struct dvm *v,const struct url_state *u){
 size_t protocol=strlen(u->protocol),authority=u->authority?strlen(u->authority):0,path=strlen(u->path);
 size_t query=u->query?strlen(u->query):0,ref=u->ref?strlen(u->ref):0;
 bool has_authority=u->authority&&authority>0;
 size_t total=protocol+1+path+(has_authority?2+authority:0)+(u->query?1+query:0)+(u->ref?1+ref:0);
 char *text=malloc(total+1);if(!text)return 0;
 size_t used=0;
 memcpy(text+used,u->protocol,protocol);used+=protocol;
 text[used++]=':';
 if(has_authority){text[used++]='/';text[used++]='/';memcpy(text+used,u->authority,authority);used+=authority;}
 memcpy(text+used,u->path,path);used+=path;
 if(u->query){text[used++]='?';memcpy(text+used,u->query,query);used+=query;}
 if(u->ref){text[used++]='#';memcpy(text+used,u->ref,ref);used+=ref;}
 text[used]=0;
 dvm_ref r=dvm_new_string_n(v,text,used);
 free(text);return r;
}
static bool url_call(struct dvm *v,struct url_state *u,const char *m,const char *sig,const union dvm_value *a,int n,union dvm_value *out){
 (void)a;(void)n;
 if(IS("getProtocol","()Ljava/lang/String;"))return url_string(v,u->protocol,out);
 if(IS("getHost","()Ljava/lang/String;"))return url_string(v,u->host,out);
 if(IS("getPort","()I")){out->i=u->port;return true;}
 if(IS("getDefaultPort","()I")){out->i=u->default_port;return true;}
 if(IS("getAuthority","()Ljava/lang/String;"))return url_string(v,u->authority,out);
 if(IS("getUserInfo","()Ljava/lang/String;"))return url_string(v,u->userinfo,out);
 if(IS("getPath","()Ljava/lang/String;"))return url_string(v,u->path,out);
 if(IS("getQuery","()Ljava/lang/String;"))return url_string(v,u->query,out);
 if(IS("getRef","()Ljava/lang/String;"))return url_string(v,u->ref,out);
 if(IS("getFile","()Ljava/lang/String;")){
  size_t path_length=strlen(u->path),query_length=u->query?strlen(u->query):0,size=path_length+(u->query?1+query_length:0);
  char *text=malloc(size+1);if(!text)return fail(v,"java/lang/OutOfMemoryError","URL");
  memcpy(text,u->path,path_length);
  if(u->query){text[path_length]='?';memcpy(text+path_length+1,u->query,query_length);}
  text[size]=0;out->l=dvm_new_string_n(v,text,size);free(text);
  return out->l?true:fail(v,"java/lang/OutOfMemoryError","URL");
 }
 if(IS("toString","()Ljava/lang/String;")||IS("toExternalForm","()Ljava/lang/String;")){
  out->l=url_external(v,u);
  return out->l?true:fail(v,"java/lang/OutOfMemoryError","URL");
 }
 if(IS("openConnection","()Ljava/net/URLConnection;")||IS("openConnection","(Ljava/net/Proxy;)Ljava/net/URLConnection;")||IS("openStream","()Ljava/io/InputStream;")||IS("toURI","()Ljava/net/URI;")){
  dvm__throw(v,"java/lang/UnsupportedOperationException","java.net.URL.%s: no real URLConnection/URI implementation",m);
  return true;
 }
 return false;
}

static bool bind_socket(struct dvm *v,dvm_ref r){
 struct dvm_object *o=dvm__obj(v,r);if(!o)return fail(v,"java/lang/NullPointerException","socket is null");
 if(o->data)return fail(v,"java/lang/IllegalStateException","socket already initialized");
 struct socket_state *s=calloc(1,sizeof *s);if(!s)return fail(v,"java/lang/OutOfMemoryError","socket allocation");
 s->fd=-1;s->owners=1;s->local_port=-1;o->data=s;o->data_destroy=socket_drop;return true;
}
static int connect_open(const struct sockaddr_storage *sa, int *original_flags)
{
 int fd=socket_counter_socket(sa->ss_family,SOCK_STREAM|SOCK_CLOEXEC,IPPROTO_TCP);
 if(fd<0)return -1;
 *original_flags=fcntl(fd,F_GETFL,0);
 if(*original_flags<0||fcntl(fd,F_SETFL,*original_flags|O_NONBLOCK)<0){close(fd);return -1;}
 return fd;
}
static int connect_addr(int fd,const struct sockaddr_storage *sa,socklen_t len,int timeout,int original_flags){
 struct socket_counter_stamp socket_started=socket_counter_begin();
 perf_ffrt_io(0);int error=0,rc=socket_counter_connect(fd,(const struct sockaddr *)sa,len);
 if(rc<0&&errno==EINPROGRESS){struct timespec deadline;if(timeout>0)deadline_after(&deadline,timeout);
  rc=wait_fd(fd,POLLOUT,timeout,&deadline);
  if(rc==0)error=ETIMEDOUT;else if(rc<0)error=errno;else{socklen_t l=sizeof error;if(socket_counter_getsockopt(fd,SOL_SOCKET,SO_ERROR,&error,&l)<0)error=errno;}}
 else if(rc<0)error=errno;
 if(!error&&fcntl(fd,F_SETFL,original_flags)<0)error=errno;
 socket_counter_connect_result(socket_started,fd,error);
 return error;
}
static bool connect_to(struct dvm *v,dvm_ref self,const struct inet_state *in,const char *host,int port,int timeout){
 struct socket_state *s=state(v,self);
 if(!s)return fail(v,"java/lang/IllegalStateException","socket not initialized");
 if(s->closed)return fail(v,"java/net/SocketException","Socket is closed");
 if(s->connected||s->fd>=0)return fail(v,"java/net/SocketException","already connected");
 if(port<0||port>65535)return fail(v,"java/lang/IllegalArgumentException","port out of range");
 if(timeout<0)return fail(v,"java/lang/IllegalArgumentException","connect: timeout can't be negative");
 /* A connect has no fd yet, but it is still an active blocking operation.
  * Count it so close() cannot race the descriptor handoff or free the state. */
 s->io_active++;
 struct sockaddr_storage sa;socklen_t len=to_sockaddr(in,port,&sa);char text[INET6_ADDRSTRLEN];literal_text(in,text,sizeof text);
 {static unsigned logged;if(logged++<32){int saved=errno;FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt","a");if(f){dprintf(fileno(f),"NET_JAVA_TCP stage=enter host=%s ip=%s port=%d timeout=%d external=%d steps=%llu thread=%u\n",host?host:"-",text,port,timeout,external_allowed,(unsigned long long)v->call_steps,v->cur_thread);fclose(f);}errno=saved;}}
 if(!external_allowed&&!loopback((const struct sockaddr *)&sa)){
  sdk_trace_fprintf(stderr,"NETWORK_POLICY blocked_connect host=%s address=%s port=%d external_network=disabled\n",host?host:"-",text,port);
  s->io_active--;
  dvm__throw(v,"java/net/ConnectException","external network disabled by diagnostic policy: /%s (port %d)",text,port);return true;
 }
 char *copy=host?strdup(host):NULL;if(host&&!copy){s->io_active--;return fail(v,"java/lang/OutOfMemoryError","host");}
 int original_flags=0;int fd=connect_open(&sa,&original_flags);
 if(fd<0){s->io_active--;dvm__throw(v,"java/net/ConnectException","failed to create socket for /%s (port %d): %s",text,port,strerror(errno));return true;}
 s->fd=fd;
 unsigned depth=dvm_gil_unlock_all(v);int e=connect_addr(fd,&sa,len,timeout,original_flags);dvm_gil_relock(v,depth);
 {static unsigned logged;if(logged++<32){int saved=errno;FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt","a");if(f){dprintf(fileno(f),"NET_JAVA_TCP stage=exit host=%s ip=%s port=%d error=%d steps=%llu thread=%u\n",host?host:"-",text,port,e,(unsigned long long)v->call_steps,v->cur_thread);fclose(f);}errno=saved;}}
 if(s->closed){s->fd=-1;close(fd);free(copy);s->io_active--;return fail(v,"java/net/SocketException","Socket closed");}
 if(e){s->fd=-1;close(fd);free(copy);s->io_active--;
  if(e==ETIMEDOUT)dvm__throw(v,"java/net/SocketTimeoutException","failed to connect to /%s (port %d) after %dms",text,port,timeout);
  else dvm__throw(v,e==EHOSTUNREACH?"java/net/NoRouteToHostException":"java/net/ConnectException","failed to connect to /%s (port %d): %s",text,port,strerror(e));
  return true;}
 s->connected=true;s->peer=sa;s->peer_len=len;s->port=port;free(s->host);s->host=copy;
 struct sockaddr_storage local;socklen_t local_len=sizeof local;
 if(!getsockname(fd,(struct sockaddr *)&local,&local_len))s->local_port=ntohs(local.ss_family==AF_INET?((struct sockaddr_in *)&local)->sin_port:((struct sockaddr_in6 *)&local)->sin6_port);
 int one=1;if(s->nodelay)setsockopt(fd,IPPROTO_TCP,TCP_NODELAY,&one,sizeof one);if(s->keepalive)setsockopt(fd,SOL_SOCKET,SO_KEEPALIVE,&one,sizeof one);
 s->io_active--;
 return true;
}
static bool connect_host(struct dvm *v,dvm_ref self,dvm_ref name,int port){
 union dvm_value in={0};if(!all_by_name(v,name,&in,true)||dvm_exception(v))return true;
 struct inet_state *st=inet(v,in.l);return st?connect_to(v,self,st,dvm_string_utf8(v,name),port,0):true;
}
static bool connect_address(struct dvm *v,dvm_ref self,dvm_ref target,int timeout){
 struct address_state *ad=address(v,target);if(!ad)return fail(v,"java/lang/IllegalArgumentException","Unsupported address type");
 struct inet_state *in=inet(v,ad->inet);
 if(!in){dvm__throw(v,"java/net/UnknownHostException","%s",ad->host?ad->host:"");return true;}
 return connect_to(v,self,in,ad->host?ad->host:in->host,ad->port,timeout);
}

static bool check_range(struct dvm *v,dvm_ref array,int off,int len){
 struct dvm_object *o=dvm__obj(v,array);
 if(!o)return fail(v,"java/lang/NullPointerException","byte array is null"),false;
 if(o->kind!=DVM_OBJ_ARRAY||o->elem_kind!='B')return fail(v,"java/lang/IllegalArgumentException","not a byte array"),false;
 if(off<0||len<0||(uint32_t)off>o->length||(uint32_t)len>o->length-(uint32_t)off){dvm__throw(v,"java/lang/IndexOutOfBoundsException","off=%d len=%d length=%u",off,len,o->length);return false;}
 return true;
}
/* Blocking read with the lock released; Java SocketInputStream semantics. */
static bool stream_read(struct dvm *v,struct socket_state *s,dvm_ref array,int off,int len,union dvm_value *out){
 if(s->closed)return fail(v,"java/net/SocketException","Socket closed");
 if(s->input_shutdown){out->i=-1;return true;}
 if(!len){out->i=0;return true;}
 size_t want=len>IO_CHUNK?IO_CHUNK:(size_t)len;unsigned char *tmp=malloc(want);if(!tmp)return fail(v,"java/lang/OutOfMemoryError","read buffer");
 int fd=io_begin(s);if(fd<0){free(tmp);return fail(v,"java/net/SocketException","Socket closed");}
 int timeout=s->timeout_ms;struct timespec deadline;if(timeout>0)deadline_after(&deadline,timeout);
 ssize_t got=-1;int e=0;bool timed_out=false;unsigned depth=dvm_gil_unlock_all(v);
 for(;;){int rc=wait_fd(fd,POLLIN,timeout,&deadline);if(rc==0){timed_out=true;break;}if(rc<0){e=errno;break;}
  perf_ffrt_io(1);got=socket_counter_recv(fd,tmp,want,MSG_DONTWAIT);if(got<0&&(errno==EINTR||errno==EAGAIN||errno==EWOULDBLOCK))continue;if(got<0)e=errno;break;}
 dvm_gil_relock(v,depth);io_end(s);
 if(s->closed){free(tmp);return fail(v,"java/net/SocketException","Socket closed");}
 if(timed_out){free(tmp);return fail(v,"java/net/SocketTimeoutException","Read timed out");}
 if(e||got<0){free(tmp);dvm__throw(v,"java/net/SocketException","%s",e==ECONNRESET?"Connection reset":strerror(e?e:EIO));return true;}
 if(!got){free(tmp);out->i=-1;return true;}
 struct dvm_object *o=dvm__obj(v,array);
 if(!o||(uint32_t)off+(uint32_t)got>o->length){free(tmp);return fail(v,"java/lang/IllegalStateException","read target changed");}
 memcpy((unsigned char *)o->data+off,tmp,(size_t)got);free(tmp);dvm__note_io_progress(v,(size_t)got);out->i=(int)got;return true;
}
/* Socket writes have no timeout in Java; close() from another thread wakes them. */
static bool stream_write(struct dvm *v,struct socket_state *s,dvm_ref array,int off,int len){
 while(len>0){
  if(s->closed)return fail(v,"java/net/SocketException","Socket closed");
  if(s->output_shutdown)return fail(v,"java/net/SocketException","Socket output is shutdown");
  size_t chunk=len>IO_CHUNK?IO_CHUNK:(size_t)len;unsigned char *tmp=malloc(chunk);if(!tmp)return fail(v,"java/lang/OutOfMemoryError","write buffer");
  memcpy(tmp,(unsigned char *)dvm_array_data(v,array)+off,chunk);
  int fd=io_begin(s);if(fd<0){free(tmp);return fail(v,"java/net/SocketException","Socket closed");}
  size_t sent=0;int e=0;unsigned depth=dvm_gil_unlock_all(v);
  while(sent<chunk){perf_ffrt_io(2);ssize_t n=socket_counter_send(fd,tmp+sent,chunk-sent,MSG_NOSIGNAL|MSG_DONTWAIT);
   if(n>0){sent+=(size_t)n;continue;}
   if(n<0&&errno==EINTR)continue;
   if(n<0&&(errno==EAGAIN||errno==EWOULDBLOCK)){if(wait_fd(fd,POLLOUT,0,NULL)<0){e=errno;break;}continue;}
   e=n<0?errno:EPIPE;break;}
  dvm_gil_relock(v,depth);io_end(s);free(tmp);
  if(s->closed)return fail(v,"java/net/SocketException","Socket closed");
  if(e){dvm__throw(v,"java/net/SocketException","%s",e==EPIPE?"Broken pipe":e==ECONNRESET?"Connection reset":strerror(e));return true;}
  dvm__note_io_progress(v,chunk);off+=(int)chunk;len-=(int)chunk;
 }
 return true;
}
static bool stream_object(struct dvm *v,dvm_ref self,struct socket_state *s,bool in,union dvm_value *out){
 if(s->closed)return fail(v,"java/net/SocketException","Socket is closed");
 if(!s->connected)return fail(v,"java/net/SocketException","Socket is not connected");
 if(in&&s->input_shutdown)return fail(v,"java/net/SocketException","Socket input is shutdown");
 if(!in&&s->output_shutdown)return fail(v,"java/net/SocketException","Socket output is shutdown");
 dvm_ref *cached=in?&s->input:&s->output;if(*cached){out->l=*cached;return true;}
 struct stream_ref *r=calloc(1,sizeof *r);if(!r)return fail(v,"java/lang/OutOfMemoryError","socket stream");
 dvm_ref obj=dvm_new_object(v,dvm_find_class_or_external(v,in?"java/net/SocketInputStream":"java/net/SocketOutputStream"));
 if(!obj){free(r);return fail(v,"java/lang/OutOfMemoryError","socket stream object");}
 r->s=s;s->owners++;if(!attach(v,obj,r,in?input_drop:output_drop))return fail(v,"java/lang/IllegalStateException","socket stream payload");
 (void)self;*cached=obj;out->l=obj;return true;
}
static bool input_call(struct dvm *v,struct socket_state *s,const char *m,const char *sig,const union dvm_value *a,int n,union dvm_value *out){
 if(IS("read","([BII)I")&&n==3){if(!check_range(v,a[0].l,a[1].i,a[2].i))return true;return stream_read(v,s,a[0].l,a[1].i,a[2].i,out);}
 if(IS("read","([B)I")&&n==1){struct dvm_object *o=dvm__obj(v,a[0].l);if(!o)return fail(v,"java/lang/NullPointerException","byte array is null");if(!check_range(v,a[0].l,0,(int)o->length))return true;return stream_read(v,s,a[0].l,0,(int)o->length,out);}
 if(IS("read","()I")&&n==0){dvm_ref one=dvm_new_array(v,'B',"B",1);union dvm_value got={0};if(!one)return fail(v,"java/lang/OutOfMemoryError","read byte");
  if(!stream_read(v,s,one,0,1,&got)||dvm_exception(v))return true;out->i=got.i<0?-1:((unsigned char *)dvm_array_data(v,one))[0];dvm__discard_temporary(v,one);return true;}
 if(IS("available","()I")&&n==0){if(s->closed)return fail(v,"java/net/SocketException","Socket closed");int pending=0;if(s->input_shutdown||s->fd<0||ioctl(s->fd,FIONREAD,&pending)<0)pending=0;out->i=pending;return true;}
 if(IS("skip","(J)J")&&n==1){if(a[0].j<=0){out->j=0;return true;}int want=a[0].j>8192?8192:(int)a[0].j;dvm_ref tmp=dvm_new_array(v,'B',"B",(uint32_t)want);union dvm_value got={0};if(!tmp)return fail(v,"java/lang/OutOfMemoryError","skip buffer");
  if(!stream_read(v,s,tmp,0,want,&got)||dvm_exception(v))return true;out->j=got.i<0?0:got.i;dvm__discard_temporary(v,tmp);return true;}
 if(IS("markSupported","()Z")){out->i=0;return true;}
 if(IS("close","()V")){state_close(s);return true;}
 return false;
}
static bool output_call(struct dvm *v,struct socket_state *s,const char *m,const char *sig,const union dvm_value *a,int n,union dvm_value *out){
 (void)out;
 if(IS("write","([BII)V")&&n==3){if(!check_range(v,a[0].l,a[1].i,a[2].i))return true;return stream_write(v,s,a[0].l,a[1].i,a[2].i);}
 if(IS("write","([B)V")&&n==1){struct dvm_object *o=dvm__obj(v,a[0].l);if(!o)return fail(v,"java/lang/NullPointerException","byte array is null");if(!check_range(v,a[0].l,0,(int)o->length))return true;return stream_write(v,s,a[0].l,0,(int)o->length);}
 if(IS("write","(I)V")&&n==1){dvm_ref one=dvm_new_array(v,'B',"B",1);if(!one)return fail(v,"java/lang/OutOfMemoryError","write byte");((unsigned char *)dvm_array_data(v,one))[0]=(unsigned char)a[0].i;stream_write(v,s,one,0,1);dvm__discard_temporary(v,one);return true;}
 if(IS("flush","()V")){if(s->closed)return fail(v,"java/net/SocketException","Socket closed");return true;}
 if(IS("close","()V")){state_close(s);return true;}
 return false;
}
static bool socket_option(struct dvm *v,struct socket_state *s,int level,int name,bool *field,bool value){
 if(s->closed)return fail(v,"java/net/SocketException","Socket is closed");
 int on=value;if(s->fd>=0&&setsockopt(s->fd,level,name,&on,sizeof on)<0){dvm__throw(v,"java/net/SocketException","setsockopt: %s",strerror(errno));return true;}
 *field=value;return true;
}
static bool socket_call(struct dvm *v,dvm_ref self,struct socket_state *s,const char *m,const char *sig,const union dvm_value *a,int n,union dvm_value *out){
 if(IS("connect","(Ljava/net/SocketAddress;)V")&&n==1)return connect_address(v,self,a[0].l,0);
 if(IS("connect","(Ljava/net/SocketAddress;I)V")&&n==2)return connect_address(v,self,a[0].l,a[1].i);
 if(IS("close","()V")&&n==0){state_close(s);return true;}
 if(IS("isConnected","()Z")){out->i=s->connected;return true;}
 if(IS("isBound","()Z")){out->i=s->local_port>=0;return true;}
 if(IS("isClosed","()Z")){out->i=s->closed;return true;}
 if(IS("getInputStream","()Ljava/io/InputStream;"))return stream_object(v,self,s,true,out);
 if(IS("getOutputStream","()Ljava/io/OutputStream;"))return stream_object(v,self,s,false,out);
 if(IS("setSoTimeout","(I)V")&&n==1){if(s->closed)return fail(v,"java/net/SocketException","Socket is closed");if(a[0].i<0)return fail(v,"java/lang/IllegalArgumentException","timeout can't be negative");s->timeout_ms=a[0].i;return true;}
 if(IS("getSoTimeout","()I")){if(s->closed)return fail(v,"java/net/SocketException","Socket is closed");out->i=s->timeout_ms;return true;}
 if(IS("setTcpNoDelay","(Z)V")&&n==1)return socket_option(v,s,IPPROTO_TCP,TCP_NODELAY,&s->nodelay,a[0].i!=0);
 if(IS("getTcpNoDelay","()Z")){if(s->closed)return fail(v,"java/net/SocketException","Socket is closed");out->i=s->nodelay;return true;}
 if(IS("setKeepAlive","(Z)V")&&n==1)return socket_option(v,s,SOL_SOCKET,SO_KEEPALIVE,&s->keepalive,a[0].i!=0);
 if(IS("getKeepAlive","()Z")){if(s->closed)return fail(v,"java/net/SocketException","Socket is closed");out->i=s->keepalive;return true;}
 if(IS("shutdownInput","()V")||IS("shutdownOutput","()V")){bool in=!strcmp(m,"shutdownInput");bool *flag=in?&s->input_shutdown:&s->output_shutdown;
  if(s->closed)return fail(v,"java/net/SocketException","Socket is closed");if(!s->connected)return fail(v,"java/net/SocketException","Socket is not connected");
  if(*flag)return fail(v,"java/net/SocketException",in?"Socket input is already shutdown":"Socket output is already shutdown");
  if(s->fd>=0&&shutdown(s->fd,in?SHUT_RD:SHUT_WR)<0&&errno!=ENOTCONN){dvm__throw(v,"java/net/SocketException","shutdown: %s",strerror(errno));return true;}*flag=true;return true;}
 if(IS("isInputShutdown","()Z")){out->i=s->input_shutdown;return true;}
 if(IS("isOutputShutdown","()Z")){out->i=s->output_shutdown;return true;}
 if(IS("getInetAddress","()Ljava/net/InetAddress;")){out->l=s->connected?socket_state_remote_address(v,s):0;return true;}
 if(IS("getPort","()I")){out->i=s->connected?s->port:0;return true;}
 if(IS("getLocalPort","()I")){out->i=s->local_port;return true;}
 if(IS("getRemoteSocketAddress","()Ljava/net/SocketAddress;")){if(!s->connected){out->l=0;return true;}dvm_ref in=socket_state_remote_address(v,s);out->l=in?new_address(v,in,NULL,s->port):0;return true;}
 if(IS("toString","()Ljava/lang/String;")){char line[256],text[INET6_ADDRSTRLEN];if(!s->connected){out->l=dvm_new_string(v,"Socket[unconnected]");return true;}
  struct inet_state in;from_sockaddr(&s->peer,&in);literal_text(&in,text,sizeof text);snprintf(line,sizeof line,"Socket[address=%s/%s,port=%d,localPort=%d]",s->host?s->host:"",text,s->port,s->local_port);out->l=dvm_new_string(v,line);return true;}
 return false;
}
dvm_ref socket_state_remote_address(struct dvm *v,struct socket_state *s){struct inet_state in;from_sockaddr(&s->peer,&in);normalize(&in);return new_inet(v,&in,s->host);}

static void declare(struct dvm *v,const char *name,const char *parent,const char *const *ifaces,int count){
 struct dvm_class *c=dvm_find_class_or_external(v,name),*p=dvm_find_class_or_external(v,parent);
 if(!c||!c->external)return;if(p)c->super=p;
 if(count&&!c->ifaces){c->ifaces=calloc((size_t)count,sizeof *c->ifaces);if(!c->ifaces)return;c->nifaces=count;for(int i=0;i<count;i++)c->ifaces[i]=dvm_find_class_or_external(v,ifaces[i]);}
}
void socket_runtime_vm_start(struct dvm *v){
 active=v;static const char *const serial[]={"java/io/Serializable"},*const closeable[]={"java/io/Closeable","java/lang/AutoCloseable"},*const flushable[]={"java/io/Closeable","java/lang/AutoCloseable","java/io/Flushable"};
 declare(v,"java/net/InetAddress","java/lang/Object",serial,1);declare(v,"java/net/Inet4Address","java/net/InetAddress",NULL,0);declare(v,"java/net/Inet6Address","java/net/InetAddress",NULL,0);
 declare(v,"java/net/SocketAddress","java/lang/Object",serial,1);declare(v,"java/net/InetSocketAddress","java/net/SocketAddress",NULL,0);
 declare(v,"java/net/Socket","java/lang/Object",closeable,2);declare(v,"java/io/InputStream","java/lang/Object",closeable,2);declare(v,"java/io/OutputStream","java/lang/Object",flushable,3);
 declare(v,"java/net/SocketInputStream","java/io/InputStream",NULL,0);declare(v,"java/net/SocketOutputStream","java/io/OutputStream",NULL,0);
 declare(v,"java/net/URL","java/lang/Object",serial,1);
}
void socket_runtime_cleanup(void){
 if(idn_converter&&idn_close)idn_close(idn_converter);
 idn_converter=NULL;active=NULL;
}

bool socket_runtime_call(struct dvm *v,const char *c,const char *m,const char *sig,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 if(v!=active)return false;
 if(self){
  struct stream_ref *r;struct inet_state *in;struct address_state *ad;struct socket_state *s;struct url_state *url;
  if((r=payload(v,self,input_drop)))return input_call(v,r->s,m,sig,a,n,out);
  if((r=payload(v,self,output_drop)))return output_call(v,r->s,m,sig,a,n,out);
  if((in=inet(v,self)))return inet_call(v,in,m,sig,a,n,out);
  if((ad=address(v,self)))return address_call(v,ad,m,sig,a,n,out);
  if((s=state(v,self)))return socket_call(v,self,s,m,sig,a,n,out);
  if((url=url_state_of(v,self)))return url_call(v,url,m,sig,a,n,out);
 }
 if(!strcmp(c,"javax/net/SocketFactory")){
  if(IS("getDefault","()Ljavax/net/SocketFactory;")&&n==0){out->l=dvm_new_object(v,dvm_find_class_or_external(v,c));return out->l?true:fail(v,"java/lang/OutOfMemoryError","factory allocation");}
  if(IS("createSocket","()Ljava/net/Socket;")&&n==0){out->l=dvm_new_object(v,dvm_find_class_or_external(v,"java/net/Socket"));if(!out->l)return fail(v,"java/lang/OutOfMemoryError","socket allocation");return bind_socket(v,out->l);}
  if(IS("createSocket","(Ljava/lang/String;I)Ljava/net/Socket;")&&n==2){out->l=dvm_new_object(v,dvm_find_class_or_external(v,"java/net/Socket"));if(!out->l)return fail(v,"java/lang/OutOfMemoryError","socket allocation");if(!bind_socket(v,out->l)||dvm_exception(v))return true;return connect_host(v,out->l,a[0].l,a[1].i);}
  if(IS("createSocket","(Ljava/net/InetAddress;I)Ljava/net/Socket;")&&n==2){struct inet_state *t=inet(v,a[0].l);if(!t)return fail(v,"java/lang/NullPointerException","address");out->l=dvm_new_object(v,dvm_find_class_or_external(v,"java/net/Socket"));if(!out->l)return fail(v,"java/lang/OutOfMemoryError","socket allocation");if(!bind_socket(v,out->l)||dvm_exception(v))return true;return connect_to(v,out->l,t,t->host,a[1].i,0);}
  return false;
 }
 if(!strcmp(c,"java/net/Socket")){
  if(IS("<init>","()V")&&n==0)return bind_socket(v,self);
  if(IS("<init>","(Ljava/lang/String;I)V")&&n==2){if(!bind_socket(v,self)||dvm_exception(v))return true;return connect_host(v,self,a[0].l,a[1].i);}
  if(IS("<init>","(Ljava/net/InetAddress;I)V")&&n==2){struct inet_state *t=inet(v,a[0].l);if(!t)return fail(v,"java/lang/NullPointerException","address");if(!bind_socket(v,self)||dvm_exception(v))return true;return connect_to(v,self,t,t->host,a[1].i,0);}
  return false;
 }
 if(!strcmp(c,"java/net/InetAddress")||!strcmp(c,"java/net/Inet4Address")||!strcmp(c,"java/net/Inet6Address")){
  if(IS("getAllByName","(Ljava/lang/String;)[Ljava/net/InetAddress;")&&n==1)return all_by_name(v,a[0].l,out,false);
  if(IS("getByName","(Ljava/lang/String;)Ljava/net/InetAddress;")&&n==1)return all_by_name(v,a[0].l,out,true);
  if((IS("getByAddress","([B)Ljava/net/InetAddress;")&&n==1)||(IS("getByAddress","(Ljava/lang/String;[B)Ljava/net/InetAddress;")&&n==2)){
   dvm_ref bytes=a[n-1].l;struct dvm_object *o=dvm__obj(v,bytes);if(!o)return fail(v,"java/lang/NullPointerException","addr is null");
   if(o->kind!=DVM_OBJ_ARRAY||o->elem_kind!='B'||(o->length!=4&&o->length!=16))return fail(v,"java/net/UnknownHostException","addr is of illegal length");
   struct inet_state st={0};st.family=o->length==4?AF_INET:AF_INET6;memcpy(st.bytes,o->data,o->length);normalize(&st);
   bool bad=false;const char *host=n==2&&a[0].l?utf8_arg(v,a[0].l,&bad):NULL;if(bad)return fail(v,"java/net/UnknownHostException","NUL in host name");
   out->l=new_inet(v,&st,host);return true;}
  return false;
 }
 if(!strcmp(c,"java/net/InetSocketAddress")){
  if(IS("<init>","(Ljava/lang/String;I)V")&&n==2){
   if(!a[0].l)return fail(v,"java/lang/IllegalArgumentException","hostname can't be null");
   if(a[1].i<0||a[1].i>65535){dvm__throw(v,"java/lang/IllegalArgumentException","port out of range:%d",a[1].i);return true;}
   bool bad=false;const char *host=utf8_arg(v,a[0].l,&bad);if(bad)return fail(v,"java/lang/IllegalArgumentException","NUL in host");
   union dvm_value resolved={0};all_by_name(v,a[0].l,&resolved,true);
   if(dvm_exception(v)){struct dvm_class *k=dvm_object_class(v,dvm_exception(v));if(!k||strcmp(k->name,"java/net/UnknownHostException"))return true;dvm_clear_exception(v);resolved.l=0;}
   struct address_state *ad=calloc(1,sizeof *ad);if(!ad||!(ad->host=strdup(dvm_string_utf8(v,a[0].l)))){free(ad);return fail(v,"java/lang/OutOfMemoryError","address");}
   ad->inet=resolved.l;ad->port=a[1].i;if(!attach(v,self,ad,address_drop))return fail(v,"java/lang/IllegalStateException","address already initialized");(void)host;return true;}
  if(IS("<init>","(Ljava/net/InetAddress;I)V")&&n==2){
   if(a[1].i<0||a[1].i>65535){dvm__throw(v,"java/lang/IllegalArgumentException","port out of range:%d",a[1].i);return true;}
   dvm_ref target=a[0].l;if(!target){struct inet_state any={0};any.family=AF_INET6;if(!(target=new_inet(v,&any,NULL)))return true;}
   else if(!inet(v,target))return fail(v,"java/lang/IllegalArgumentException","unsupported InetAddress");
   struct address_state *ad=calloc(1,sizeof *ad);if(!ad)return fail(v,"java/lang/OutOfMemoryError","address");ad->inet=target;ad->port=a[1].i;
   if(!attach(v,self,ad,address_drop))return fail(v,"java/lang/IllegalStateException","address already initialized");return true;}
  if(IS("createUnresolved","(Ljava/lang/String;I)Ljava/net/InetSocketAddress;")&&n==2){
   if(!a[0].l)return fail(v,"java/lang/IllegalArgumentException","hostname can't be null");
   if(a[1].i<0||a[1].i>65535){dvm__throw(v,"java/lang/IllegalArgumentException","port out of range:%d",a[1].i);return true;}
   out->l=new_address(v,0,dvm_string_utf8(v,a[0].l),a[1].i);return true;}
  return false;
 }
 if(!strcmp(c,"java/net/IDN")){
  if(IS("toASCII","(Ljava/lang/String;)Ljava/lang/String;")&&n==1)return idn_to_ascii(v,a[0].l,out);
  return false;
 }
 if(!strcmp(c,"java/net/URLEncoder")){
  if(IS("encode","(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;")&&n==2)return urlencoder_encode(v,a[0].l,a[1].l,out);
  return false;
 }
 if(!strcmp(c,"java/net/URL")){
  if(IS("<init>","(Ljava/lang/String;)V")&&n==1){
   if(!a[0].l)return fail(v,"java/lang/NullPointerException","spec");
   if(url_state_of(v,self))return fail(v,"java/lang/IllegalStateException","URL already initialized");
   const char *text=dvm_string_utf8(v,a[0].l);size_t length=dvm_string_utf8_length(v,a[0].l);
   if(!text)return fail(v,"java/lang/NullPointerException","spec");
   struct url_state *state=calloc(1,sizeof *state);if(!state)return fail(v,"java/lang/OutOfMemoryError","URL");
   /* A raw NUL would silently truncate the C-string parser; the DVM normally
    * stores U+0000 as the MUTF-8 pair C0 80, which stays in the data. */
   if(strlen(text)!=length){url_drop(state);dvm__throw(v,"java/net/MalformedURLException","NUL in URL");return true;}
   if(!url_parse(v,text,length,state)){url_drop(state);return true;}
   if(!attach(v,self,state,url_drop))return fail(v,"java/lang/IllegalStateException","URL already initialized");
   return true;
  }
  return false;
 }
 return false;
}

/* ---- selftest: loopback only; never contacts an external address ---- */
struct closer { struct dvm *v; dvm_ref socket; };
static void *close_later(void *p){struct closer *c=p;struct timespec t={0,150000000};nanosleep(&t,NULL);dvm_gil_acquire(c->v);union dvm_value o={0};socket_runtime_call(c->v,"java/net/Socket","close","()V",c->socket,NULL,0,&o);dvm_gil_release(c->v);return NULL;}
static bool thrown(struct dvm *v,const char *name){dvm_ref e=dvm_exception(v);struct dvm_class *k=e?dvm_object_class(v,e):NULL;bool ok=k&&!strcmp(k->name,name);dvm_clear_exception(v);return ok;}
static dvm_ref call_obj(struct dvm *v,const char *c,const char *m,const char *sig,dvm_ref self,const union dvm_value *a,int n){union dvm_value out={0};return socket_runtime_call(v,c,m,sig,self,a,n,&out)&&!dvm_exception(v)?out.l:0;}
static int call_int(struct dvm *v,const char *c,const char *m,const char *sig,dvm_ref self,const union dvm_value *a,int n,bool *ok){union dvm_value out={0};*ok=*ok&&socket_runtime_call(v,c,m,sig,self,a,n,&out)&&!dvm_exception(v);return out.i;}
static dvm_ref connect_loopback(struct dvm *v,int port){
 dvm_ref pending=dvm_exception(v);
 if(pending){char why[256]={0};dvm_describe_exception(v,pending,why,sizeof why);sdk_trace_fprintf(stderr,"SOCKET_DIAG pending_exception=%s\n",why);dvm_clear_exception(v);}
 union dvm_value arg[2];dvm_ref client=call_obj(v,"javax/net/SocketFactory","createSocket","()Ljava/net/Socket;",dvm_new_object(v,dvm_find_class_or_external(v,"javax/net/SocketFactory")),NULL,0);
 dvm_ref target=dvm_new_object(v,dvm_find_class_or_external(v,"java/net/InetSocketAddress"));
 if(!client||!target){sdk_trace_fprintf(stderr,"SOCKET_DIAG factory client=%d target=%d\n",client!=0,target!=0);return 0;}
 arg[0].l=dvm_new_string(v,"127.0.0.1");arg[1].i=port;union dvm_value out={0};
 if(!socket_runtime_call(v,"java/net/InetSocketAddress","<init>","(Ljava/lang/String;I)V",target,arg,2,&out)||dvm_exception(v)){
  char why[256]={0};dvm_ref e=dvm_exception(v);if(e)dvm_describe_exception(v,e,why,sizeof why);sdk_trace_fprintf(stderr,"SOCKET_DIAG inet_init_exception=%s\n",why);return 0;}
 arg[0].l=target;arg[1].i=1000;
 if(!socket_runtime_call(v,"java/net/Socket","connect","(Ljava/net/SocketAddress;I)V",client,arg,2,&out)||dvm_exception(v)){
  char why[256]={0};dvm_ref e=dvm_exception(v);if(e)dvm_describe_exception(v,e,why,sizeof why);sdk_trace_fprintf(stderr,"SOCKET_DIAG connect_exception=%s\n",why);return 0;}
 return client;
}
static dvm_ref url_make(struct dvm *v,const char *spec){
 dvm_ref object=dvm_new_object(v,dvm_find_class_or_external(v,"java/net/URL"));
 union dvm_value a[1]={{0}},o={0};
 if(!object)return 0;
 a[0].l=dvm_new_string(v,spec);
 if(!a[0].l)return 0;
 if(!socket_runtime_call(v,"java/net/URL","<init>","(Ljava/lang/String;)V",object,a,1,&o))return 0;
 return dvm_exception(v)?0:object;
}
static bool url_text(struct dvm *v,dvm_ref r,const char *want){const char *text=r?dvm_string_utf8(v,r):NULL;return text&&!strcmp(text,want);}
bool socket_runtime_selftest(struct dvm *v){
 if(v!=active)return false;
 int listener=socket(AF_INET,SOCK_STREAM|SOCK_CLOEXEC,0);if(listener<0)return false;
 struct sockaddr_in addr={.sin_family=AF_INET,.sin_addr={.s_addr=htonl(INADDR_LOOPBACK)},.sin_port=0};socklen_t len=sizeof addr;
 bool ok=bind(listener,(struct sockaddr *)&addr,len)==0&&listen(listener,4)==0&&getsockname(listener,(struct sockaddr *)&addr,&len)==0;
 int port=ntohs(addr.sin_port),peer=-1;bool streams=false,timeout=false,eof=false,concurrent=false,dns=false,policy=false,unresolved=false;
 sdk_trace_fprintf(stderr,"SOCKET_DIAG listener_ok=%d port=%d\n",ok,port);
 dvm_ref client=ok?connect_loopback(v,port):0;ok=ok&&client;
 sdk_trace_fprintf(stderr,"SOCKET_DIAG client=%d\n",client!=0);
 struct pollfd p={.fd=listener,.events=POLLIN};ok=ok&&poll(&p,1,1000)==1;if(ok)peer=accept(listener,NULL,NULL);ok=ok&&peer>=0;
 if(ok){
  dvm_ref in=call_obj(v,"java/net/Socket","getInputStream","()Ljava/io/InputStream;",client,NULL,0),outs=call_obj(v,"java/net/Socket","getOutputStream","()Ljava/io/OutputStream;",client,NULL,0);
  dvm_ref again=call_obj(v,"java/net/Socket","getInputStream","()Ljava/io/InputStream;",client,NULL,0);
  dvm_ref buffer=dvm_new_array(v,'B',"B",8);union dvm_value arg[3];
  ok=in&&outs&&again==in&&buffer;
  if(ok){memcpy(dvm_array_data(v,buffer),"ping",4);arg[0].l=buffer;arg[1].i=0;arg[2].i=4;union dvm_value o={0};
   ok=socket_runtime_call(v,"java/io/OutputStream","write","([BII)V",outs,arg,3,&o)&&!dvm_exception(v);
   char got[8]={0};p.fd=peer;p.events=POLLIN;ok=ok&&poll(&p,1,1000)==1&&recv(peer,got,sizeof got,0)==4&&!memcmp(got,"ping",4)&&send(peer,"pong!",5,MSG_NOSIGNAL)==5;
   arg[0].l=buffer;arg[1].i=1;arg[2].i=7;int n=call_int(v,"java/io/InputStream","read","([BII)I",in,arg,3,&ok);
   streams=ok&&n>=1&&n<=5&&!memcmp((char *)dvm_array_data(v,buffer)+1,"pong!",(size_t)n);
   /* Drain the rest so the following timeout test starts from an empty socket. */
   while(streams&&n<5){int more=call_int(v,"java/io/InputStream","read","([BII)I",in,arg,3,&ok);if(more<=0)break;n+=more;}
   arg[0].i=200;union dvm_value set={0};ok=ok&&socket_runtime_call(v,"java/net/Socket","setSoTimeout","(I)V",client,arg,1,&set)&&!dvm_exception(v);
   arg[0].l=buffer;arg[1].i=0;arg[2].i=8;socket_runtime_call(v,"java/io/InputStream","read","([BII)I",in,arg,3,&o);timeout=thrown(v,"java/net/SocketTimeoutException");
   shutdown(peer,SHUT_WR);arg[0].i=1000;socket_runtime_call(v,"java/net/Socket","setSoTimeout","(I)V",client,arg,1,&set);
   arg[0].l=buffer;arg[1].i=0;arg[2].i=8;bool fine=true;int end=call_int(v,"java/io/InputStream","read","([BII)I",in,arg,3,&fine);eof=fine&&end==-1;
   arg[1].i=-1;socket_runtime_call(v,"java/io/InputStream","read","([BII)I",in,arg,3,&o);ok=ok&&thrown(v,"java/lang/IndexOutOfBoundsException");}
 }
 if(peer>=0){close(peer);peer=-1;}
 /* A read blocked with the lock released is woken by close() from another thread. */
 dvm_ref second=ok?connect_loopback(v,port):0;
 if(second&&(peer=accept(listener,NULL,NULL))>=0){
  dvm_ref in=call_obj(v,"java/net/Socket","getInputStream","()Ljava/io/InputStream;",second,NULL,0);dvm_ref buffer=dvm_new_array(v,'B',"B",4);
  struct closer job={v,second};pthread_t t;
  if(in&&buffer&&!pthread_create(&t,NULL,close_later,&job)){union dvm_value arg[3]={{.l=buffer},{.i=0},{.i=4}},o={0};
   socket_runtime_call(v,"java/io/InputStream","read","([BII)I",in,arg,3,&o);concurrent=thrown(v,"java/net/SocketException");
   unsigned depth=dvm_gil_unlock_all(v);pthread_join(t,NULL);dvm_gil_relock(v,depth);
   bool fine=true;concurrent=concurrent&&call_int(v,"java/net/Socket","isClosed","()Z",second,NULL,0,&fine)&&fine&&state(v,second)->fd<0;}
 }
 if(peer>=0)close(peer);close(listener);
 {union dvm_value arg[1]={{.l=dvm_new_string(v,"127.0.0.1")}};dvm_ref all=call_obj(v,"java/net/InetAddress","getAllByName","(Ljava/lang/String;)[Ljava/net/InetAddress;",0,arg,1);
  dvm_ref first=all&&dvm_array_length(v,all)==1?((dvm_ref *)dvm_array_data(v,all))[0]:0;bool fine=true;
  dvm_ref text=first?call_obj(v,"java/net/InetAddress","getHostAddress","()Ljava/lang/String;",first,NULL,0):0,shown=first?call_obj(v,"java/net/InetAddress","toString","()Ljava/lang/String;",first,NULL,0):0;
  int hash=first?call_int(v,"java/net/InetAddress","hashCode","()I",first,NULL,0,&fine):0;
  dvm_ref raw=dvm_new_array(v,'B',"B",4);if(raw)memcpy(dvm_array_data(v,raw),"\x7f\0\0\x01",4);union dvm_value b[1]={{.l=raw}};dvm_ref same=raw?call_obj(v,"java/net/InetAddress","getByAddress","([B)Ljava/net/InetAddress;",0,b,1):0;
  union dvm_value e[1]={{.l=same}};int equal=same&&first?call_int(v,"java/net/InetAddress","equals","(Ljava/lang/Object;)Z",first,e,1,&fine):0;
  union dvm_value six[1]={{.l=dvm_new_string(v,"::1")}};dvm_ref v6=call_obj(v,"java/net/InetAddress","getByName","(Ljava/lang/String;)Ljava/net/InetAddress;",0,six,1);dvm_ref v6text=v6?call_obj(v,"java/net/InetAddress","getHostAddress","()Ljava/lang/String;",v6,NULL,0):0;
  dns=fine&&text&&!strcmp(dvm_string_utf8(v,text),"127.0.0.1")&&shown&&!strcmp(dvm_string_utf8(v,shown),"/127.0.0.1")&&hash==0x7f000001&&equal&&v6text&&!strcmp(dvm_string_utf8(v,v6text),"::1")&&!strcmp(dvm_object_class(v,v6)->name,"java/net/Inet6Address");}
 {union dvm_value arg[2]={{.l=dvm_new_string(v,"unresolved.invalid")},{.i=443}};dvm_ref target=call_obj(v,"java/net/InetSocketAddress","createUnresolved","(Ljava/lang/String;I)Ljava/net/InetSocketAddress;",0,arg,2);bool fine=true;
  int flag=target?call_int(v,"java/net/InetSocketAddress","isUnresolved","()Z",target,NULL,0,&fine):0;dvm_ref s=dvm_new_object(v,dvm_find_class_or_external(v,"java/net/Socket"));union dvm_value o={0};
  if(fine&&flag&&s&&bind_socket(v,s)){union dvm_value c[1]={{.l=target}};socket_runtime_call(v,"java/net/Socket","connect","(Ljava/net/SocketAddress;)V",s,c,1,&o);unresolved=thrown(v,"java/net/UnknownHostException");}}
 if(!external_allowed){
  /* TEST-NET-1 literal: refused before any packet is sent. */
  union dvm_value arg[1]={{.l=dvm_new_string(v,"192.0.2.1")}};dvm_ref target=call_obj(v,"java/net/InetAddress","getByName","(Ljava/lang/String;)Ljava/net/InetAddress;",0,arg,1);
  dvm_ref s=dvm_new_object(v,dvm_find_class_or_external(v,"java/net/Socket"));
  if(target&&s&&bind_socket(v,s)){struct inet_state *t=inet(v,target);connect_to(v,s,t,NULL,443,100);policy=thrown(v,"java/net/ConnectException")&&state(v,s)->fd<0&&!state(v,s)->connected;}
 }else policy=true;
 bool idn_ascii=false,idn_case=false,idn_empty=false,idn_ascii_verbatim=false,idn_null=false;
 bool idn_empty_label=false,idn_long_label=false,idn_unicode=false,idn_unicode_native=false,idn_unicode_case=false,idn_signature=false;
 {union dvm_value arg[2]={{0}},o={0};
  arg[0].l=dvm_new_string(v,"game.bilibili.com");
  dvm_ref r=call_obj(v,"java/net/IDN","toASCII","(Ljava/lang/String;)Ljava/lang/String;",0,arg,1);
  idn_ascii=r&&dvm_string_utf8_length(v,r)==17&&!memcmp(dvm_string_utf8(v,r),"game.bilibili.com",17);
  arg[0].l=dvm_new_string(v,"Game.BiliBili.COM");
  r=call_obj(v,"java/net/IDN","toASCII","(Ljava/lang/String;)Ljava/lang/String;",0,arg,1);
  idn_case=r&&!strcmp(dvm_string_utf8(v,r),"Game.BiliBili.COM");
  arg[0].l=dvm_new_string(v,"");
  r=call_obj(v,"java/net/IDN","toASCII","(Ljava/lang/String;)Ljava/lang/String;",0,arg,1);
  idn_empty=r&&dvm_string_utf8_length(v,r)==0;
  /* Java's IDN.toASCII passes ASCII spaces/control bytes through unchanged;
   * OkHttp rejects them in Util.containsInvalidHostnameAsciiCodes(). */
  arg[0].l=dvm_new_string(v,"a\tb.com");
  r=call_obj(v,"java/net/IDN","toASCII","(Ljava/lang/String;)Ljava/lang/String;",0,arg,1);
  idn_ascii_verbatim=r&&!strcmp(dvm_string_utf8(v,r),"a\tb.com");
  arg[0].l=0;socket_runtime_call(v,"java/net/IDN","toASCII","(Ljava/lang/String;)Ljava/lang/String;",0,arg,1,&o);
  idn_null=thrown(v,"java/lang/NullPointerException");
  arg[0].l=dvm_new_string(v,"a..b");socket_runtime_call(v,"java/net/IDN","toASCII","(Ljava/lang/String;)Ljava/lang/String;",0,arg,1,&o);
  idn_empty_label=thrown(v,"java/lang/IllegalArgumentException");
  char long_label[80];memset(long_label,'a',64);memcpy(long_label+64,".com",5);
  arg[0].l=dvm_new_string(v,long_label);socket_runtime_call(v,"java/net/IDN","toASCII","(Ljava/lang/String;)Ljava/lang/String;",0,arg,1,&o);
  idn_long_label=thrown(v,"java/lang/IllegalArgumentException");
  arg[0].l=dvm_new_string(v,"例子.测试");
  r=call_obj(v,"java/net/IDN","toASCII","(Ljava/lang/String;)Ljava/lang/String;",0,arg,1);
  if(r){idn_unicode=!strcmp(dvm_string_utf8(v,r),"xn--fsqu00a.xn--0zwm56d");idn_unicode_native=idn_unicode;}
  else idn_unicode=thrown(v,"java/lang/UnsupportedOperationException");
  arg[0].l=dvm_new_string(v,"Game.例子");
  r=call_obj(v,"java/net/IDN","toASCII","(Ljava/lang/String;)Ljava/lang/String;",0,arg,1);
  if(r)idn_unicode_case=!strcmp(dvm_string_utf8(v,r),"Game.xn--fsqu00a");
  else idn_unicode_case=thrown(v,"java/lang/UnsupportedOperationException");
  arg[0].l=dvm_new_string(v,"game.bilibili.com");arg[1].i=0;
  bool handled=socket_runtime_call(v,"java/net/IDN","toASCII","(Ljava/lang/String;I)Ljava/lang/String;",0,arg,2,&o);
  idn_signature=!handled&&!dvm_exception(v);
 }
 bool url_space=false,url_safe=false,url_plus=false,url_utf8=false,url_reserved=false,url_alias=false;
 bool url_null_input=false,url_null_charset=false,url_empty_charset=false,url_bad_charset=false,url_signature=false;
 {union dvm_value arg[2]={{0}},o={0};
  arg[0].l=dvm_new_string(v,"a b");arg[1].l=dvm_new_string(v,"UTF-8");
  dvm_ref r=call_obj(v,"java/net/URLEncoder","encode","(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",0,arg,2);
  url_space=r&&!strcmp(dvm_string_utf8(v,r),"a+b");
  arg[0].l=dvm_new_string(v,"A-Za-z0-9._-*");
  r=call_obj(v,"java/net/URLEncoder","encode","(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",0,arg,2);
  url_safe=r&&!strcmp(dvm_string_utf8(v,r),"A-Za-z0-9._-*");
  arg[0].l=dvm_new_string(v,"+");
  r=call_obj(v,"java/net/URLEncoder","encode","(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",0,arg,2);
  url_plus=r&&!strcmp(dvm_string_utf8(v,r),"%2B");
  arg[0].l=dvm_new_string(v,"中文");
  r=call_obj(v,"java/net/URLEncoder","encode","(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",0,arg,2);
  url_utf8=r&&!strcmp(dvm_string_utf8(v,r),"%E4%B8%AD%E6%96%87");
  arg[0].l=dvm_new_string(v,"a/b?c=d&e");
  r=call_obj(v,"java/net/URLEncoder","encode","(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",0,arg,2);
  url_reserved=r&&!strcmp(dvm_string_utf8(v,r),"a%2Fb%3Fc%3Dd%26e");
  /* OpenJDK alias + case-insensitive lookup must reach the same encoder. */
  arg[0].l=dvm_new_string(v,"中文");arg[1].l=dvm_new_string(v,"uTf8");
  r=call_obj(v,"java/net/URLEncoder","encode","(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",0,arg,2);
  url_alias=r&&!strcmp(dvm_string_utf8(v,r),"%E4%B8%AD%E6%96%87");
  arg[0].l=dvm_new_string(v,"UTF-8");arg[1].l=0;
  socket_runtime_call(v,"java/net/URLEncoder","encode","(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",0,arg,2,&o);
  url_null_charset=thrown(v,"java/lang/NullPointerException");
  arg[0].l=0;arg[1].l=dvm_new_string(v,"UTF-8");
  socket_runtime_call(v,"java/net/URLEncoder","encode","(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",0,arg,2,&o);
  url_null_input=thrown(v,"java/lang/NullPointerException");
  arg[0].l=dvm_new_string(v,"a");arg[1].l=dvm_new_string(v,"");
  socket_runtime_call(v,"java/net/URLEncoder","encode","(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",0,arg,2,&o);
  url_empty_charset=thrown(v,"java/io/UnsupportedEncodingException");
  arg[1].l=dvm_new_string(v,"x-no-such-charset");
  socket_runtime_call(v,"java/net/URLEncoder","encode","(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",0,arg,2,&o);
  url_bad_charset=thrown(v,"java/io/UnsupportedEncodingException");
  /* A signature URLEncoder does not have must stay unclaimed (fail closed,
   * no exception).  The real one-arg encode(String) overload is not part of
   * this task and is deliberately left to the missing-API boundary. */
  arg[1].i=0;
  bool handled=socket_runtime_call(v,"java/net/URLEncoder","encode","(Ljava/lang/String;I)Ljava/lang/String;",0,arg,2,&o);
  url_signature=!handled&&!dvm_exception(v);
 }
 bool url_https=false,url_http=false,url_userinfo=false,url_ipv6=false,url_file=false,url_file_uri=false;
 bool url_errors=false,url_failclosed=false,url_absent=false;
 {bool fine=true;dvm_ref u=url_make(v,"https://game.bilibili.com/a/b?q=1#f");
  dvm_ref protocol=u?call_obj(v,"java/net/URL","getProtocol","()Ljava/lang/String;",u,NULL,0):0;
  dvm_ref host=u?call_obj(v,"java/net/URL","getHost","()Ljava/lang/String;",u,NULL,0):0;
  int port=u?call_int(v,"java/net/URL","getPort","()I",u,NULL,0,&fine):-2;
  int def=u?call_int(v,"java/net/URL","getDefaultPort","()I",u,NULL,0,&fine):-2;
  dvm_ref path=u?call_obj(v,"java/net/URL","getPath","()Ljava/lang/String;",u,NULL,0):0;
  dvm_ref query=u?call_obj(v,"java/net/URL","getQuery","()Ljava/lang/String;",u,NULL,0):0;
  dvm_ref ref=u?call_obj(v,"java/net/URL","getRef","()Ljava/lang/String;",u,NULL,0):0;
  dvm_ref authority=u?call_obj(v,"java/net/URL","getAuthority","()Ljava/lang/String;",u,NULL,0):0;
  dvm_ref userinfo=u?call_obj(v,"java/net/URL","getUserInfo","()Ljava/lang/String;",u,NULL,0):0;
  dvm_ref file=u?call_obj(v,"java/net/URL","getFile","()Ljava/lang/String;",u,NULL,0):0;
  dvm_ref shown=u?call_obj(v,"java/net/URL","toString","()Ljava/lang/String;",u,NULL,0):0;
  dvm_ref external=u?call_obj(v,"java/net/URL","toExternalForm","()Ljava/lang/String;",u,NULL,0):0;
  url_https=fine&&u&&url_text(v,protocol,"https")&&url_text(v,host,"game.bilibili.com")&&port==-1&&def==443&&url_text(v,path,"/a/b")&&url_text(v,query,"q=1")&&url_text(v,ref,"f")&&url_text(v,authority,"game.bilibili.com")&&!userinfo&&url_text(v,file,"/a/b?q=1")&&url_text(v,shown,"https://game.bilibili.com/a/b?q=1#f")&&url_text(v,external,"https://game.bilibili.com/a/b?q=1#f");
  union dvm_value o={0};
  url_absent=u&&!socket_runtime_call(v,"java/net/URL","getProtocol","(I)Ljava/lang/String;",u,NULL,0,&o)&&!dvm_exception(v);
 }
 {bool fine=true;dvm_ref u=url_make(v,"http://example.com");
  dvm_ref protocol=u?call_obj(v,"java/net/URL","getProtocol","()Ljava/lang/String;",u,NULL,0):0;
  dvm_ref host=u?call_obj(v,"java/net/URL","getHost","()Ljava/lang/String;",u,NULL,0):0;
  int port=u?call_int(v,"java/net/URL","getPort","()I",u,NULL,0,&fine):-2;
  int def=u?call_int(v,"java/net/URL","getDefaultPort","()I",u,NULL,0,&fine):-2;
  dvm_ref path=u?call_obj(v,"java/net/URL","getPath","()Ljava/lang/String;",u,NULL,0):0;
  dvm_ref query=u?call_obj(v,"java/net/URL","getQuery","()Ljava/lang/String;",u,NULL,0):0;
  dvm_ref ref=u?call_obj(v,"java/net/URL","getRef","()Ljava/lang/String;",u,NULL,0):0;
  dvm_ref authority=u?call_obj(v,"java/net/URL","getAuthority","()Ljava/lang/String;",u,NULL,0):0;
  dvm_ref file=u?call_obj(v,"java/net/URL","getFile","()Ljava/lang/String;",u,NULL,0):0;
  dvm_ref external=u?call_obj(v,"java/net/URL","toExternalForm","()Ljava/lang/String;",u,NULL,0):0;
  url_http=fine&&u&&url_text(v,protocol,"http")&&url_text(v,host,"example.com")&&port==-1&&def==80&&url_text(v,path,"")&&!query&&!ref&&url_text(v,authority,"example.com")&&url_text(v,file,"")&&url_text(v,external,"http://example.com");
 }
 {bool fine=true;dvm_ref u=url_make(v,"http://user:pass@example.com:8080/x");
  dvm_ref host=u?call_obj(v,"java/net/URL","getHost","()Ljava/lang/String;",u,NULL,0):0;
  int port=u?call_int(v,"java/net/URL","getPort","()I",u,NULL,0,&fine):-2;
  int def=u?call_int(v,"java/net/URL","getDefaultPort","()I",u,NULL,0,&fine):-2;
  dvm_ref userinfo=u?call_obj(v,"java/net/URL","getUserInfo","()Ljava/lang/String;",u,NULL,0):0;
  dvm_ref authority=u?call_obj(v,"java/net/URL","getAuthority","()Ljava/lang/String;",u,NULL,0):0;
  dvm_ref external=u?call_obj(v,"java/net/URL","toExternalForm","()Ljava/lang/String;",u,NULL,0):0;
  url_userinfo=fine&&u&&url_text(v,host,"example.com")&&port==8080&&def==80&&url_text(v,userinfo,"user:pass")&&url_text(v,authority,"user:pass@example.com:8080")&&url_text(v,external,"http://user:pass@example.com:8080/x");
 }
 {bool fine=true;dvm_ref u=url_make(v,"https://[::1]:8443/p");
  dvm_ref host=u?call_obj(v,"java/net/URL","getHost","()Ljava/lang/String;",u,NULL,0):0;
  int port=u?call_int(v,"java/net/URL","getPort","()I",u,NULL,0,&fine):-2;
  int def=u?call_int(v,"java/net/URL","getDefaultPort","()I",u,NULL,0,&fine):-2;
  dvm_ref external=u?call_obj(v,"java/net/URL","toExternalForm","()Ljava/lang/String;",u,NULL,0):0;
  dvm_ref z=url_make(v,"https://[fe80::1%25eth0]:9443/z");
  dvm_ref zhost=z?call_obj(v,"java/net/URL","getHost","()Ljava/lang/String;",z,NULL,0):0;
  int zport=z?call_int(v,"java/net/URL","getPort","()I",z,NULL,0,&fine):-2;
  url_ipv6=fine&&u&&url_text(v,host,"[::1]")&&port==8443&&def==443&&url_text(v,external,"https://[::1]:8443/p")&&z&&url_text(v,zhost,"[fe80::1%25eth0]")&&zport==9443;
 }
 {bool fine=true;dvm_ref u=url_make(v,"file:/path/to");
  dvm_ref protocol=u?call_obj(v,"java/net/URL","getProtocol","()Ljava/lang/String;",u,NULL,0):0;
  dvm_ref host=u?call_obj(v,"java/net/URL","getHost","()Ljava/lang/String;",u,NULL,0):0;
  int port=u?call_int(v,"java/net/URL","getPort","()I",u,NULL,0,&fine):-2;
  int def=u?call_int(v,"java/net/URL","getDefaultPort","()I",u,NULL,0,&fine):-2;
  dvm_ref path=u?call_obj(v,"java/net/URL","getPath","()Ljava/lang/String;",u,NULL,0):0;
  dvm_ref authority=u?call_obj(v,"java/net/URL","getAuthority","()Ljava/lang/String;",u,NULL,0):0;
  dvm_ref file=u?call_obj(v,"java/net/URL","getFile","()Ljava/lang/String;",u,NULL,0):0;
  dvm_ref external=u?call_obj(v,"java/net/URL","toExternalForm","()Ljava/lang/String;",u,NULL,0):0;
  url_file=fine&&u&&url_text(v,protocol,"file")&&url_text(v,host,"")&&port==-1&&def==-1&&url_text(v,path,"/path/to")&&!authority&&url_text(v,file,"/path/to")&&url_text(v,external,"file:/path/to");
 }
 {bool fine=true;dvm_ref u=url_make(v,"file:///path/to");
  dvm_ref host=u?call_obj(v,"java/net/URL","getHost","()Ljava/lang/String;",u,NULL,0):0;
  dvm_ref authority=u?call_obj(v,"java/net/URL","getAuthority","()Ljava/lang/String;",u,NULL,0):0;
  dvm_ref path=u?call_obj(v,"java/net/URL","getPath","()Ljava/lang/String;",u,NULL,0):0;
  dvm_ref external=u?call_obj(v,"java/net/URL","toExternalForm","()Ljava/lang/String;",u,NULL,0):0;
  bool empty_authority=authority&&dvm_string_utf8_length(v,authority)==0;
  url_file_uri=fine&&u&&url_text(v,host,"")&&empty_authority&&url_text(v,path,"/path/to")&&url_text(v,external,"file:/path/to");
 }
 {dvm_ref u=url_make(v,"");bool errors=!u&&thrown(v,"java/net/MalformedURLException");
  u=url_make(v,"not a url");errors=errors&&!u&&thrown(v,"java/net/MalformedURLException");
  u=url_make(v,"1http://h/x");errors=errors&&!u&&thrown(v,"java/net/MalformedURLException");
  u=url_make(v,"ftp://h/x");errors=errors&&!u&&thrown(v,"java/net/MalformedURLException");
  /* Deliberate fail-closed deviations from OpenJDK, which would accept these
   * with an empty host instead of inventing one. */
  u=url_make(v,"http:/foo");errors=errors&&!u&&thrown(v,"java/net/MalformedURLException");
  u=url_make(v,"http://");errors=errors&&!u&&thrown(v,"java/net/MalformedURLException");
  u=url_make(v,"http://h:abc/");errors=errors&&!u&&thrown(v,"java/net/MalformedURLException");
  u=url_make(v,"http://[::1/");errors=errors&&!u&&thrown(v,"java/net/MalformedURLException");
  u=url_make(v,"http://[abc]/");errors=errors&&!u&&thrown(v,"java/net/MalformedURLException");
  u=url_make(v,"http://[1:2:3:4:5:6:7:8:9]/");errors=errors&&!u&&thrown(v,"java/net/MalformedURLException");
  u=url_make(v,"http://[]/");errors=errors&&!u&&thrown(v,"java/net/MalformedURLException");
  u=url_make(v,"http://[::1%a@b]/");errors=errors&&!u&&thrown(v,"java/net/MalformedURLException");
  u=url_make(v,"http://h:2147483648/p");errors=errors&&!u&&thrown(v,"java/net/MalformedURLException");
  dvm_ref nullurl=dvm_new_object(v,dvm_find_class_or_external(v,"java/net/URL"));
  union dvm_value zero[1]={{0}},o={0};
  bool null_ok=nullurl&&socket_runtime_call(v,"java/net/URL","<init>","(Ljava/lang/String;)V",nullurl,zero,1,&o)&&thrown(v,"java/lang/NullPointerException");
  url_errors=errors&&null_ok;
 }
 {dvm_ref u=url_make(v,"https://example.com/path");union dvm_value o={0},proxy[1]={{0}};bool closed=u!=0;
  closed=closed&&socket_runtime_call(v,"java/net/URL","openConnection","()Ljava/net/URLConnection;",u,NULL,0,&o)&&thrown(v,"java/lang/UnsupportedOperationException");
  closed=closed&&socket_runtime_call(v,"java/net/URL","openConnection","(Ljava/net/Proxy;)Ljava/net/URLConnection;",u,proxy,1,&o)&&thrown(v,"java/lang/UnsupportedOperationException");
  closed=closed&&socket_runtime_call(v,"java/net/URL","openStream","()Ljava/io/InputStream;",u,NULL,0,&o)&&thrown(v,"java/lang/UnsupportedOperationException");
  closed=closed&&socket_runtime_call(v,"java/net/URL","toURI","()Ljava/net/URI;",u,NULL,0,&o)&&thrown(v,"java/lang/UnsupportedOperationException");
  url_failclosed=closed;
 }
 sdk_trace_fprintf(stderr,"SOCKET_DIAG streams=%d timeout=%d eof=%d concurrent=%d dns=%d policy=%d unresolved=%d idn_ascii=%d idn_case=%d idn_empty=%d idn_ascii_verbatim=%d idn_null=%d idn_empty_label=%d idn_long_label=%d idn_unicode=%d idn_unicode_native=%d idn_unicode_case=%d idn_signature=%d url_space=%d url_safe=%d url_plus=%d url_utf8=%d url_reserved=%d url_alias=%d url_null_input=%d url_null_charset=%d url_empty_charset=%d url_bad_charset=%d url_signature=%d url_https=%d url_http=%d url_userinfo=%d url_ipv6=%d url_file=%d url_file_uri=%d url_errors=%d url_failclosed=%d url_absent=%d\n",streams,timeout,eof,concurrent,dns,policy,unresolved,idn_ascii,idn_case,idn_empty,idn_ascii_verbatim,idn_null,idn_empty_label,idn_long_label,idn_unicode,idn_unicode_native,idn_unicode_case,idn_signature,url_space,url_safe,url_plus,url_utf8,url_reserved,url_alias,url_null_input,url_null_charset,url_empty_charset,url_bad_charset,url_signature,url_https,url_http,url_userinfo,url_ipv6,url_file,url_file_uri,url_errors,url_failclosed,url_absent);
 ok=ok&&streams&&timeout&&eof&&concurrent&&dns&&policy&&unresolved&&idn_ascii&&idn_case&&idn_empty&&idn_ascii_verbatim&&idn_null&&idn_empty_label&&idn_long_label&&idn_unicode&&idn_unicode_case&&idn_signature;
 ok=ok&&url_space&&url_safe&&url_plus&&url_utf8&&url_reserved&&url_alias&&url_null_input&&url_null_charset&&url_empty_charset&&url_bad_charset&&url_signature&&url_https&&url_http&&url_userinfo&&url_ipv6&&url_file&&url_file_uri&&url_errors&&url_failclosed&&url_absent&&!dvm_exception(v);
 sdk_trace_fprintf(stderr,"SOCKET_RUNTIME_SELFTEST %s loopback=1 streams=%d read_timeout=%d eof=%d concurrent_close=%d dns_literal=%d unresolved=%d external_policy=%s idn_ascii=%d idn_invalid=%d idn_unicode=%d idn_unicode_native=%d idn_signature=%d urlencode_form=%d urlencode_utf8=%d urlencode_errors=%d\n",ok?"PASS":"FAIL",streams,timeout,eof,concurrent,dns,unresolved,external_allowed?"enabled":"blocked_verified",idn_ascii&&idn_case&&idn_empty&&idn_ascii_verbatim,idn_null&&idn_empty_label&&idn_long_label,idn_unicode&&idn_unicode_case,idn_unicode_native,idn_signature,url_space&&url_safe&&url_plus&&url_reserved&&url_alias,url_utf8,url_null_input&&url_null_charset&&url_empty_charset&&url_bad_charset&&url_signature);
 return ok;
}

/* Explicit native payload ownership descriptor; unknown never implies atomic. */

int socket_gc_payload(struct dvm *v,struct dvm_object *o,dvm_gc_visit_fn visit,void *user){
 (void)v;if(o->data_destroy==inet_drop||o->data_destroy==url_drop)return DVM_GC_PAYLOAD_ATOMIC;
 if(o->data_destroy==address_drop){if(visit)visit(&((struct address_state*)o->data)->inet,user);return DVM_GC_PAYLOAD_REFS;}
 struct socket_state *p=NULL;if(o->data_destroy==socket_drop)p=o->data;
 else if(o->data_destroy==input_drop||o->data_destroy==output_drop)p=((struct stream_ref*)o->data)->s;
 else return DVM_GC_PAYLOAD_UNKNOWN;
 if(p&&visit){visit(&p->input,user);visit(&p->output,user);}return DVM_GC_PAYLOAD_REFS;
}
