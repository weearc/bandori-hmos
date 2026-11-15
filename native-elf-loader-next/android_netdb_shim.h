#ifndef ANDROID_NETDB_SHIM_H
#define ANDROID_NETDB_SHIM_H
#include <netdb.h>
#include <netinet/in.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <time.h>
/* AOSP bionic LP64 layout, unlike musl: canonname precedes sockaddr. */
struct android_addrinfo {
 int ai_flags,ai_family,ai_socktype,ai_protocol;
 socklen_t ai_addrlen;
 char *ai_canonname;
 struct sockaddr *ai_addr;
 struct android_addrinfo *ai_next;
};
_Static_assert(offsetof(struct android_addrinfo,ai_canonname)==24,"bionic canonname offset");
_Static_assert(offsetof(struct android_addrinfo,ai_addr)==32,"bionic sockaddr offset");
static int android_gai_error(int e){
 switch(e){case 0:return 0;case EAI_AGAIN:return 2;case EAI_BADFLAGS:return 3;
 case EAI_FAIL:return 4;case EAI_FAMILY:return 5;case EAI_MEMORY:return 6;
 case EAI_NONAME:return 8;case EAI_SERVICE:return 9;case EAI_SOCKTYPE:return 10;
 case EAI_SYSTEM:return 11;case EAI_OVERFLOW:return 14;default:return 4;}
}
static void android_freeaddrinfo(struct android_addrinfo *p){
 while(p){struct android_addrinfo *next=p->ai_next;free(p->ai_addr);free(p->ai_canonname);free(p);p=next;}
}
/* Keep every resolver result, but prefer addresses whose TCP handshake succeeds.
 * Probe all candidates concurrently, never on the guest socket; preserve its
 * flags, ownership and connect semantics. No hostname/address policy table.
 * Stop at the first successful candidate; a 2500ms overall deadline allows a
 * normal TCP SYN retry without inheriting the guest's ~15s silent-drop wait.
 */
