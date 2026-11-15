#include <network/netmanager/net_connection.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <arpa/inet.h>
#include <time.h>
static long task6m_ms(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*1000L+t.tv_nsec/1000000;}
static void task6m_tcp(FILE *out,const struct sockaddr *a,socklen_t len){
 char ip[128];int family=a->sa_family;inet_ntop(family,family==AF_INET?(void*)&((struct sockaddr_in*)a)->sin_addr:(void*)&((struct sockaddr_in6*)a)->sin6_addr,ip,sizeof ip);
 int fd=socket(family,SOCK_STREAM,0);long t=task6m_ms();int e=errno,rc=-1;
 if(fd>=0){fcntl(fd,F_SETFL,O_NONBLOCK);rc=connect(fd,a,len);e=rc?errno:0;if(rc&&e==EINPROGRESS){struct pollfd p={fd,POLLOUT,0};int n=poll(&p,1,2500);socklen_t z=sizeof e;if(n>0){getsockopt(fd,SOL_SOCKET,SO_ERROR,&e,&z);rc=e?-1:0;}else{rc=-1;e=n==0?ETIMEDOUT:errno;}}close(fd);}
 fprintf(out,"TCP ip=%s rc=%d errno=%d elapsed_ms=%ld mono=%ld\n",ip,rc,e,task6m_ms()-t,task6m_ms());fflush(out);
}
static void *task6m_probe_thread(void *unused){
 (void)unused;char words[8][256];char *argv[8];int argc=1;argv[0]=words[0];FILE *input=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/task6m-dns-target.txt","r");if(!input)return NULL;while(argc<8&&fscanf(input,"%255s",words[argc])==1){argv[argc]=words[argc];argc++;}fclose(input);if(argc<2)return NULL;FILE *out=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/task6m-dns-probe.txt","a");if(!out)return NULL;fprintf(out,"PROBE pid=%d epoch=%ld mono=%ld\n",getpid(),(long)time(NULL),task6m_ms());
 NetConn_NetHandle h={0};NetConn_ConnectionProperties p={0};int rc=OH_NetConn_GetDefaultNet(&h);fprintf(out,"DEFAULT rc=%d netId=%d\n",rc,h.netId);
 rc=OH_NetConn_GetConnectionProperties(&h,&p);fprintf(out,"PROPERTIES rc=%d iface=%s dnsCount=%d routes=%d\n",rc,p.ifaceName,p.dnsListSize,p.routeListSize);
 if(!rc){for(int i=0;i<p.dnsListSize&&i<NETCONN_MAX_ADDR_SIZE;i++)fprintf(out,"DNS_SERVER %s\n",p.dnsList[i].address);for(int i=0;i<p.routeListSize&&i<NETCONN_MAX_ROUTE_SIZE;i++)fprintf(out,"ROUTE iface=%s gateway=%s default=%d\n",p.routeList[i].iface,p.routeList[i].gateway.address,p.routeList[i].isDefaultRoute);}
 /* Real sockets: closed bound port refuses, listening port accepts handshake.
  * Both nodes survive stable partition and caller errno survives the probes. */
 int ls=socket(AF_INET,SOCK_STREAM,0),cs=socket(AF_INET,SOCK_STREAM,0);
 struct sockaddr_in la={.sin_family=AF_INET,.sin_addr.s_addr=htonl(INADDR_LOOPBACK)},ca=la;socklen_t al=sizeof la;
 int setup=ls>=0&&cs>=0&&!bind(ls,(void*)&la,sizeof la)&&!listen(ls,8)&&!getsockname(ls,(void*)&la,&al)&&!bind(cs,(void*)&ca,sizeof ca)&&!getsockname(cs,(void*)&ca,&al);
 if(setup){struct android_addrinfo bad={.ai_family=AF_INET,.ai_socktype=SOCK_STREAM,.ai_addrlen=sizeof ca,.ai_addr=(void*)&ca},good=bad;bad.ai_canonname="canonical.test";good.ai_addr=(void*)&la;bad.ai_next=&good;struct android_addrinfo *head=&bad;errno=EDOM;android_rank_tcp_addresses(&head);fprintf(out,"RANK_SELFTEST pass=%d errno_preserved=%d all_nodes_preserved=%d canonical_first=%d\n",head==&good&&good.ai_next==&bad&&!bad.ai_next,errno==EDOM,head==&good&&head->ai_next==&bad,head->ai_canonname&&!strcmp(head->ai_canonname,"canonical.test")&&!bad.ai_canonname);close(ls);ls=-1;android_rank_tcp_addresses(&head);fprintf(out,"RANK_ALL_REFUSED_SELFTEST stable=%d all_nodes_preserved=%d\n",head==&good&&head->ai_next==&bad&&!bad.ai_next,head==&good&&head->ai_next==&bad);}
 else fprintf(out,"RANK_SELFTEST setup_failed errno=%d\n",errno);
 if(ls>=0)close(ls);if(cs>=0)close(cs);
 const char*host=argv[1];
 for(int mode=0;mode<2;mode++){struct addrinfo hint={.ai_family=AF_UNSPEC,.ai_socktype=SOCK_STREAM},*res=NULL;long t=task6m_ms();rc=mode?OH_NetConn_GetAddrInfo((char*)host,"443",&hint,&res,0):getaddrinfo(host,"443",&hint,&res);fprintf(out,"RESOLVE mode=%s host=%s rc=%d elapsed_ms=%ld\n",mode?"netconn":"libc",host,rc,task6m_ms()-t);fflush(out);for(struct addrinfo*a=res;a;a=a->ai_next)task6m_tcp(out,a->ai_addr,a->ai_addrlen);if(res){if(mode)OH_NetConn_FreeDnsResult(res);else freeaddrinfo(res);}}
 for(int i=2;i<argc;i++){struct sockaddr_in a={.sin_family=AF_INET,.sin_port=htons(443)};if(inet_pton(AF_INET,argv[i],&a.sin_addr)==1)task6m_tcp(out,(void*)&a,sizeof a);}
 fclose(out);return NULL;
}
