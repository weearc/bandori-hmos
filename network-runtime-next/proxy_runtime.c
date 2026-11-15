#include "proxy_runtime.h"
#include <network/netmanager/net_connection.h>
#include <string.h>
#include <stdio.h>
extern int sdk_trace_fprintf(FILE*,const char*,...);
bool proxy_runtime_call(struct dvm *v,const char *c,const char *m,const char *s,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 (void)self;(void)a;
 if(!strcmp(c,"java/net/ProxySelector")&&!strcmp(m,"<init>")&&!strcmp(s,"()V")&&!n){
  struct dvm_object *o=dvm__obj(v,self);for(struct dvm_class *k=o?o->cls:NULL;k;k=k->super)if(!strcmp(k->name,"java/net/ProxySelector"))return true;
  dvm__throw(v,"java/lang/IllegalArgumentException","ProxySelector constructor receiver");return true;
 }
 if(strcmp(c,"java/net/ProxySelector")||strcmp(m,"getDefault")||strcmp(s,"()Ljava/net/ProxySelector;")||n)return false;
 NetConn_HttpProxy proxy={0};int32_t result=OH_NetConn_GetDefaultHttpProxy(&proxy);
 sdk_trace_fprintf(stderr,"HOST_PROXY_QUERY result=%d configured=%d\n",result,proxy.host[0]!=0||proxy.port!=0);
 if(result){dvm__throw(v,"java/lang/UnsupportedOperationException","host proxy query failed: %d",result);return true;}
 if(proxy.host[0]||proxy.port){dvm__throw(v,"java/lang/UnsupportedOperationException","configured host proxy bridge is unavailable");return true;}
 /* Java permits no installed selector. OkHttp then uses its original DEX
  * NullProxySelector. Never substitute direct routing if a proxy was returned. */
 out->l=0;return true;
}
