#include "socket_runtime.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* Payloads are shared by the Socket and its streams. Closing any owner closes
 * the descriptor; remaining owners retain only the closed state. */
struct socket_state { int fd; bool connected; bool closed; unsigned owners; };
struct address_state { char *host; int port; };
static struct dvm *active;
static void socket_drop(void *p){struct socket_state *s=p;if(s){if(s->fd>=0)close(s->fd);s->fd=-1;s->closed=true;if(!--s->owners)free(s);}}
static void address_drop(void *p){struct address_state *a=p;if(a){free(a->host);free(a);}}
static bool fail(struct dvm *v,const char *type,const char *message){dvm__throw(v,type,"%s",message);return true;}
static struct socket_state *state(struct dvm *v,dvm_ref r){struct dvm_object *o=dvm__obj(v,r);return o&&o->data_destroy==socket_drop?o->data:NULL;}
static struct address_state *address(struct dvm *v,dvm_ref r){struct dvm_object *o=dvm__obj(v,r);return o&&o->data_destroy==address_drop?o->data:NULL;}
static bool bind_socket(struct dvm *v,dvm_ref r){struct dvm_object *o=dvm__obj(v,r);if(!o)return fail(v,"java/lang/NullPointerException","socket is null");if(o->data)return fail(v,"java/lang/IllegalStateException","socket already initialized");struct socket_state *s=calloc(1,sizeof *s);if(!s)return fail(v,"java/lang/OutOfMemoryError","socket allocation");s->fd=-1;s->owners=1;o->data=s;o->data_destroy=socket_drop;return true;}
static int connect_host(struct socket_state *s,const char *host,int port,int timeout){
 char service[8];snprintf(service,sizeof service,"%d",port);struct addrinfo hints={0},*all=NULL;hints.ai_family=AF_UNSPEC;hints.ai_socktype=SOCK_STREAM;hints.ai_protocol=IPPROTO_TCP;
 if(getaddrinfo(host,service,&hints,&all))return EHOSTUNREACH;
 int error=ECONNREFUSED;
 for(struct addrinfo *it=all;it;it=it->ai_next){int fd=socket(it->ai_family,it->ai_socktype,it->ai_protocol);if(fd<0){error=errno;continue;}int flags=fcntl(fd,F_GETFL,0);if(flags<0||fcntl(fd,F_SETFL,flags|O_NONBLOCK)<0){error=errno;close(fd);continue;}
  int rc=connect(fd,it->ai_addr,it->ai_addrlen);if(rc<0&&errno==EINPROGRESS){struct pollfd p={.fd=fd,.events=POLLOUT};do rc=poll(&p,1,timeout>0?timeout:-1);while(rc<0&&errno==EINTR);if(rc==0)error=ETIMEDOUT;else if(rc<0)error=errno;else{socklen_t len=sizeof error;if(getsockopt(fd,SOL_SOCKET,SO_ERROR,&error,&len)<0)error=errno;}}else error=rc<0?errno:0;
  if(!error){if(fcntl(fd,F_SETFL,flags)<0){error=errno;close(fd);continue;}s->fd=fd;s->connected=true;break;}close(fd);
 }
 freeaddrinfo(all);return error;
}
static bool connect_ref(struct dvm *v,dvm_ref self,const char *host,int port,int timeout){struct socket_state *s=state(v,self);if(!s)return fail(v,"java/lang/IllegalStateException","socket not initialized");if(s->closed||s->fd>=0||s->connected)return fail(v,"java/net/SocketException","socket closed or already connected");if(!host)return fail(v,"java/lang/NullPointerException","host is null");if(port<0||port>65535||timeout<0)return fail(v,"java/lang/IllegalArgumentException","invalid port or timeout");char *copy=strdup(host);if(!copy)return fail(v,"java/lang/OutOfMemoryError","host allocation");struct socket_state result={.fd=-1};unsigned depth=dvm_gil_unlock_all(v);int e=connect_host(&result,copy,port,timeout);dvm_gil_relock(v,depth);free(copy);if(s->closed){if(result.fd>=0)close(result.fd);return fail(v,"java/net/SocketException","socket closed during connect");}if(e){dvm__throw(v,e==ETIMEDOUT?"java/net/SocketTimeoutException":"java/net/ConnectException","connect failed: %s",strerror(e));}else{s->fd=result.fd;s->connected=true;}return true;}
void socket_runtime_vm_start(struct dvm *v){active=v;}
void socket_runtime_cleanup(void){active=NULL;}
#define IS(x,y) (!strcmp(m,x)&&!strcmp(sig,y))
bool socket_runtime_call(struct dvm *v,const char *c,const char *m,const char *sig,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 if(v!=active)return false;
 if(!strcmp(c,"javax/net/SocketFactory")){
  if(IS("getDefault","()Ljavax/net/SocketFactory;")&&n==0){out->l=dvm_new_object(v,dvm_find_class_or_external(v,c));return out->l?true:fail(v,"java/lang/OutOfMemoryError","factory allocation");}
  if(IS("createSocket","()Ljava/net/Socket;")&&n==0){out->l=dvm_new_object(v,dvm_find_class_or_external(v,"java/net/Socket"));if(!out->l)return fail(v,"java/lang/OutOfMemoryError","socket allocation");return bind_socket(v,out->l);}
  if(IS("createSocket","(Ljava/lang/String;I)Ljava/net/Socket;")&&n==2){out->l=dvm_new_object(v,dvm_find_class_or_external(v,"java/net/Socket"));if(!out->l)return fail(v,"java/lang/OutOfMemoryError","socket allocation");if(!bind_socket(v,out->l)||dvm_exception(v))return true;const char *host=dvm_string_utf8(v,a[0].l);if(host&&strlen(host)!=dvm_string_utf8_length(v,a[0].l))return fail(v,"java/lang/IllegalArgumentException","NUL in host");return connect_ref(v,out->l,host,a[1].i,0);}
  return false;
 }
 if(!strcmp(c,"java/net/InetSocketAddress")){
  if(IS("<init>","(Ljava/lang/String;I)V")&&n==2){struct dvm_object *o=dvm__obj(v,self);const char *host=dvm_string_utf8(v,a[0].l);if(!o||!host)return fail(v,"java/lang/NullPointerException","address host");if(strlen(host)!=dvm_string_utf8_length(v,a[0].l)||o->data||a[1].i<0||a[1].i>65535)return fail(v,"java/lang/IllegalArgumentException","invalid address");struct address_state *ad=calloc(1,sizeof *ad);if(!ad)return fail(v,"java/lang/OutOfMemoryError","address allocation");ad->host=strdup(host);ad->port=a[1].i;if(!ad->host){free(ad);return fail(v,"java/lang/OutOfMemoryError","address host allocation");}o->data=ad;o->data_destroy=address_drop;return true;}
  struct address_state *ad=address(v,self);if(!ad)return false;if(IS("getHostString","()Ljava/lang/String;")){out->l=dvm_new_string(v,ad->host);return true;}if(IS("getPort","()I")){out->i=ad->port;return true;}return false;
 }
 if(!strcmp(c,"java/net/Socket")){
  if(IS("<init>","()V")&&n==0)return bind_socket(v,self);
  if(IS("<init>","(Ljava/lang/String;I)V")&&n==2){if(!bind_socket(v,self)||dvm_exception(v))return true;const char *host=dvm_string_utf8(v,a[0].l);if(host&&strlen(host)!=dvm_string_utf8_length(v,a[0].l))return fail(v,"java/lang/IllegalArgumentException","NUL in host");return connect_ref(v,self,host,a[1].i,0);}
  struct socket_state *s=state(v,self);if(!s)return false;
  if(IS("connect","(Ljava/net/SocketAddress;)V")&&n==1){struct address_state *ad=address(v,a[0].l);if(!ad)return fail(v,"java/lang/UnsupportedOperationException","unsupported socket address");return connect_ref(v,self,ad->host,ad->port,0);}
  if(IS("connect","(Ljava/net/SocketAddress;I)V")&&n==2){struct address_state *ad=address(v,a[0].l);if(!ad)return fail(v,"java/lang/UnsupportedOperationException","unsupported socket address");return connect_ref(v,self,ad->host,ad->port,a[1].i);}
  if(IS("close","()V")&&n==0){if(s->fd>=0){shutdown(s->fd,SHUT_RDWR);close(s->fd);}s->fd=-1;s->closed=true;return true;}
  if(IS("isConnected","()Z")&&n==0){out->i=s->connected;return true;}
  if(IS("isClosed","()Z")&&n==0){out->i=s->closed;return true;}
  if((IS("getInputStream","()Ljava/io/InputStream;")||IS("getOutputStream","()Ljava/io/OutputStream;"))&&n==0)return fail(v,"java/lang/UnsupportedOperationException","socket streams unavailable");
  return false;
 }
 return false;
}
bool socket_runtime_selftest(struct dvm *v){
 if(v!=active)return false;int listener=socket(AF_INET,SOCK_STREAM,0);if(listener<0)return false;struct sockaddr_in addr={.sin_family=AF_INET,.sin_addr={.s_addr=htonl(INADDR_LOOPBACK)},.sin_port=0};socklen_t len=sizeof addr;bool ok=bind(listener,(struct sockaddr *)&addr,len)==0&&listen(listener,1)==0&&getsockname(listener,(struct sockaddr *)&addr,&len)==0;
 union dvm_value out={0},arg[2]={{0},{0}};dvm_ref factory=0,client=0;if(ok){ok=socket_runtime_call(v,"javax/net/SocketFactory","getDefault","()Ljavax/net/SocketFactory;",0,NULL,0,&out)&&out.l;factory=out.l;}
 if(ok){ok=socket_runtime_call(v,"javax/net/SocketFactory","createSocket","()Ljava/net/Socket;",factory,NULL,0,&out)&&out.l;client=out.l;}
 if(ok){dvm_ref address_ref=dvm_new_object(v,dvm_find_class_or_external(v,"java/net/InetSocketAddress"));arg[0].l=dvm_new_string(v,"127.0.0.1");arg[1].i=ntohs(addr.sin_port);ok=address_ref&&socket_runtime_call(v,"java/net/InetSocketAddress","<init>","(Ljava/lang/String;I)V",address_ref,arg,2,&out);arg[0].l=address_ref;arg[1].i=1000;ok=ok&&socket_runtime_call(v,"java/net/Socket","connect","(Ljava/net/SocketAddress;I)V",client,arg,2,&out)&&!dvm_exception(v);}
 struct pollfd p={.fd=listener,.events=POLLIN};ok=ok&&poll(&p,1,1000)==1;int peer=ok?accept(listener,NULL,NULL):-1;ok=ok&&peer>=0;if(ok){ok=socket_runtime_call(v,"java/net/Socket","isConnected","()Z",client,NULL,0,&out)&&out.i;unsigned char byte=0x5a;ok=ok&&send(state(v,client)->fd,&byte,1,MSG_NOSIGNAL)==1;p.fd=peer;p.events=POLLIN;ok=ok&&poll(&p,1,1000)==1;ok=ok&&recv(peer,&byte,1,0)==1&&byte==0x5a;byte=0x73;ok=ok&&send(peer,&byte,1,MSG_NOSIGNAL)==1;p.fd=state(v,client)->fd;ok=ok&&poll(&p,1,1000)==1;ok=ok&&recv(p.fd,&byte,1,0)==1&&byte==0x73;ok=ok&&socket_runtime_call(v,"java/net/Socket","close","()V",client,NULL,0,&out);ok=ok&&socket_runtime_call(v,"java/net/Socket","isConnected","()Z",client,NULL,0,&out)&&out.i;ok=ok&&socket_runtime_call(v,"java/net/Socket","isClosed","()Z",client,NULL,0,&out)&&out.i;ok=ok&&state(v,client)->fd<0;}
 if(peer>=0)close(peer);close(listener);if(ok&&!dvm_exception(v)){extern int sdk_trace_fprintf(FILE *,const char *,...);sdk_trace_fprintf(stderr,"SOCKET_RUNTIME_SELFTEST PASS loopback=1 duplex=1 close=1\n");return true;}return false;
}
