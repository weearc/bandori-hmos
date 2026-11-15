#include "thread_extras.h"
#include <stdio.h>
#include <string.h>
extern int sdk_trace_fprintf(FILE*,const char*,...);

/* The executor owns failure reporting. This side table only preserves the
 * handler reference a router ThreadFactory installs on its new Thread. */
#define MAX_THREAD_EXTRAS 64
typedef struct {dvm_ref thread,handler;} thread_extra;
static struct dvm *active;
static thread_extra extras[MAX_THREAD_EXTRAS];
static int extras_used;
/* Thread.setDefaultUncaughtExceptionHandler is process-wide in AOSP; this is
 * the VM-lifetime slot for it.  thread_extras_vm_start resets it per VM. */
static dvm_ref default_handler;
void thread_extras_vm_start(struct dvm *v){active=v;extras_used=0;default_handler=0;}
static int find_extra(dvm_ref thread){for(int i=0;i<extras_used;i++)if(extras[i].thread==thread)return i;return -1;}
static bool store_handler(struct dvm *v,dvm_ref thread,dvm_ref handler){
 int i=find_extra(thread);if(i>=0){extras[i].handler=handler;return true;}
 if(extras_used>=MAX_THREAD_EXTRAS){dvm__throw(v,"java/lang/UnsupportedOperationException","Thread handler table full");return false;}
 extras[extras_used].thread=thread;extras[extras_used].handler=handler;extras_used++;return true;
}
#define IS(a,b) (!strcmp(m,a)&&!strcmp(sig,b))
bool thread_extras_call(struct dvm *v,const char *c,const char *m,const char *sig,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 if(v!=active||strcmp(c,"java/lang/Thread"))return false;
 /* Static methods: no receiver.  Bugly asks for the default handler. */
 if(IS("getDefaultUncaughtExceptionHandler","()Ljava/lang/Thread$UncaughtExceptionHandler;")&&n==0){
  out->l=default_handler;return true;
 }
 if(IS("setDefaultUncaughtExceptionHandler","(Ljava/lang/Thread$UncaughtExceptionHandler;)V")&&n==1){
  default_handler=a[0].l;return true;
 }
 if(!self)return false;
 if(IS("setUncaughtExceptionHandler","(Ljava/lang/Thread$UncaughtExceptionHandler;)V")&&n==1){
  if(!store_handler(v,self,a[0].l))return true;return true;
 }
 if(IS("getUncaughtExceptionHandler","()Ljava/lang/Thread$UncaughtExceptionHandler;")){
  int i=find_extra(self);out->l=i>=0?extras[i].handler:0;return true;
 }
 return false;
}
bool thread_extras_selftest(struct dvm *v){
 dvm_ref thread=dvm_new_object(v,dvm_find_class_or_external(v,"java/lang/Thread"));
 dvm_ref handler=dvm_new_object(v,dvm_find_class_or_external(v,"java/lang/Thread$UncaughtExceptionHandler"));
 if(!thread||!handler)return false;
 union dvm_value a={.l=handler},out={0};
 if(!thread_extras_call(v,"java/lang/Thread","setUncaughtExceptionHandler","(Ljava/lang/Thread$UncaughtExceptionHandler;)V",thread,&a,1,&out))return false;
 if(!thread_extras_call(v,"java/lang/Thread","getUncaughtExceptionHandler","()Ljava/lang/Thread$UncaughtExceptionHandler;",thread,NULL,0,&out)||out.l!=handler)return false;
 a.l=0;
 if(!thread_extras_call(v,"java/lang/Thread","setUncaughtExceptionHandler","(Ljava/lang/Thread$UncaughtExceptionHandler;)V",thread,&a,1,&out))return false;
 if(!thread_extras_call(v,"java/lang/Thread","getUncaughtExceptionHandler","()Ljava/lang/Thread$UncaughtExceptionHandler;",thread,NULL,0,&out)||out.l)return false;
 sdk_trace_fprintf(stderr,"THREAD_EXTRAS_SELFTEST PASS uncaught_handler=1 clear=1\n");
 return true;
}
