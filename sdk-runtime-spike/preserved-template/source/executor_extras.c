#include "executor_extras.h"
#include "executor_runtime.h"
#include "java_basics.h"
#include <stdio.h>
#include <string.h>
extern int sdk_trace_fprintf(FILE*,const char*,...);

#define IS(a,b) (!strcmp(m,a)&&!strcmp(sig,b))
bool executor_extras_call(struct dvm *v,const char *c,const char *m,const char *sig,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 (void)self;(void)n;
 if(strcmp(c,"java/util/concurrent/Executors")||strcmp(m,"newSingleThreadExecutor"))return false;
 bool factory=IS("newSingleThreadExecutor","(Ljava/util/concurrent/ThreadFactory;)Ljava/util/concurrent/ExecutorService;");
 if(!factory&&!IS("newSingleThreadExecutor","()Ljava/util/concurrent/ExecutorService;"))return false;
 if(factory&&n!=1)return false;
 dvm_ref pool=dvm_new_object(v,dvm_find_class_or_external(v,"java/util/concurrent/ThreadPoolExecutor"));
 dvm_ref queue=dvm_new_object(v,dvm_find_class_or_external(v,"java/util/concurrent/LinkedBlockingQueue"));
 if(!pool||!queue){dvm__throw(v,"java/lang/OutOfMemoryError","single-thread executor");return true;}
 union dvm_value qout={0};
 if(!executor_runtime_call(v,"java/util/concurrent/LinkedBlockingQueue","<init>","()V",queue,NULL,0,&qout)){dvm__throw(v,"java/lang/UnsupportedOperationException","single-thread work queue");return true;}
 union dvm_value unit={0};
 if(!java_basics_static(v,"java/util/concurrent/TimeUnit","MILLISECONDS","Ljava/util/concurrent/TimeUnit;",&unit)||!unit.l){dvm__throw(v,"java/lang/IllegalStateException","single-thread TimeUnit");return true;}
 union dvm_value args[6]={{.i=1},{.i=1},{.j=0},{.l=unit.l},{.l=queue},{.l=factory?a[0].l:0}};
 const char *ctor=factory?"(IIJLjava/util/concurrent/TimeUnit;Ljava/util/concurrent/BlockingQueue;Ljava/util/concurrent/ThreadFactory;)V":"(IIJLjava/util/concurrent/TimeUnit;Ljava/util/concurrent/BlockingQueue;)V";
 if(!executor_runtime_call(v,"java/util/concurrent/ThreadPoolExecutor","<init>",ctor,pool,args,factory?6:5,&qout)){dvm__throw(v,"java/lang/IllegalStateException","single-thread executor constructor");return true;}
 out->l=pool;return true;
}
bool executor_extras_selftest(struct dvm *v){
 union dvm_value out={0};
 if(!executor_extras_call(v,"java/util/concurrent/Executors","newSingleThreadExecutor","()Ljava/util/concurrent/ExecutorService;",0,NULL,0,&out)||!out.l)return false;
 dvm_ref pool=out.l;
 if(!executor_runtime_call(v,"java/util/concurrent/ExecutorService","isShutdown","()Z",pool,NULL,0,&out)||out.i)return false;
 if(!executor_runtime_call(v,"java/util/concurrent/ExecutorService","shutdown","()V",pool,NULL,0,&out))return false;
 if(!executor_runtime_call(v,"java/util/concurrent/ExecutorService","isShutdown","()Z",pool,NULL,0,&out)||!out.i)return false;
 if(dvm_exception(v))return false;
 sdk_trace_fprintf(stderr,"EXECUTOR_EXTRAS_SELFTEST PASS newSingleThreadExecutor=1 shutdown=1\n");
 return true;
}
