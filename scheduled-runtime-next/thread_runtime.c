/* SPDX-License-Identifier: MPL-2.0
 * Adapted from pinned Lunaria bytecode_thread_main/ht_run/looper_loop boundary;
 * see thread-runtime-provenance.json and generated/LICENSE.
 * Minimal actual HandlerThread/Looper worker. VM entry follows upstream
 * bytecode_thread_main: mark -> GIL acquire -> TLS reset -> cur_thread -> call.
 * Queue ownership, publication and termination are guarded independently;
 * never acquire GIL while holding the queue mutex. Workers are owned by the
 * VM and joined before VM teardown. No main/UI event loop is provided. */
#include "thread_runtime.h"
#include "portable.h"
#include <stdlib.h>
#include <errno.h>
#include <stdatomic.h>
struct handler{dvm_ref looper,callback;bool async;};
struct task{dvm_ref handler,runnable,message;uint64_t due,sequence;struct task *next;};
struct worker{struct dvm *vm;dvm_ref self,name,looper;pthread_t thread;bool started,ready,alive,quit,joined,active;int priority;struct task *head;unsigned queued,executed,cancelled;struct worker *next;};
struct plain_thread { struct dvm *vm; dvm_ref self,target,name,group; bool daemon,started,alive; struct plain_thread *next; };
static struct plain_thread *plain_threads;
static struct plain_thread *plain_find(struct dvm *v,dvm_ref self){for(struct plain_thread *t=plain_threads;t;t=t->next)if(t->vm==v&&t->self==self)return t;return NULL;}
bool thread_runtime_executor_bind(struct dvm *v,dvm_ref self,dvm_ref target){
 struct plain_thread *t=plain_find(v,self);struct dvm_object *o=dvm__obj(v,self);
 if(!t||!o||strcmp(o->cls->name,"java/lang/Thread")||t->started||t->target!=target){dvm__throw(v,"java/lang/UnsupportedOperationException","factory must return a NEW plain Thread with the worker target");return false;}
 t->started=t->alive=true;return true;
}
void thread_runtime_executor_finished(struct dvm *v,dvm_ref self){struct plain_thread *t=plain_find(v,self);if(t)t->alive=false;}
static struct {struct dvm *vm;pthread_t main_host;dvm_ref main_java;struct worker *workers;uint64_t sequence;} run;
static pthread_mutex_t queue_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed=PTHREAD_COND_INITIALIZER;
static uint64_t now_ms(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000;}
static struct timespec deadline(unsigned ms){struct timespec t;clock_gettime(CLOCK_REALTIME,&t);t.tv_nsec+=(long)(ms%1000)*1000000;t.tv_sec+=ms/1000+t.tv_nsec/1000000000;t.tv_nsec%=1000000000;return t;}
static bool is_thread(struct dvm *v,dvm_ref self){struct dvm_object *o=dvm__obj(v,self);for(struct dvm_class *c=o?o->cls:NULL;c;c=c->super)if(!strcmp(c->name,"android/os/HandlerThread"))return true;return false;}
static void message_field(struct dvm_class *c,int index,const char *name,const char *type){struct dvm_field *f=&c->ifields[index];f->cls=c;f->name=name;f->type=type;f->slot=(uint16_t)index;f->width=1;f->kind=type[0]=='L'?'L':type[0];}
void thread_runtime_init(struct dvm *v){
 run.vm=v;run.main_host=pthread_self();run.workers=NULL;run.sequence=0;
 run.main_java=dvm_new_object(v,dvm_find_class_or_external(v,"java/lang/Thread"));
 struct dvm_class *c=dvm__class_by_desc(v,"Landroid/os/Message;");if(!c)return;c->ifields=calloc(6,sizeof *c->ifields);if(!c->ifields){dvm__throw(v,"java/lang/OutOfMemoryError","Message metadata");return;}c->nifields=c->islots=6;
 message_field(c,0,"what","I");message_field(c,1,"arg1","I");message_field(c,2,"arg2","I");message_field(c,3,"obj","Ljava/lang/Object;");message_field(c,4,"target","Landroid/os/Handler;");message_field(c,5,"when","J");
}
dvm_ref thread_runtime_current(struct dvm *v){return v->cur_thread?v->cur_thread:(run.vm==v&&pthread_equal(pthread_self(),run.main_host)?run.main_java:0);}
bool thread_runtime_is_main(struct dvm *v){return run.vm==v&&pthread_equal(pthread_self(),run.main_host);}
static struct worker *worker_for_looper(struct dvm *v,dvm_ref looper){for(struct worker *w=run.workers;w;w=w->next)if(w->vm==v&&w->ready&&looper==w->looper)return w;return NULL;}
static struct worker *worker_for_host(struct dvm *v,pthread_t host){for(struct worker *w=run.workers;w;w=w->next)if(w->vm==v&&w->started&&pthread_equal(host,w->thread))return w;return NULL;}
bool thread_runtime_looper(struct dvm *v,dvm_ref looper){return worker_for_looper(v,looper)!=NULL;}
static bool call_method(struct dvm *v,dvm_ref self,const char *name,const char *sig,const union dvm_value *a,int n){
 struct dvm_class *c=dvm_object_class(v,self);struct dvm_method *m=c?dvm_find_method(v,c,name,sig):NULL;union dvm_value out={0};
 if(!m){sdk_trace_missing_api(c?c->name:"?",name,sig);dvm__throw(v,"java/lang/NoSuchMethodError","async callback %s%s",name,sig);return false;}return dvm_call(v,m,self,a,n,&out);
}
static bool dispatch(struct worker *w,struct task *t){
 struct dvm *v=w->vm;dvm_set_step_limit(v,100000);
 if(t->runnable)return call_method(v,t->runnable,"run","()V",NULL,0);
 struct dvm_object *o=dvm__obj(v,t->handler);struct handler *h=o?o->data:NULL;union dvm_value arg={.l=t->message};
 if(h&&h->callback){struct dvm_method *m=dvm_find_method(v,dvm_object_class(v,h->callback),"handleMessage","(Landroid/os/Message;)Z");union dvm_value out={0};if(!m){sdk_trace_missing_api("android/os/Handler$Callback","handleMessage","(Landroid/os/Message;)Z");dvm__throw(v,"java/lang/NoSuchMethodError","Handler.Callback");return false;}if(!dvm_call(v,m,h->callback,&arg,1,&out))return false;if(out.i)return true;}
 struct dvm_class *c=dvm_object_class(v,t->handler);struct dvm_method *m=c?dvm_find_method(v,c,"handleMessage","(Landroid/os/Message;)V"):NULL;
 if(m){union dvm_value out={0};return dvm_call(v,m,t->handler,&arg,1,&out);}
 /* The DEX lookup returns overrides only when the boot Handler class is
  * linked into the guest hierarchy. Its inherited default is a no-op. */
 return true;
}
static void *worker_main(void *arg){
 struct worker *w=arg;struct dvm *v=w->vm;dvm__mark_bytecode_thread();dvm_gil_acquire(v);dvm__tstate_reset(v);v->cur_thread=w->self;v->quiet_uncaught=true;
 dvm_ref looper=dvm_new_object(v,dvm_find_class_or_external(v,"android/os/Looper"));
 pthread_mutex_lock(&queue_lock);w->looper=looper;w->ready=looper!=0;pthread_cond_broadcast(&changed);pthread_mutex_unlock(&queue_lock);
 if(!looper){dvm__throw(v,"java/lang/OutOfMemoryError","worker Looper");sdk_trace_async_failure(v,"prepare Looper");dvm_clear_exception(v);pthread_mutex_lock(&queue_lock);w->quit=true;pthread_mutex_unlock(&queue_lock);}
 struct dvm_method *prep=dvm_find_method(v,dvm_object_class(v,w->self),"onLooperPrepared","()V");
 if(prep&&(prep->has_code||prep->builtin)){union dvm_value out={0};dvm_set_step_limit(v,100000);if(!dvm_call(v,prep,w->self,NULL,0,&out)){sdk_trace_async_failure(v,"onLooperPrepared");dvm_clear_exception(v);pthread_mutex_lock(&queue_lock);w->quit=true;pthread_mutex_unlock(&queue_lock);}}
 dvm_gil_release(v);
 for(;;){pthread_mutex_lock(&queue_lock);while(!w->quit&&(!w->head||w->head->due>now_ms())){struct timespec until=deadline(20);pthread_cond_timedwait(&changed,&queue_lock,&until);}if(w->quit){pthread_mutex_unlock(&queue_lock);break;}struct task *t=w->head;w->head=t->next;--w->queued;w->active=true;pthread_mutex_unlock(&queue_lock);
  bool failed=false;dvm_gil_acquire(v);if(!dvm_is_stopping(v)){bool ok=dispatch(w,t);if(!ok||dvm_exception(v)){sdk_trace_async_failure(v,"dispatch");dvm_clear_exception(v);failed=true;}}free(t);dvm_gil_release(v);
  pthread_mutex_lock(&queue_lock);++w->executed;w->active=false;if(failed)w->quit=true;pthread_cond_broadcast(&changed);pthread_mutex_unlock(&queue_lock);
 }
 pthread_mutex_lock(&queue_lock);w->alive=false;pthread_cond_broadcast(&changed);pthread_mutex_unlock(&queue_lock);return NULL;
}
static bool start(struct dvm *v,struct worker *w){
 pthread_mutex_lock(&queue_lock);if(w->started){pthread_mutex_unlock(&queue_lock);dvm__throw(v,"java/lang/IllegalThreadStateException","HandlerThread already started");return true;}
 struct dvm_method *override=dvm_find_method(v,dvm_object_class(v,w->self),"run","()V");if(override&&(override->has_code||override->builtin)){pthread_mutex_unlock(&queue_lock);sdk_trace_missing_api(dvm_class_name(override->cls),"run","()V");dvm__throw(v,"java/lang/UnsupportedOperationException","custom Thread.run requires full worker lifecycle");return true;}
 w->started=w->alive=true;w->next=run.workers;run.workers=w;int rc=pthread_create(&w->thread,NULL,worker_main,w);if(rc){w->started=w->alive=false;run.workers=w->next;}pthread_mutex_unlock(&queue_lock);if(rc)dvm__throw(v,"java/lang/OutOfMemoryError","pthread_create error %d",rc);return true;
}
static dvm_ref await_looper(struct dvm *v,struct worker *w){
 uint64_t end=now_ms()+2000;pthread_mutex_lock(&queue_lock);
 while(w->alive&&!w->ready&&now_ms()<end){unsigned depth=dvm_gil_unlock_all(v);struct timespec until=deadline(20);pthread_cond_timedwait(&changed,&queue_lock,&until);pthread_mutex_unlock(&queue_lock);dvm_gil_relock(v,depth);pthread_mutex_lock(&queue_lock);}
 bool timeout=w->alive&&!w->ready;dvm_ref result=w->alive&&w->ready?w->looper:0;pthread_mutex_unlock(&queue_lock);if(timeout)dvm__throw(v,"java/lang/IllegalStateException","Looper publication timed out");return result;
}
static bool enqueue(struct dvm *v,dvm_ref handler,dvm_ref runnable,dvm_ref message,int64_t delay,union dvm_value *out){
 struct dvm_object *o=dvm__obj(v,handler);struct handler *h=o?o->data:NULL;struct worker *w=h?worker_for_looper(v,h->looper):NULL;
 if(!h||!w||!thread_runtime_looper(v,h->looper))return false;
 if(!runnable&&!message){dvm__throw(v,"java/lang/NullPointerException","posted task");return true;}
 struct task *t=calloc(1,sizeof *t);if(!t){dvm__throw(v,"java/lang/OutOfMemoryError","queued task");return true;}t->handler=handler;t->runnable=runnable;t->message=message;t->due=now_ms()+(delay>0?(uint64_t)delay:0);
 pthread_mutex_lock(&queue_lock);if(w->quit||!w->alive){pthread_mutex_unlock(&queue_lock);free(t);out->i=0;return true;}if(w->queued>=1024){pthread_mutex_unlock(&queue_lock);free(t);dvm__throw(v,"java/lang/IllegalStateException","diagnostic queue full");return true;}
 if(message){for(struct task *queued=w->head;queued;queued=queued->next)if(queued->message==message){pthread_mutex_unlock(&queue_lock);free(t);dvm__throw(v,"java/lang/IllegalStateException","Message already queued");return true;}
  union dvm_value target={.l=handler},when={.j=(int64_t)t->due};if(!dvm_set_field(v,message,"target","Landroid/os/Handler;",target)||!dvm_set_field(v,message,"when","J",when)){pthread_mutex_unlock(&queue_lock);free(t);dvm__throw(v,"java/lang/IllegalArgumentException","invalid Message object");return true;}}
 t->sequence=++run.sequence;struct task **at=&w->head;while(*at&&((*at)->due<t->due||((*at)->due==t->due&&(*at)->sequence<t->sequence)))at=&(*at)->next;t->next=*at;*at=t;++w->queued;out->i=1;pthread_cond_broadcast(&changed);pthread_mutex_unlock(&queue_lock);return true;
}
static bool send_empty(struct dvm *v,dvm_ref handler,int what,int64_t delay,union dvm_value *out){
 struct dvm_object *h=dvm__obj(v,handler);if(!h||!h->data||!thread_runtime_looper(v,((struct handler *)h->data)->looper))return false;
 dvm_ref message=dvm_new_object(v,dvm_find_class_or_external(v,"android/os/Message"));
 if(!message){dvm__throw(v,"java/lang/OutOfMemoryError","Message");return true;}
 union dvm_value value={.i=what};
 if(!dvm_set_field(v,message,"what","I",value)){dvm__throw(v,"java/lang/IllegalArgumentException","Message.what unavailable");return true;}
 return enqueue(v,handler,0,message,delay,out);
}
void thread_runtime_join_host(struct dvm *v,pthread_t thread){unsigned depth=dvm_gil_unlock_all(v);pthread_join(thread,NULL);dvm_gil_relock(v,depth);}
void thread_runtime_shutdown(struct dvm *v,unsigned grace){
 for(struct worker *w=run.workers;w;w=w->next){if(w->vm!=v||!w->started||w->joined)continue;uint64_t end=now_ms()+grace;pthread_mutex_lock(&queue_lock);
  while((w->active||(w->head&&w->head->due<=now_ms()))&&now_ms()<end){unsigned depth=dvm_gil_unlock_all(v);struct timespec until=deadline(10);pthread_cond_timedwait(&changed,&queue_lock,&until);pthread_mutex_unlock(&queue_lock);dvm_gil_relock(v,depth);pthread_mutex_lock(&queue_lock);}
  w->quit=true;while(w->head){struct task *t=w->head;w->head=t->next;free(t);++w->cancelled;}w->queued=0;bool busy=w->active;pthread_cond_broadcast(&changed);pthread_mutex_unlock(&queue_lock);if(busy)dvm_request_stop(v);
  thread_runtime_join_host(v,w->thread);w->joined=true;sdk_trace_fprintf(stderr,"THREAD_SHUTDOWN joined=1 executed=%u cancelled=%u busy_at_deadline=%d\n",w->executed,w->cancelled,busy);
 }
 struct plain_thread **pp=&plain_threads;while(*pp){struct plain_thread *t=*pp;if(t->vm==v){*pp=t->next;free(t);}else pp=&t->next;}
}
#define IS(name,signature) (!strcmp(m,name)&&!strcmp(sig,signature))
bool thread_runtime_call(struct dvm *v,const char *c,const char *m,const char *sig,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 (void)n;
 if(!strcmp(c,"java/lang/Thread")&&self&&!is_thread(v,self)){
  struct plain_thread *t=plain_find(v,self);
  if(!strcmp(m,"<init>")){
   if(t){dvm__throw(v,"java/lang/IllegalThreadStateException","Thread already initialized");return true;}
   dvm_ref target=0,name=0;
   if(!strcmp(sig,"(Ljava/lang/Runnable;)V"))target=a[0].l;
   else if(!strcmp(sig,"(Ljava/lang/Runnable;Ljava/lang/String;)V")){target=a[0].l;name=a[1].l;}
   else if(!strcmp(sig,"(Ljava/lang/ThreadGroup;Ljava/lang/Runnable;Ljava/lang/String;J)V")){if(a[3].j){dvm__throw(v,"java/lang/UnsupportedOperationException","custom stack size");return true;}target=a[1].l;name=a[2].l;}
   else if(!strcmp(sig,"()V")){}
   else return false;
   t=calloc(1,sizeof *t);if(!t){dvm__throw(v,"java/lang/OutOfMemoryError","Thread state");return true;}
   t->vm=v;t->self=self;t->target=target;if(!strcmp(sig,"(Ljava/lang/ThreadGroup;Ljava/lang/Runnable;Ljava/lang/String;J)V"))t->group=a[0].l;t->name=name?name:dvm_new_string(v,"Thread");t->next=plain_threads;plain_threads=t;return true;
  }
  if(t){
   if(IS("getName","()Ljava/lang/String;")){out->l=t->name;return true;}
   if(IS("setName","(Ljava/lang/String;)V")){if(!a[0].l)dvm__throw(v,"java/lang/NullPointerException","Thread name");else t->name=a[0].l;return true;}
   if(IS("setDaemon","(Z)V")){if(t->started)dvm__throw(v,"java/lang/IllegalThreadStateException","Thread already started");else t->daemon=a[0].i!=0;return true;}
   if(IS("isDaemon","()Z")){out->i=t->daemon;return true;}
   if(IS("isAlive","()Z")){out->i=t->alive;return true;}
   if(IS("getPriority","()I")){out->i=5;return true;}
   if(IS("getThreadGroup","()Ljava/lang/ThreadGroup;")){out->l=t->group;return true;}
   if(IS("setPriority","(I)V")){if(a[0].i!=5)dvm__throw(v,"java/lang/UnsupportedOperationException","nondefault Thread priority");return true;}
  }
 }
 if(is_thread(v,self)&&(!strcmp(c,"android/os/HandlerThread")||!strcmp(c,"java/lang/Thread"))){struct dvm_object *o=dvm__obj(v,self);struct worker *w=o->data;
  if(IS("<init>","(Ljava/lang/String;)V")||IS("<init>","(Ljava/lang/String;I)V")){if(!a[0].l){dvm__throw(v,"java/lang/NullPointerException","thread name");return true;}if(IS("<init>","(Ljava/lang/String;I)V")&&a[1].i!=0){dvm__throw(v,"java/lang/UnsupportedOperationException","nondefault OS worker priority not implemented");return true;}w=calloc(1,sizeof *w);if(!w){dvm__throw(v,"java/lang/OutOfMemoryError","worker state");return true;}w->vm=v;w->self=self;w->name=a[0].l;w->priority=n>1?a[1].i:0;o->data=w;return true;}
  if(!w)return false;
  if(IS("start","()V"))return start(v,w);if(IS("getLooper","()Landroid/os/Looper;")){out->l=await_looper(v,w);return true;}
  if(IS("getName","()Ljava/lang/String;")){out->l=w->name;return true;}if(IS("setName","(Ljava/lang/String;)V")){if(!a[0].l)dvm__throw(v,"java/lang/NullPointerException","thread name");else w->name=a[0].l;return true;}if(IS("isAlive","()Z")){pthread_mutex_lock(&queue_lock);out->i=w->alive;pthread_mutex_unlock(&queue_lock);return true;}
  if(IS("quit","()Z")){pthread_mutex_lock(&queue_lock);out->i=w->ready&&w->alive;if(out->i){w->quit=true;pthread_cond_broadcast(&changed);}pthread_mutex_unlock(&queue_lock);return true;}
 }
 if(!strcmp(c,"java/lang/Thread")&&IS("currentThread","()Ljava/lang/Thread;")){out->l=thread_runtime_current(v);if(!out->l)dvm__throw(v,"java/lang/IllegalStateException","host thread not attached to VM");return true;}
 if(!strcmp(c,"android/os/Looper")){
  if(IS("myLooper","()Landroid/os/Looper;")){struct worker *w=worker_for_host(v,pthread_self());if(w){out->l=w->looper;return true;}return false;}
  struct worker *w=thread_runtime_looper(v,self)?worker_for_looper(v,self):NULL;
  if(w&&IS("isCurrentThread","()Z")){out->i=pthread_equal(pthread_self(),w->thread);return true;}
  if(w&&IS("quit","()V")){pthread_mutex_lock(&queue_lock);w->quit=true;pthread_cond_broadcast(&changed);pthread_mutex_unlock(&queue_lock);return true;}
 }
 if(!strcmp(c,"android/os/Message")&&IS("obtain","()Landroid/os/Message;")){out->l=dvm_new_object(v,dvm_find_class_or_external(v,"android/os/Message"));return true;}
 if(!strcmp(c,"android/os/Handler")){
  if(IS("post","(Ljava/lang/Runnable;)Z"))return enqueue(v,self,a[0].l,0,0,out);
  if(IS("postDelayed","(Ljava/lang/Runnable;J)Z"))return enqueue(v,self,a[0].l,0,a[1].j,out);
  if(IS("sendMessage","(Landroid/os/Message;)Z"))return enqueue(v,self,0,a[0].l,0,out);
  if(IS("sendEmptyMessage","(I)Z"))return send_empty(v,self,a[0].i,0,out);
  if(IS("sendEmptyMessageDelayed","(IJ)Z"))return send_empty(v,self,a[0].i,a[1].j,out);
 }
 return false;
}