static long android_gai_ms(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*1000L+t.tv_nsec/1000000;}
static void android_rank_tcp_addresses(struct android_addrinfo **head){
 enum {MAX_PROBES=64,DEADLINE_MS=2500};
 struct android_addrinfo *nodes[MAX_PROBES];struct pollfd pf[MAX_PROBES];
 unsigned char good[MAX_PROBES]={0};int errors[MAX_PROBES]={0};int n=0,pending=0,successes=0,saved=errno;
 for(struct android_addrinfo*p=*head;p;p=p->ai_next){
  if(n==MAX_PROBES||p->ai_socktype!=SOCK_STREAM||!p->ai_addr||
     (p->ai_family!=AF_INET&&p->ai_family!=AF_INET6))return;
  if((p->ai_family==AF_INET&&!((struct sockaddr_in*)p->ai_addr)->sin_port)||
     (p->ai_family==AF_INET6&&!((struct sockaddr_in6*)p->ai_addr)->sin6_port))return;
  nodes[n++]=p;
 }
 if(n<2)return;
 long deadline=android_gai_ms()+DEADLINE_MS;
 for(int i=0;i<n;i++){
  pf[i]=(struct pollfd){.fd=-1,.events=POLLOUT};
  int fd=socket(nodes[i]->ai_family,SOCK_STREAM,IPPROTO_TCP);if(fd<0){errors[i]=errno;continue;}
  if(fcntl(fd,F_SETFL,O_NONBLOCK)<0){errors[i]=errno;close(fd);continue;}
  int rc=connect(fd,nodes[i]->ai_addr,nodes[i]->ai_addrlen);
  if(!rc){good[i]=1;successes++;close(fd);}else if(errno==EINPROGRESS){errors[i]=EINPROGRESS;pf[i].fd=fd;pending++;}else{errors[i]=errno;close(fd);}
 }
 while(pending&&!successes){
  long left=deadline-android_gai_ms();if(left<=0)break;
  int rc=poll(pf,n,(int)left);if(rc<0&&errno==EINTR)continue;if(rc<=0)break;
  for(int i=0;i<n;i++)if(pf[i].fd>=0&&pf[i].revents){
   int e=0;socklen_t z=sizeof e;
   good[i]=!getsockopt(pf[i].fd,SOL_SOCKET,SO_ERROR,&e,&z)&&!e;
   errors[i]=good[i]?0:(e?e:errno);successes+=good[i];
   close(pf[i].fd);pf[i].fd=-1;pending--;
  }
 }
#ifdef LUNARIA_TASK6K_OBSERVER
 FILE *ranklog=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/task6m-rank.txt","a");
 if(ranklog){long now=android_gai_ms();for(int i=0;i<n;i++){char ip[INET6_ADDRSTRLEN]="?";inet_ntop(nodes[i]->ai_family,nodes[i]->ai_family==AF_INET?(void*)&((struct sockaddr_in*)nodes[i]->ai_addr)->sin_addr:(void*)&((struct sockaddr_in6*)nodes[i]->ai_addr)->sin6_addr,ip,sizeof ip);fprintf(ranklog,"RANK pid=%d mono=%ld ip=%s ready=%u error=%d elapsed_ms=%ld successes=%d\n",getpid(),now,ip,good[i],errors[i],now-(deadline-DEADLINE_MS),successes);}fclose(ranklog);}
#endif
 struct android_addrinfo *original_head=*head,**tail=head;
 for(int pass=1;pass>=0;pass--)for(int i=0;i<n;i++)if(good[i]==pass){*tail=nodes[i];tail=&nodes[i]->ai_next;}
 *tail=NULL;
 /* AI_CANONNAME belongs on the first result, even after stable reordering. */
 if(*head!=original_head&&original_head->ai_canonname){
  char *name=(*head)->ai_canonname;(*head)->ai_canonname=original_head->ai_canonname;original_head->ai_canonname=name;
 }
 for(int i=0;i<n;i++)if(pf[i].fd>=0)close(pf[i].fd);
 errno=saved;
}
static int android_getaddrinfo(const char *node,const char *service,const struct android_addrinfo *hints,struct android_addrinfo **out){
 if(!out)return 4;*out=NULL;struct addrinfo host_hints={0},*host=NULL;
 if(hints){
  unsigned f=(unsigned)hints->ai_flags;
  if(f&~0xf0fu)return 3;
  host_hints.ai_flags=(f&7)|((f&8)?AI_NUMERICSERV:0)|((f&0x100)?AI_ALL:0)|((f&0x200)?AI_V4MAPPED:0)|((f&0x400)?AI_ADDRCONFIG:0)|((f&0x800)?AI_V4MAPPED:0);
  host_hints.ai_family=hints->ai_family;host_hints.ai_socktype=hints->ai_socktype;host_hints.ai_protocol=hints->ai_protocol;
 }
 long dns_started=android_gai_ms();int rc=getaddrinfo(node,service,hints?&host_hints:NULL,&host);int dns_errno=errno;
#ifdef LUNARIA_TASK6K_OBSERVER
 FILE *dnslog=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/config-dns.txt","a");
 if(dnslog){dprintf(fileno(dnslog),"DNS pid=%d mono_ms=%ld host=%s service=%s native_gai=%d guest_gai=%d errno=%d resolver_ms=%ld\n",getpid(),android_gai_ms(),node?node:"<null>",service?service:"<null>",rc,android_gai_error(rc),rc==EAI_SYSTEM?dns_errno:0,android_gai_ms()-dns_started);fclose(dnslog);}errno=dns_errno;
#endif
 if(rc)return android_gai_error(rc);
 struct android_addrinfo **tail=out;
 for(struct addrinfo *p=host;p;p=p->ai_next){
  struct android_addrinfo *g=calloc(1,sizeof *g);if(!g)goto oom;
  *tail=g;tail=&g->ai_next;
  g->ai_flags=hints?hints->ai_flags:0;g->ai_family=p->ai_family;g->ai_socktype=p->ai_socktype;g->ai_protocol=p->ai_protocol;g->ai_addrlen=p->ai_addrlen;
  if(p->ai_addrlen){g->ai_addr=malloc(p->ai_addrlen);if(!g->ai_addr)goto oom;memcpy(g->ai_addr,p->ai_addr,p->ai_addrlen);}
  if(p->ai_canonname){g->ai_canonname=strdup(p->ai_canonname);if(!g->ai_canonname)goto oom;}
 }
 freeaddrinfo(host);
 if(node&&hints&&!(host_hints.ai_flags&(AI_PASSIVE|AI_NUMERICHOST)))android_rank_tcp_addresses(out);
 return 0;
oom:
 freeaddrinfo(host);android_freeaddrinfo(*out);*out=NULL;return 6;
}
static const char *android_gai_strerror(int e){
 switch(e){case 0:return "Success";case 1:return "Address family for hostname not supported";
 case 2:return gai_strerror(EAI_AGAIN);case 3:return gai_strerror(EAI_BADFLAGS);
 case 4:return gai_strerror(EAI_FAIL);case 5:return gai_strerror(EAI_FAMILY);
 case 6:return gai_strerror(EAI_MEMORY);case 7:return "No address associated with hostname";
 case 8:return gai_strerror(EAI_NONAME);case 9:return gai_strerror(EAI_SERVICE);
 case 10:return gai_strerror(EAI_SOCKTYPE);case 11:return gai_strerror(EAI_SYSTEM);
 case 12:return "Invalid hints";case 13:return "Unknown resolved protocol";
 case 14:return gai_strerror(EAI_OVERFLOW);default:return "Unknown address resolution error";}
}
#endif
