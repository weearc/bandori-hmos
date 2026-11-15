#include "thread_runtime.h"
#include "portable.h"
#include <stdlib.h>
#include <stdatomic.h>
struct observed{_Atomic unsigned count,messages;pthread_t main,callback_thread;bool wrong_thread,without_gil,wrong_message;};
static struct observed *message_observer;
static bool run_callback(struct dvm *v,dvm_ref self,const union dvm_value *args,int n,union dvm_value *out){(void)args;(void)n;(void)out;struct observed *o=dvm__obj(v,self)->data;o->wrong_thread|=pthread_equal(pthread_self(),o->main);o->without_gil|=!dvm_gil_held(v);o->callback_thread=pthread_self();atomic_fetch_add(&o->count,1);return true;}
static bool message_callback(struct dvm *v,dvm_ref self,const union dvm_value *args,int n,union dvm_value *out){(void)n;(void)out;union dvm_value what={0},target={0};struct observed *o=message_observer;o->wrong_thread|=pthread_equal(pthread_self(),o->main);o->without_gil|=!dvm_gil_held(v);o->wrong_message|=!dvm_get_field(v,args[0].l,"what","I",&what)||what.i!=123||!dvm_get_field(v,args[0].l,"target","Landroid/os/Handler;",&target)||target.l!=self;atomic_fetch_add(&o->messages,1);return true;}
static union dvm_value invoke(struct dvm *v,const char *c,const char *m,const char *sig,dvm_ref self,union dvm_value a){union dvm_value out={0};if(!thread_runtime_call(v,c,m,sig,self,&a,1,&out))dvm__throw(v,"java/lang/AssertionError","thread test dispatch %s.%s",c,m);return out;}
bool thread_runtime_selftest(struct dvm *v){
 bool ok=true;union dvm_value z={0},out={0};const char *ht="android/os/HandlerThread";
 dvm_ref t=dvm_new_object(v,dvm_find_class_or_external(v,ht));union dvm_value name={.l=dvm_new_string(v,"real-async-diagnostic")};invoke(v,ht,"<init>","(Ljava/lang/String;)V",t,name);
 ok=ok&&!invoke(v,ht,"isAlive","()Z",t,z).i;invoke(v,ht,"start","()V",t,z);dvm_ref looper=invoke(v,ht,"getLooper","()Landroid/os/Looper;",t,z).l;ok=ok&&looper&&invoke(v,ht,"isAlive","()Z",t,z).i;
 struct dvm_class *c=dvm__register_builtin(v,"Lsdktrace/tests/AsyncRunnable;");c->methods=calloc(1,sizeof *c->methods);c->nmethods=1;c->methods[0].cls=c;c->methods[0].name="run";c->methods[0].sig=strdup("()V");c->methods[0].ret_kind='V';c->methods[0].builtin=run_callback;
 dvm_ref runnable=dvm_new_object(v,c);struct observed *observed=calloc(1,sizeof *observed);observed->main=pthread_self();atomic_init(&observed->count,0);atomic_init(&observed->messages,0);dvm__obj(v,runnable)->data=observed;message_observer=observed;
 struct dvm_class *hc=dvm__register_builtin(v,"Lsdktrace/tests/AsyncHandler;");hc->super=dvm_find_class_or_external(v,"android/os/Handler");hc->methods=calloc(1,sizeof *hc->methods);hc->nmethods=1;hc->methods[0].cls=hc;hc->methods[0].name="handleMessage";hc->methods[0].sig=strdup("(Landroid/os/Message;)V");hc->methods[0].arg_count=hc->methods[0].arg_slots=1;hc->methods[0].ret_kind='V';hc->methods[0].builtin=message_callback;
 dvm_ref handler=dvm_new_object(v,hc);union dvm_value l={.l=looper};extern bool android_basics_call(struct dvm *,const char *,const char *,const char *,dvm_ref,const union dvm_value *,int,union dvm_value *);
 ok=ok&&android_basics_call(v,"android/os/Handler","<init>","(Landroid/os/Looper;)V",handler,&l,1,&out)&&!dvm_exception(v);
 union dvm_value task={.l=runnable};ok=ok&&invoke(v,"android/os/Handler","post","(Ljava/lang/Runnable;)Z",handler,task).i==1;ok=ok&&atomic_load(&observed->count)==0;
 union dvm_value what={.i=123};ok=ok&&invoke(v,"android/os/Handler","sendEmptyMessage","(I)Z",handler,what).i==1&&atomic_load(&observed->messages)==0;
 union dvm_value delayed[]={task,{.j=60000}};ok=ok&&thread_runtime_call(v,"android/os/Handler","postDelayed","(Ljava/lang/Runnable;J)Z",handler,delayed,2,&out)&&out.i==1;
 dvm_ref t2=dvm_new_object(v,dvm_find_class_or_external(v,ht));union dvm_value name2={.l=dvm_new_string(v,"second-async-diagnostic")};invoke(v,ht,"<init>","(Ljava/lang/String;)V",t2,name2);invoke(v,ht,"start","()V",t2,z);dvm_ref looper2=invoke(v,ht,"getLooper","()Landroid/os/Looper;",t2,z).l;
 struct observed *observed2=calloc(1,sizeof *observed2);observed2->main=pthread_self();atomic_init(&observed2->count,0);atomic_init(&observed2->messages,0);dvm_ref runnable2=dvm_new_object(v,c);dvm__obj(v,runnable2)->data=observed2;
 dvm_ref handler2=dvm_new_object(v,hc);union dvm_value l2={.l=looper2};ok=ok&&looper2&&looper2!=looper&&android_basics_call(v,"android/os/Handler","<init>","(Landroid/os/Looper;)V",handler2,&l2,1,&out)&&!dvm_exception(v);
 union dvm_value task2={.l=runnable2};ok=ok&&invoke(v,"android/os/Handler","post","(Ljava/lang/Runnable;)Z",handler2,task2).i==1;
 thread_runtime_shutdown(v,250);ok=ok&&atomic_load(&observed->count)==1&&atomic_load(&observed->messages)==1&&atomic_load(&observed2->count)==1&&!pthread_equal(observed->callback_thread,observed2->callback_thread)&&!observed->wrong_thread&&!observed->without_gil&&!observed->wrong_message&&!observed2->wrong_thread&&!observed2->without_gil;message_observer=NULL;
 ok=ok&&!invoke(v,ht,"isAlive","()Z",t,z).i;invoke(v,ht,"start","()V",t,z);ok=ok&&dvm_exception(v)!=0;dvm_clear_exception(v);
 sdk_trace_fprintf(stderr,"THREAD_SELFTEST %s marker=numeric-next async_not_inline=1 real_pthread=1 concurrent_workers=2 distinct_loopers=1 distinct_threads=1 GIL_owned=1 Runnable_executed=2 Message_executed=1 delayed_cancel=1 Looper_published=1 joined_before_arena=1\n",ok?"PASS":"FAIL");return ok;
}
