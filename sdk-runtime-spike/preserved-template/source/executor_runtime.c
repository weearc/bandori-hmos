/* Real bounded queue and executor workers for the isolated SDK runner. */
#include "executor_runtime.h"
#include "java_basics.h"
#include "thread_runtime.h"
#include "portable.h"
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>

enum { QUEUE_LINKED = 1, QUEUE_SYNC, FUTURE_NEW, FUTURE_RUNNING,
       FUTURE_DONE, FUTURE_FAILED, FUTURE_CANCELLED };
enum { QUEUE_LIMIT = 65536 };
/* ScheduledFutureTask re-execution policy.  PERIODIC_NONE is one-shot
 * schedule(); the other two follow AOSP's ScheduledFutureTask.reExecutePeriodic:
 * fixed-delay waits period from completion, fixed-rate advances the previous
 * deadline by period (so a long task catches up immediately). */
enum { PERIODIC_NONE = 0, PERIODIC_FIXED_DELAY, PERIODIC_FIXED_RATE };

struct queue_state {
 struct dvm *vm;
 dvm_ref self;
 int kind;
 size_t maximum,size,allocated,head;
 dvm_ref *items;
 unsigned waiting_takers;
 dvm_ref handoff;
 bool has_handoff,closed;
 pthread_mutex_t lock;
 pthread_cond_t changed;
 struct queue_state *next;
};
struct executor_state;
struct executor_worker {
 struct executor_state *pool;
 pthread_t thread;
 dvm_ref self,first_task,trampoline;
 bool started,joined,alive;
 struct executor_worker *next;
};
struct executor_state {
 struct dvm *vm;
 dvm_ref self,queue_ref,handler,factory;
 struct queue_state *queue;
 unsigned core,maximum,live,active,executed,cancelled;
 uint64_t keep_alive_ms;
 bool shutdown,scheduled;
 /* ThreadPoolExecutor.allowCoreThreadTimeOut: when true, idle core workers
  * also use keep_alive_ms.  Java requires keepAliveTime > 0 to enable it; the
  * setter enforces that below. */
 bool allow_core_timeout;
 struct executor_worker *workers;
 struct executor_state *next;
};
struct future_state {
 dvm_ref callable,runnable,result,exception;
 uint64_t due_ms; /* monotonic deadline for scheduled futures */
 uint64_t period_ms; /* re-execution period for periodic futures */
 struct queue_state *scheduled_queue;
 int periodic; /* PERIODIC_NONE / PERIODIC_FIXED_DELAY / PERIODIC_FIXED_RATE */
 int status;
 pthread_mutex_t lock;
 pthread_cond_t changed;
};
struct executor_test_value { dvm_ref result; };
static struct queue_state *all_queues;
static struct executor_state *all_executors;
static void *executor_worker_main(void *arg);
static bool executor_worker_run(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out);
static bool executor_execute(struct dvm *v,struct executor_state *p,dvm_ref task);
/* Thread-runtime-side Java priority storage for ThreadFactory-created Threads;
 * declared here because thread_runtime.h is not part of this change. */
extern bool thread_runtime_executor_apply_priority(struct dvm *,dvm_ref,int *,int *,int *);
static void borrowed_worker_destroy(void *data){(void)data;}
static bool worker_trampoline(struct dvm *v,struct executor_worker *w){
 struct dvm_class *c=dvm_find_class_or_external(v,"java/util/concurrent/ExecutorRuntimeWorker");
 if(!c)return false;
 if(!c->nmethods){struct dvm_method *m=calloc(1,sizeof *m);if(!m)return false;m->cls=c;m->name="run";m->sig=strdup("()V");m->builtin=executor_worker_run;m->ret_kind='V';c->methods=m;c->nmethods=1;}
 w->trampoline=dvm_new_object(v,c);struct dvm_object *o=dvm__obj(v,w->trampoline);if(!o)return false;o->data=w;o->data_destroy=borrowed_worker_destroy;return true;
}
static _Atomic unsigned executor_test_calls;
static dvm_ref fixture_thread;
static _Atomic unsigned fixture_rejections;
static bool fixture_factory(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 (void)self;if(n!=1)return false;struct dvm_class *c=dvm_find_class_or_external(v,"java/lang/Thread");out->l=dvm_new_object(v,c);fixture_thread=out->l;
 union dvm_value ignored={0};
 if(!thread_runtime_call(v,"java/lang/Thread","<init>","(Ljava/lang/Runnable;)V",out->l,a,1,&ignored))return false;
 /* Mirrors BLogThreadFactory.newThread: construct the Thread, then raise its
  * Java priority before returning it to the executor.  This makes the
  * scheduled-factory selftest cover the real bind + priority path. */
 union dvm_value priority={.i=10};
 return thread_runtime_call(v,"java/lang/Thread","setPriority","(I)V",out->l,&priority,1,&ignored);
}
static bool fixture_reject(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){(void)v;(void)self;(void)out;if(n!=2||!a[0].l||!a[1].l)return false;atomic_fetch_add(&fixture_rejections,1);return true;}
static bool fixture_install(struct dvm_class *c,const char *name,const char *sig,bool (*fn)(struct dvm *,dvm_ref,const union dvm_value *,int,union dvm_value *),char ret){
 if(!c||c->nmethods)return false;struct dvm_method *m=calloc(1,sizeof *m);if(!m)return false;m->cls=c;m->name=name;m->sig=strdup(sig);m->builtin=fn;m->ret_kind=ret;c->methods=m;c->nmethods=1;return true;
}
static bool executor_test_callable(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 (void)a;(void)n;struct dvm_object *o=dvm__obj(v,self);struct executor_test_value *value=o?o->data:NULL;
 if(!value){dvm__throw(v,"java/lang/IllegalStateException","executor selftest callable data");return false;}
 atomic_fetch_add(&executor_test_calls,1);out->l=value->result;return true;
}
static bool install_executor_test_callable(struct dvm *v,struct dvm_class *cls){
 if(!cls||cls->nmethods){dvm__throw(v,"java/lang/IllegalStateException","executor selftest class is not empty");return false;}
 struct dvm_method *methods=calloc(1,sizeof *methods);char *sig=calloc(1,sizeof "()Ljava/lang/Object;");
 if(!methods||!sig){free(methods);free(sig);dvm__throw(v,"java/lang/OutOfMemoryError","executor selftest method");return false;}
 memcpy(sig,"()Ljava/lang/Object;",sizeof "()Ljava/lang/Object;");methods->cls=cls;methods->name="call";methods->sig=sig;methods->builtin=executor_test_callable;methods->ret_kind='L';
 cls->methods=methods;cls->nmethods=1;return true;
}
/* scheduleWithFixedDelay / scheduleAtFixedRate take a plain Runnable.  The real
 * AOSP path calls command.run() on every period; a FutureTask passed as that
 * command would only run its callable once (FutureTask.run() is a no-op after
 * completion), which is correct Java semantics but useless as a repetition
 * probe.  This fixture counts every run() invocation, so the selftest measures
 * the scheduler itself instead of FutureTask's one-shot state machine. */
static bool executor_test_runnable(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 (void)v;(void)self;(void)a;(void)n;(void)out;
 atomic_fetch_add(&executor_test_calls,1);return true;
}
static bool install_executor_test_runnable(struct dvm *v,struct dvm_class *cls){
 if(!cls||cls->nmethods){dvm__throw(v,"java/lang/IllegalStateException","executor selftest runnable class is not empty");return false;}
 struct dvm_method *methods=calloc(1,sizeof *methods);char *sig=calloc(1,sizeof "()V");
 if(!methods||!sig){free(methods);free(sig);dvm__throw(v,"java/lang/OutOfMemoryError","executor selftest runnable method");return false;}
 memcpy(sig,"()V",sizeof "()V");methods->cls=cls;methods->name="run";methods->sig=sig;methods->builtin=executor_test_runnable;methods->ret_kind='V';
 cls->methods=methods;cls->nmethods=1;return true;
}

static uint64_t now_ms(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000u+(uint64_t)t.tv_nsec/1000000u;}
static struct timespec deadline_ms(uint64_t ms){struct timespec t;clock_gettime(CLOCK_REALTIME,&t);t.tv_sec+=(time_t)(ms/1000u);t.tv_nsec+=(long)(ms%1000u)*1000000L;if(t.tv_nsec>=1000000000L){++t.tv_sec;t.tv_nsec-=1000000000L;}return t;}
static uint64_t deadline_after(uint64_t duration){uint64_t now=now_ms();return duration>UINT64_MAX-now?UINT64_MAX:now+duration;}
static uint64_t duration_ms(uint64_t count,uint64_t nanos){__uint128_t total=(__uint128_t)count*nanos+999999u;total/=1000000u;return total>UINT64_MAX?UINT64_MAX:(uint64_t)total;}
static bool is_name(struct dvm_object *o,const char *name){for(struct dvm_class *k=o?o->cls:NULL;k;k=k->super)if(k->name&&!strcmp(k->name,name))return true;return false;}
static bool exception_is(struct dvm *v,const char *name){dvm_ref e=dvm_exception(v);struct dvm_object *o=e?dvm__obj(v,e):NULL;return o&&o->cls&&o->cls->name&&!strcmp(o->cls->name,name);}

/* ---- AOSP Executors.DefaultThreadFactory -------------------------------
 * Executors.defaultThreadFactory() 返回真实对象，类名与 AOSP 相同。这个对象
 * 不是空壳：它的类上安装了真实的 newThread(Runnable) builtin，按 AOSP 语义
 * 分配 java/lang/Thread 并执行 Thread.<init>(Runnable)。因此:
 *   - start_worker 的 factory 路径会 dvm_call 这个 newThread，再把返回的
 *     Thread 交给 thread_runtime_executor_bind 绑定 worker trampoline；
 *   - 应用直接调用 factory.newThread(r) 也会得到真实 Thread，而不是假的
 *     成功或 NoSuchMethodError。
 * AOSP DefaultThreadFactory 会把线程设为 daemon=false、priority=NORM(5)；
 * 本运行时的 Thread 默认值就是 0/5，故不需要额外 setDaemon/setPriority。
 * 未实现：AOSP 的 "pool-N-thread-M" 线程命名（Thread 名保持默认 "Thread"），
 * 以及每个工厂实例独立的 poolNumber/threadNumber 计数。 */
struct default_factory_state { struct dvm *vm; };
static void default_factory_state_destroy(void *data){free(data);}
static struct dvm *default_factory_vm;
static dvm_ref default_factory_ref;
static bool default_factory_new_thread(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 (void)self;
 if(n!=1){dvm__throw(v,"java/lang/IllegalArgumentException","DefaultThreadFactory.newThread arity %d",n);return false;}
 struct dvm_class *thread_class=dvm_find_class_or_external(v,"java/lang/Thread");
 dvm_ref thread=thread_class?dvm_new_object(v,thread_class):0;
 if(!thread){dvm__throw(v,"java/lang/OutOfMemoryError","DefaultThreadFactory Thread");return false;}
 union dvm_value arg={.l=a? a[0].l:0},ignored={0};
 if(!thread_runtime_call(v,"java/lang/Thread","<init>","(Ljava/lang/Runnable;)V",thread,&arg,1,&ignored)){
  if(!dvm_exception(v))dvm__throw(v,"java/lang/NoSuchMethodError","java/lang/Thread.<init>(Ljava/lang/Runnable;)V");
  return false;
 }
 out->l=thread;return true;
}
static bool install_default_factory_method(struct dvm *v,struct dvm_class *c){
 if(!c)return false;
 if(dvm_find_method(v,c,"newThread","(Ljava/lang/Runnable;)Ljava/lang/Thread;"))return true;
 /* 类上已经有别的方法时不追加：class->methods 可能被其他代码扩容，追加会
  * 让 mcache 与活动调用帧里已缓存的方法指针失效。此时 executor_ctor 的
  * 正规化回退会保证线程池仍走默认 worker 路径。 */
 if(c->nmethods)return false;
 struct dvm_method *m=calloc(1,sizeof *m);
 char *sig=m?strdup("(Ljava/lang/Runnable;)Ljava/lang/Thread;"):NULL;
 if(!m||!sig){free(m);free(sig);dvm__throw(v,"java/lang/OutOfMemoryError","DefaultThreadFactory.newThread method");return false;}
 m->cls=c;m->name="newThread";m->sig=sig;m->builtin=default_factory_new_thread;
 m->access=DEX_ACC_PUBLIC;m->arg_slots=1;m->arg_count=1;m->ret_kind='L';
 c->methods=m;c->nmethods=1;return true;
}
static bool is_default_thread_factory(struct dvm *v,dvm_ref ref){
 struct dvm_object *o=ref?dvm__obj(v,ref):NULL;
 return o&&o->cls&&o->cls->name&&!strcmp(o->cls->name,"java/util/concurrent/Executors$DefaultThreadFactory");
}
static dvm_ref default_thread_factory(struct dvm *v){
 struct dvm_object *cached=default_factory_ref?dvm__obj(v,default_factory_ref):NULL;
 bool valid=cached&&cached->data_destroy==default_factory_state_destroy&&cached->data&&
            ((struct default_factory_state*)cached->data)->vm==v;
 if(default_factory_vm!=v||!valid)default_factory_ref=0;
 if(!default_factory_ref){
  struct dvm_class *c=dvm_find_class_or_external(v,"java/util/concurrent/Executors$DefaultThreadFactory");
  if(!c){dvm__throw(v,"java/lang/NoClassDefFoundError","java/util/concurrent/Executors$DefaultThreadFactory");return 0;}
  struct default_factory_state *state=calloc(1,sizeof *state);
  if(!state){dvm__throw(v,"java/lang/OutOfMemoryError","DefaultThreadFactory state");return 0;}
  dvm_ref ref=dvm_new_object(v,c);struct dvm_object *o=ref?dvm__obj(v,ref):NULL;
  if(!o){free(state);if(!dvm_exception(v))dvm__throw(v,"java/lang/OutOfMemoryError","Executors DefaultThreadFactory");return 0;}
  state->vm=v;o->data=state;o->data_destroy=default_factory_state_destroy;
  bool installed=install_default_factory_method(v,c);
  if(dvm_exception(v))return 0;
  dvm_pin(v,ref);default_factory_vm=v;default_factory_ref=ref;
  sdk_trace_fprintf(stderr,"EXECUTORS_CACHED default_factory=created new_thread=%d\n",installed?1:0);
 }
 return default_factory_ref;
}
static void queue_destroy(void *data){
 struct queue_state *q=data;if(!q)return;struct queue_state **at=&all_queues;while(*at&&*at!=q)at=&(*at)->next;if(*at)*at=q->next;
 pthread_cond_destroy(&q->changed);pthread_mutex_destroy(&q->lock);free(q->items);free(q);
}
static void executor_destroy(void *data){
 struct executor_state *p=data;if(!p)return;struct executor_state **at=&all_executors;while(*at&&*at!=p)at=&(*at)->next;if(*at)*at=p->next;
 struct executor_worker *w=p->workers;while(w){struct executor_worker *next=w->next;free(w);w=next;}free(p);
}
static void future_destroy(void *data){
 struct future_state *f=data;if(!f)return;pthread_cond_destroy(&f->changed);pthread_mutex_destroy(&f->lock);free(f);
}
static struct queue_state *queue_of(struct dvm *v,dvm_ref ref){
 struct dvm_object *o=dvm__obj(v,ref);if(!o||(!is_name(o,"java/util/concurrent/LinkedBlockingDeque")&&!is_name(o,"java/util/concurrent/LinkedBlockingQueue")&&!is_name(o,"java/util/concurrent/ArrayBlockingQueue")&&!is_name(o,"java/util/concurrent/SynchronousQueue")))return NULL;struct queue_state *q=o->data;
 if(!q||q->vm!=v||q->self!=ref)return NULL;return q;
}
static struct queue_state *new_queue(struct dvm *v,dvm_ref self,int kind,size_t maximum){
 struct dvm_object *o=dvm__obj(v,self);if(!o){dvm__throw(v,"java/lang/NullPointerException","queue receiver");return NULL;}
 if(o->data){dvm__throw(v,"java/lang/IllegalStateException","queue already initialized");return NULL;}
 struct queue_state *q=calloc(1,sizeof *q);if(!q){dvm__throw(v,"java/lang/OutOfMemoryError","queue state");return NULL;}
 if(pthread_mutex_init(&q->lock,NULL)){free(q);dvm__throw(v,"java/lang/OutOfMemoryError","queue mutex");return NULL;}
 if(pthread_cond_init(&q->changed,NULL)){pthread_mutex_destroy(&q->lock);free(q);dvm__throw(v,"java/lang/OutOfMemoryError","queue condition");return NULL;}
 q->vm=v;q->self=self;q->kind=kind;q->maximum=maximum;q->next=all_queues;all_queues=q;o->data=q;o->data_destroy=queue_destroy;return q;
}
static bool queue_grow(struct queue_state *q){
 if(q->allocated>=q->maximum||q->allocated>=QUEUE_LIMIT)return false;
 size_t cap=q->allocated?q->allocated*2:8;if(cap<q->allocated||cap>q->maximum)cap=q->maximum;if(cap>QUEUE_LIMIT)cap=QUEUE_LIMIT;
 dvm_ref *items=calloc(cap,sizeof *items);if(!items)return false;
 for(size_t i=0;i<q->size;++i)items[i]=q->items[(q->head+i)%q->allocated];
 free(q->items);q->items=items;q->allocated=cap;q->head=0;return true;
}
enum put_result { PUT_OK, PUT_FULL, PUT_OOM, PUT_CLOSED };
static enum put_result queue_put_locked(struct queue_state *q,dvm_ref item,bool first){
 if(q->closed)return PUT_CLOSED;
 if(q->kind==QUEUE_SYNC){if(!q->waiting_takers||q->has_handoff)return PUT_FULL;q->handoff=item;q->has_handoff=true;pthread_cond_broadcast(&q->changed);return PUT_OK;}
 if(q->size>=q->maximum)return PUT_FULL;
 if(q->size==q->allocated&&!queue_grow(q))return q->allocated>=q->maximum||q->allocated>=QUEUE_LIMIT?PUT_FULL:PUT_OOM;
 if(first)q->head=(q->head+q->allocated-1)%q->allocated;
 size_t index=first?q->head:(q->head+q->size)%q->allocated;q->items[index]=item;++q->size;pthread_cond_broadcast(&q->changed);return PUT_OK;
}
static dvm_ref queue_pop_locked(struct queue_state *q,bool first){
 if(q->kind==QUEUE_SYNC){if(!q->has_handoff)return 0;dvm_ref item=q->handoff;q->handoff=0;q->has_handoff=false;pthread_cond_broadcast(&q->changed);return item;}
 if(!q->size)return 0;size_t index=first?q->head:(q->head+q->size-1)%q->allocated;dvm_ref item=q->items[index];q->items[index]=0;if(first)q->head=(q->head+1)%q->allocated;--q->size;pthread_cond_broadcast(&q->changed);return item;
}
static size_t queue_count_locked(struct queue_state *q){return q->kind==QUEUE_SYNC?0:q->size;}
static bool queue_wait_locked(struct dvm *v,struct queue_state *q,uint64_t wait_ms){
 unsigned depth=dvm_gil_unlock_all(v);struct timespec until=deadline_ms(wait_ms);int rc=pthread_cond_timedwait(&q->changed,&q->lock,&until);pthread_mutex_unlock(&q->lock);dvm_gil_relock(v,depth);pthread_mutex_lock(&q->lock);return rc==ETIMEDOUT;
}
static dvm_ref queue_take(struct dvm *v,struct queue_state *q,bool first,bool blocking,uint64_t timeout_ms,bool timed,bool *timed_out,bool *interrupted){
 uint64_t end=now_ms()+timeout_ms;if(timed_out)*timed_out=false;if(interrupted)*interrupted=false;
 pthread_mutex_lock(&q->lock);
 for(;;){
  dvm_ref item=queue_pop_locked(q,first);if(item){pthread_mutex_unlock(&q->lock);return item;}
  if(q->closed||dvm_is_stopping(v)){if(interrupted)*interrupted=true;pthread_mutex_unlock(&q->lock);return 0;}
  if(!blocking){pthread_mutex_unlock(&q->lock);return 0;}
  if(timed&&now_ms()>=end){if(timed_out)*timed_out=true;pthread_mutex_unlock(&q->lock);return 0;}
  if(q->kind==QUEUE_SYNC)++q->waiting_takers;
  uint64_t wait=timed?end-now_ms():50;if(wait>50)wait=50;queue_wait_locked(v,q,wait);
  if(q->kind==QUEUE_SYNC&&q->waiting_takers)--q->waiting_takers;
 }
}
static bool queue_put(struct dvm *v,struct queue_state *q,dvm_ref item,bool first,bool blocking,uint64_t timeout_ms,bool timed,union dvm_value *out){
 if(!item){dvm__throw(v,"java/lang/NullPointerException","queue elements cannot be null");return true;}
 if(!dvm__obj(v,item)){dvm__throw(v,"java/lang/IllegalArgumentException","invalid queue element");return true;}
 uint64_t end=now_ms()+timeout_ms;pthread_mutex_lock(&q->lock);
 for(;;){enum put_result r=queue_put_locked(q,item,first);if(r==PUT_OK){if(out)out->i=1;pthread_mutex_unlock(&q->lock);return true;}
  if(r==PUT_OOM){pthread_mutex_unlock(&q->lock);dvm__throw(v,"java/lang/OutOfMemoryError","diagnostic queue storage");return true;}
  if(r==PUT_CLOSED||q->closed||dvm_is_stopping(v)){pthread_mutex_unlock(&q->lock);dvm__throw(v,"java/lang/InterruptedException","queue closed during VM shutdown");return true;}
  if(!blocking){if(out)out->i=0;pthread_mutex_unlock(&q->lock);return true;}
  if(timed&&now_ms()>=end){if(out)out->i=0;pthread_mutex_unlock(&q->lock);return true;}
  uint64_t wait=timed?end-now_ms():50;if(wait>50)wait=50;queue_wait_locked(v,q,wait);
 }
}
static bool queue_snapshot(struct dvm *v,struct queue_state *q,dvm_ref **items,size_t *count){
 pthread_mutex_lock(&q->lock);size_t n=q->kind==QUEUE_SYNC?0:q->size;dvm_ref *copy=n?calloc(n,sizeof *copy):NULL;
 if(n&&!copy){pthread_mutex_unlock(&q->lock);dvm__throw(v,"java/lang/OutOfMemoryError","queue snapshot");return false;}
 for(size_t i=0;i<n;++i)copy[i]=q->items[(q->head+i)%q->allocated];pthread_mutex_unlock(&q->lock);*items=copy;*count=n;return true;
}
static bool queue_contains(struct dvm *v,struct queue_state *q,dvm_ref target,bool remove,bool last,union dvm_value *out){
 if(!target){if(out)out->i=0;return true;}
 dvm_ref *items=NULL;size_t count=0;if(!queue_snapshot(v,q,&items,&count))return true;size_t found=count;for(size_t i=0;i<count;++i){size_t at=last?count-1-i:i;bool equal=java_basics_equal(v,items[at],target);if(dvm_exception(v)){free(items);return true;}if(equal){found=at;break;}}
 bool removed=false;if(found<count&&remove){pthread_mutex_lock(&q->lock);for(size_t i=0;i<q->size;++i){size_t ix=(q->head+i)%q->allocated;if(q->items[ix]==items[found]){for(size_t j=i;j+1<q->size;++j)q->items[(q->head+j)%q->allocated]=q->items[(q->head+j+1)%q->allocated];q->items[(q->head+q->size-1)%q->allocated]=0;--q->size;removed=true;pthread_cond_broadcast(&q->changed);break;}}pthread_mutex_unlock(&q->lock);}
 free(items);if(out)out->i=remove?removed:found<count;return true;
}
static bool unit_millis(struct dvm *v,dvm_ref unit,uint64_t *scale){
 if(!unit){dvm__throw(v,"java/lang/NullPointerException","TimeUnit");return false;}
 if(!java_basics_timeunit_nanos(v,unit,scale)){dvm__throw(v,"java/lang/IllegalArgumentException","unrecognized TimeUnit");return false;}
 return true;
}

static struct executor_state *executor_of(struct dvm *v,dvm_ref self){struct dvm_object *o=dvm__obj(v,self);struct executor_state *p=o&&(is_name(o,"java/util/concurrent/ThreadPoolExecutor")||is_name(o,"java/util/concurrent/ScheduledThreadPoolExecutor"))?o->data:NULL;return p&&p->vm==v&&p->self==self?p:NULL;}
static struct future_state *future_of(struct dvm *v,dvm_ref self){struct dvm_object *o=dvm__obj(v,self);return o&&is_name(o,"java/util/concurrent/FutureTask")?o->data:NULL;}
static bool invoke_runnable(struct dvm *v,dvm_ref runnable){
 struct dvm_object *o=dvm__obj(v,runnable);if(!o){dvm__throw(v,"java/lang/NullPointerException","executor task");return false;}
 bool future_like=is_name(o,"java/util/concurrent/FutureTask");
 sdk_trace_fprintf(stderr,"RUNNABLE_DISPATCH class=%s future_like=%d data=%p destroy=%p\n",
                   o->cls&&o->cls->name?o->cls->name:"?",future_like,o->data,(void*)o->data_destroy);
 if(future_like){union dvm_value ignored={0};return executor_runtime_call(v,"java/util/concurrent/FutureTask","run","()V",runnable,NULL,0,&ignored);}
 struct dvm_method *m=dvm_find_method(v,o->cls,"run","()V");if(!m){sdk_trace_missing_api(o->cls?o->cls->name:"?","run","()V");dvm__throw(v,"java/lang/NoSuchMethodError","Runnable.run");return false;}
 union dvm_value out={0};return dvm_call(v,m,runnable,NULL,0,&out);
}
/* ---- AOSP ThreadPoolExecutor rejection policies -------------------------
 * The DEX references exactly three policy classes: DiscardPolicy (12 sites),
 * CallerRunsPolicy (1) and DiscardOldestPolicy (1); AbortPolicy is unused.
 * Their constructors are empty and rejectedExecution is the whole behavior:
 *   DiscardPolicy       : silently drop the task; execute() returns normally.
 *   CallerRunsPolicy    : run the task on the caller unless the pool is shut
 *                         down (AOSP runs r.run(), not the executor).
 *   DiscardOldestPolicy : if not shut down, poll and drop the queue head,
 *                         then retry e.execute(r).
 * The task is never executed by DiscardPolicy; no Thread is fabricated. */
static bool rejection_policy_ctor(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 (void)a;(void)out;
 if(n!=0){dvm__throw(v,"java/lang/IllegalArgumentException","rejection policy arity %d",n);return true;}
 if(!self||!dvm__obj(v,self)){dvm__throw(v,"java/lang/NullPointerException","rejection policy receiver");return true;}
 return true;
}
static bool rejection_discard(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 (void)v;(void)self;(void)a;(void)n;(void)out;return true;
}
static bool rejection_caller_runs(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 (void)self;(void)out;
 if(n!=2||!a||!a[0].l||!a[1].l){dvm__throw(v,"java/lang/NullPointerException","CallerRunsPolicy task or executor");return false;}
 struct executor_state *p=executor_of(v,a[1].l);
 if(!p){dvm__throw(v,"java/lang/IllegalArgumentException","CallerRunsPolicy requires a ThreadPoolExecutor");return false;}
 bool shutdown;{pthread_mutex_lock(&p->queue->lock);shutdown=p->shutdown;pthread_mutex_unlock(&p->queue->lock);}
 if(shutdown)return true;
 return invoke_runnable(v,a[0].l);
}
static bool rejection_discard_oldest(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 (void)self;(void)out;
 if(n!=2||!a||!a[0].l||!a[1].l){dvm__throw(v,"java/lang/NullPointerException","DiscardOldestPolicy task or executor");return false;}
 struct executor_state *p=executor_of(v,a[1].l);
 if(!p){dvm__throw(v,"java/lang/IllegalArgumentException","DiscardOldestPolicy requires a ThreadPoolExecutor");return false;}
 bool shutdown;{pthread_mutex_lock(&p->queue->lock);shutdown=p->shutdown;pthread_mutex_unlock(&p->queue->lock);}
 if(shutdown)return true;
 /* AOSP DiscardOldestPolicy only polls the head (dropping it without
  * cancelling its Future) and retries execute(r).  Do not fabricate a
  * CancellationException; a dropped FutureTask simply never completes. */
 pthread_mutex_lock(&p->queue->lock);
 (void)queue_pop_locked(p->queue,true);
 pthread_mutex_unlock(&p->queue->lock);
 return executor_execute(v,p,a[0].l);
}
static bool rejection_policy_dispatch(struct dvm *v,dvm_ref handler,const union dvm_value *a,int n,union dvm_value *out){
 struct dvm_object *o=dvm__obj(v,handler);const char *name=o&&o->cls?o->cls->name:NULL;
 if(!name)return false;
 if(!strcmp(name,"java/util/concurrent/ThreadPoolExecutor$DiscardPolicy"))return rejection_discard(v,handler,a,n,out);
 if(!strcmp(name,"java/util/concurrent/ThreadPoolExecutor$CallerRunsPolicy"))return rejection_caller_runs(v,handler,a,n,out);
 if(!strcmp(name,"java/util/concurrent/ThreadPoolExecutor$DiscardOldestPolicy"))return rejection_discard_oldest(v,handler,a,n,out);
 return false;
}
static void discard_worker(struct dvm *v,struct executor_worker *w){if(w){struct dvm_object *o=dvm__obj(v,w->trampoline);if(o)o->data=NULL;free(w);}}
static struct executor_worker *start_worker(struct executor_state *p,dvm_ref first_task,bool require_space){
 struct dvm *v=p->vm;struct executor_worker *w=calloc(1,sizeof *w);if(!w){dvm__throw(v,"java/lang/OutOfMemoryError","executor worker state");return NULL;}
 if(p->factory){
  if(!worker_trampoline(v,w)){discard_worker(v,w);dvm__throw(v,"java/lang/OutOfMemoryError","executor worker trampoline");return NULL;}
  struct dvm_object *fo=dvm__obj(v,p->factory);struct dvm_method *fm=fo?dvm_find_method(v,fo->cls,"newThread","(Ljava/lang/Runnable;)Ljava/lang/Thread;"):NULL;
  if(!fm){discard_worker(v,w);dvm__throw(v,"java/lang/NoSuchMethodError","ThreadFactory.newThread");return NULL;}
  union dvm_value arg={.l=w->trampoline},ret={0};if(!dvm_call(v,fm,p->factory,&arg,1,&ret)||dvm_exception(v)){discard_worker(v,w);return NULL;}
  if(!ret.l){discard_worker(v,w);return NULL;}w->self=ret.l;
 }else{struct dvm_class *thread_class=dvm_find_class_or_external(v,"java/lang/Thread");w->self=thread_class?dvm_new_object(v,thread_class):0;}if(!w->self){discard_worker(v,w);dvm__throw(v,"java/lang/OutOfMemoryError","executor Thread");return NULL;}
 w->pool=p;w->first_task=first_task;if(!w->trampoline&&!worker_trampoline(v,w)){discard_worker(v,w);dvm__throw(v,"java/lang/OutOfMemoryError","worker trampoline");return NULL;}
 pthread_mutex_lock(&p->queue->lock);
 if(p->shutdown||p->live>=p->maximum||(require_space&&p->live&&p->live>=p->core)){pthread_mutex_unlock(&p->queue->lock);discard_worker(v,w);return NULL;}
 if(p->factory&&!thread_runtime_executor_bind(v,w->self,w->trampoline)){pthread_mutex_unlock(&p->queue->lock);discard_worker(v,w);return NULL;}
 w->alive=true;w->started=true;w->next=p->workers;p->workers=w;++p->live;
 int rc=pthread_create(&w->thread,NULL,executor_worker_main,w);
 if(rc){if(p->factory)thread_runtime_executor_finished(v,w->self);p->workers=w->next;--p->live;w->alive=w->started=false;pthread_mutex_unlock(&p->queue->lock);discard_worker(v,w);dvm__throw(v,"java/lang/OutOfMemoryError","executor pthread_create error %d",rc);return NULL;}
 pthread_cond_broadcast(&p->queue->changed);pthread_mutex_unlock(&p->queue->lock);return w;
}
/* Re-arm a periodic ScheduledFutureTask after one run.  The caller holds the
 * queue lock, so this uses the same q->lock -> f->lock order as shutdown and
 * the cancel path; cancel never holds both locks at once.  AOSP semantics:
 * fixed-delay counts the period from completion, fixed-rate advances the
 * previous deadline (an overdue task runs again immediately, one at a time).
 * Periodic futures never reach the DONE state on success: future_run puts
 * them back to NEW so isDone() stays false and the next run is allowed.
 * Shutdown cancels periodic tasks (AOSP's default
 * continueExistingPeriodicTasksAfterShutdownPolicy is false); a re-arm that
 * cannot be queued fails the future rather than leaving it NEW forever. */
static void scheduled_requeue(struct dvm *v,struct executor_state *p,dvm_ref task,uint64_t completed_ms){
 struct future_state *f=future_of(v,task);if(!f)return;bool requeue=false,drop=false;
 pthread_mutex_lock(&f->lock);
 if(f->periodic!=PERIODIC_NONE&&f->status==FUTURE_NEW){
  if(p->shutdown||p->queue->closed||dvm_is_stopping(v)){f->status=FUTURE_CANCELLED;drop=true;}
  else{
   uint64_t period=f->period_ms;uint64_t base=f->periodic==PERIODIC_FIXED_DELAY?completed_ms:f->due_ms;
   f->due_ms=period>UINT64_MAX-base?UINT64_MAX:base+period;requeue=true;
  }
 }
 if(drop)pthread_cond_broadcast(&f->changed);
 pthread_mutex_unlock(&f->lock);
 if(requeue){
  enum put_result r=queue_put_locked(p->queue,task,false);
  if(r!=PUT_OK){
   pthread_mutex_lock(&f->lock);if(f->status==FUTURE_NEW)f->status=FUTURE_CANCELLED;pthread_cond_broadcast(&f->changed);pthread_mutex_unlock(&f->lock);
   sdk_trace_fprintf(stderr,"SCHEDULED_REQUEUE_FAIL reason=%s\n",r==PUT_OOM?"out_of_memory":"closed_or_full");
  }
 }
}
static bool executor_worker_run(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 (void)a;(void)n;(void)out;struct dvm_object *o=dvm__obj(v,self);struct executor_worker *w=o?o->data:NULL;if(!w)return false;struct executor_state *p=w->pool;struct queue_state *q=p->queue;
 dvm_ref first=w->first_task;w->first_task=0;
 for(;;){dvm_ref task=first;first=0;pthread_mutex_lock(&q->lock);
  if(!task){for(;;){if(p->scheduled && q->size){
    if(q->closed||dvm_is_stopping(v))break;
    size_t best=0;uint64_t due=UINT64_MAX;
    for(size_t i=0;i<q->size;i++){dvm_ref ref=q->items[(q->head+i)%q->allocated];struct future_state *f=future_of(v,ref);if(f&&f->due_ms<due){due=f->due_ms;best=i;}}
    uint64_t now=now_ms();if(due>now){uint64_t wait=due-now;if(wait>50)wait=50;queue_wait_locked(v,q,wait);continue;}
    size_t ix=(q->head+best)%q->allocated;task=q->items[ix];for(size_t j=best;j+1<q->size;j++)q->items[(q->head+j)%q->allocated]=q->items[(q->head+j+1)%q->allocated];q->items[(q->head+q->size-1)%q->allocated]=0;--q->size;
   }else task=queue_pop_locked(q,true);if(task)break;if(p->shutdown||q->closed||dvm_is_stopping(v))break;
   if(p->keep_alive_ms&&(p->allow_core_timeout||p->live>p->core)){
    /* allowCoreThreadTimeOut(true) 后 core worker 也按 keepAlive 回收；未开启
     * 时只有超出 core 的 worker 会超时，与 AOSP 一致。Cached 池的 worker 在
     * 60s keep-alive 等待期间也是 SynchronousQueue 的
     * 消费者。必须计入 waiting_takers，否则 execute 的 offer 会误判为无人
     * 接收而落到 PUT_FULL，转而创建新 worker；AOSP 会直接交给空闲 worker。 */
    bool sync=q->kind==QUEUE_SYNC;if(sync)++q->waiting_takers;
    bool timeout=queue_wait_locked(v,q,p->keep_alive_ms);if(sync)--q->waiting_takers;
    if(timeout&&!queue_count_locked(q)&&!q->has_handoff){--p->live;w->alive=false;pthread_cond_broadcast(&q->changed);pthread_mutex_unlock(&q->lock);return true;}
   }
   else {if(q->kind==QUEUE_SYNC)++q->waiting_takers;queue_wait_locked(v,q,50);if(q->kind==QUEUE_SYNC&&q->waiting_takers)--q->waiting_takers;}
  }}
  if(!task){--p->live;w->alive=false;pthread_cond_broadcast(&q->changed);pthread_mutex_unlock(&q->lock);break;}
  ++p->active;pthread_mutex_unlock(&q->lock);
  dvm_set_step_limit(v,100000);bool ok=!dvm_is_stopping(v)&&invoke_runnable(v,task);bool failed=!ok||dvm_exception(v);
  if(failed){sdk_trace_async_failure(v,"executor");dvm_clear_exception(v);}
  uint64_t completed_ms=now_ms();
  pthread_mutex_lock(&q->lock);if(p->active)--p->active;++p->executed;
  if(p->scheduled)scheduled_requeue(v,p,task,completed_ms);
  pthread_cond_broadcast(&q->changed);pthread_mutex_unlock(&q->lock);
 }
 return true;
}
static void *executor_worker_main(void *arg){
 struct executor_worker *w=arg;struct dvm *v=w->pool->vm;
 /* A ThreadFactory such as BLogThreadFactory calls Thread.setPriority(10)
  * before start_worker binds the Thread.  Apply the stored Java priority to
  * the real worker thread here (the only thread that can set its own nice),
  * mirroring ART's EPERM-tolerant behavior: log and keep running. */
 if(w->pool->factory){
  int java_priority=0,priority_error=0,priority_observed=0;
  if(thread_runtime_executor_apply_priority(v,w->self,&java_priority,&priority_error,&priority_observed)){
   sdk_trace_fprintf(stderr,"THREAD_PRIORITY_APPLY java_priority=%d nice=%d\n",java_priority,priority_observed);
  }else if(java_priority){
   sdk_trace_fprintf(stderr,"THREAD_PRIORITY_FAIL java_priority=%d errno=%d action=continue_worker\n",java_priority,priority_error);
  }
 }
 dvm__mark_bytecode_thread();dvm_gil_acquire(v);dvm__tstate_reset(v);v->cur_thread=w->self;v->quiet_uncaught=true;
 if(w->trampoline){union dvm_value out={0};struct dvm_method *m=dvm_find_method(v,dvm_object_class(v,w->trampoline),"run","()V");if(m)dvm_call(v,m,w->trampoline,NULL,0,&out);}
 else {struct dvm_object fake={0};(void)fake; /* default worker has a trampoline too */ union dvm_value out={0};executor_worker_run(v,w->trampoline,NULL,0,&out);}
 if(w->pool->factory)thread_runtime_executor_finished(v,w->self);dvm_gil_release(v);return NULL;
}
static bool executor_ctor(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,const char *sig){
 if(n<5||n>8)return false;int core=a[0].i,maximum=a[1].i;int64_t keep=a[2].j;
 if(core<0||maximum<=0||core>maximum||keep<0||!a[3].l||!a[4].l){dvm__throw(v,"java/lang/IllegalArgumentException","invalid ThreadPoolExecutor configuration");return true;}
 dvm_ref handler=0,factory=0;
 if(n>=6){
  if(n>=7){
   if(!a[5].l||!a[6].l){dvm__throw(v,"java/lang/NullPointerException","ThreadPoolExecutor factory or rejection handler");return true;}
   factory=a[5].l;handler=a[6].l;
  }
  else if(strstr(sig,"Ljava/util/concurrent/ThreadFactory;"))factory=a[5].l;
  else if(strstr(sig,"Ljava/util/concurrent/RejectedExecutionHandler;"))handler=a[5].l;
  else return false;
 }
 /* DefaultThreadFactory 正规化回退：normal 情况下 default_thread_factory()
  * 已在该类上安装真实 newThread，所以这里的条件不成立，factory 保持原样并
  * 走 start_worker 的真实工厂路径。只有类上拿不到可用 newThread（例如类已
  * 被其它模块占用、无法安全追加方法）时，才把 factory 置 0，让本运行时用
  * 自己等价的默认 worker 线程创建路径；否则 execute 时会抛 NoSuchMethodError。 */
 if(factory&&is_default_thread_factory(v,factory)&&
    !dvm_find_method(v,dvm_object_class(v,factory),"newThread","(Ljava/lang/Runnable;)Ljava/lang/Thread;")){
  sdk_trace_fprintf(stderr,"EXECUTORS_CACHED default_factory=new_thread_missing action=fallback_internal_worker\n");
  factory=0;
 }
 struct queue_state *q=queue_of(v,a[4].l);if(!q){dvm__throw(v,"java/lang/UnsupportedOperationException","ThreadPoolExecutor requires a supported queue object");return true;}
 uint64_t nanos=0;if(!unit_millis(v,a[3].l,&nanos))return true;if((uint64_t)keep>UINT64_MAX/nanos){dvm__throw(v,"java/lang/IllegalArgumentException","keep-alive overflow");return true;}
 struct dvm_object *o=dvm__obj(v,self);if(!o){dvm__throw(v,"java/lang/NullPointerException","ThreadPoolExecutor receiver");return true;}if(o->data){dvm__throw(v,"java/lang/IllegalStateException","ThreadPoolExecutor already initialized");return true;}
 struct executor_state *p=calloc(1,sizeof *p);if(!p){dvm__throw(v,"java/lang/OutOfMemoryError","ThreadPoolExecutor state");return true;}
 p->vm=v;p->self=self;p->handler=handler;p->factory=factory;p->queue_ref=a[4].l;p->queue=q;p->core=(unsigned)core;p->maximum=(unsigned)maximum;
 /* Java keeps sub-millisecond keep-alive times positive; round up instead of
  * truncating 1 ns to "no timeout".  allowCoreThreadTimeOut(true) relies on
  * the original keepAliveTime being > 0, so keep_alive_ms is our truthful
  * positive-time indicator as well. */
 uint64_t keep_ms=((uint64_t)keep*nanos)/1000000u;if(keep>0&&keep_ms==0)keep_ms=1;p->keep_alive_ms=keep_ms;
 p->next=all_executors;all_executors=p;o->data=p;o->data_destroy=executor_destroy;return true;
}
static bool executor_reject(struct dvm *v,struct executor_state *p,dvm_ref task,const char *reason){
 if(!p->handler){dvm__throw(v,"java/util/concurrent/RejectedExecutionException","%s",reason);return true;}
 union dvm_value args[2]={{.l=task},{.l=p->self}},out={0};
 /* The three AOSP policy classes referenced by the DEX have no guest
  * methods installed; their real behavior lives here by exact class name.
  * This keeps DiscardPolicy a true no-op instead of MISSING_API or an
  * accidental RejectedExecutionException. */
 if(rejection_policy_dispatch(v,p->handler,args,2,&out))return true;
 struct dvm_object *o=dvm__obj(v,p->handler);
 struct dvm_method *m=o?dvm_find_method(v,o->cls,"rejectedExecution","(Ljava/lang/Runnable;Ljava/util/concurrent/ThreadPoolExecutor;)V"):NULL;
 if(!m){dvm__throw(v,"java/lang/NoSuchMethodError","RejectedExecutionHandler.rejectedExecution");return true;}
 dvm_call(v,m,p->handler,args,2,&out);return true;
}
static bool executor_execute(struct dvm *v,struct executor_state *p,dvm_ref task){
 if(!task){dvm__throw(v,"java/lang/NullPointerException","Executor task");return true;}
 pthread_mutex_lock(&p->queue->lock);if(p->shutdown){pthread_mutex_unlock(&p->queue->lock);return executor_reject(v,p,task,"executor is shut down");}
 bool needs_worker=p->live<p->core||p->live==0;pthread_mutex_unlock(&p->queue->lock);
 if(needs_worker){if(start_worker(p,task,true))return true;if(dvm_exception(v))return true;}
 pthread_mutex_lock(&p->queue->lock);if(p->shutdown){pthread_mutex_unlock(&p->queue->lock);return executor_reject(v,p,task,"executor is shut down");}
 enum put_result r=queue_put_locked(p->queue,task,false);if(r==PUT_OK){pthread_cond_broadcast(&p->queue->changed);pthread_mutex_unlock(&p->queue->lock);return true;}
 bool can_expand=p->live<p->maximum;pthread_mutex_unlock(&p->queue->lock);
 if(r==PUT_OOM){dvm__throw(v,"java/lang/OutOfMemoryError","executor queue storage");return true;}
 if(can_expand&&start_worker(p,task,false))return true;if(dvm_exception(v))return true;
 return executor_reject(v,p,task,"executor queue full");
}
static bool future_ctor(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,const char *sig){
 struct dvm_object *o=dvm__obj(v,self);if(!o){dvm__throw(v,"java/lang/NullPointerException","FutureTask receiver");return true;}if(o->data){dvm__throw(v,"java/lang/IllegalStateException","FutureTask already initialized");return true;}
 struct future_state *f=calloc(1,sizeof *f);if(!f){dvm__throw(v,"java/lang/OutOfMemoryError","FutureTask state");return true;}
 if(pthread_mutex_init(&f->lock,NULL)){free(f);dvm__throw(v,"java/lang/OutOfMemoryError","FutureTask mutex");return true;}
 if(pthread_cond_init(&f->changed,NULL)){pthread_mutex_destroy(&f->lock);free(f);dvm__throw(v,"java/lang/OutOfMemoryError","FutureTask condition");return true;}
 if(!strcmp(sig,"(Ljava/util/concurrent/Callable;)V")){if(n!=1||!a[0].l){future_destroy(f);dvm__throw(v,"java/lang/NullPointerException","FutureTask callable");return true;}f->callable=a[0].l;}
 else if(!strcmp(sig,"(Ljava/lang/Runnable;Ljava/lang/Object;)V")){if(n!=2||!a[0].l){future_destroy(f);dvm__throw(v,"java/lang/NullPointerException","FutureTask runnable");return true;}f->runnable=a[0].l;f->result=a[1].l;}
 else {future_destroy(f);return false;}
 f->status=FUTURE_NEW;o->data=f;o->data_destroy=future_destroy;return true;
}
static bool future_run(struct dvm *v,dvm_ref self){
 struct future_state *f=future_of(v,self);if(!f){dvm__throw(v,"java/lang/IllegalStateException","FutureTask constructor not called");return true;}
 pthread_mutex_lock(&f->lock);if(f->status!=FUTURE_NEW){pthread_mutex_unlock(&f->lock);return true;}f->status=FUTURE_RUNNING;dvm_ref callable=f->callable,runnable=f->runnable;pthread_mutex_unlock(&f->lock);
 dvm_ref result=f->result;bool success=true;
 if(callable){struct dvm_object *o=dvm__obj(v,callable);struct dvm_method *m=o?dvm_find_method(v,o->cls,"call","()Ljava/lang/Object;"):NULL;if(!m){sdk_trace_missing_api(o&&o->cls?o->cls->name:"?","call","()Ljava/lang/Object;");dvm__throw(v,"java/lang/NoSuchMethodError","Callable.call");success=false;}else{union dvm_value out={0};success=dvm_call(v,m,callable,NULL,0,&out);result=out.l;}}
 else if(runnable)success=invoke_runnable(v,runnable);
 dvm_ref exception=dvm_exception(v);if(exception){dvm_clear_exception(v);success=false;}
 pthread_mutex_lock(&f->lock);f->result=result;f->exception=exception;
 /* A successful periodic run is not completion: ScheduledFuture.get() keeps
  * blocking and isDone() stays false until cancel or a failing run.  The
  * worker re-arms the future from this NEW state after the call returns. */
 f->status=(success&&f->periodic!=PERIODIC_NONE)?FUTURE_NEW:(success?FUTURE_DONE:FUTURE_FAILED);
 pthread_cond_broadcast(&f->changed);pthread_mutex_unlock(&f->lock);return true;
}
static bool future_wait(struct dvm *v,struct future_state *f,bool timed,uint64_t timeout_ms,union dvm_value *out){
 uint64_t end=now_ms()+timeout_ms;pthread_mutex_lock(&f->lock);
 while(f->status==FUTURE_NEW||f->status==FUTURE_RUNNING){if(dvm_is_stopping(v)){pthread_mutex_unlock(&f->lock);dvm__throw(v,"java/lang/InterruptedException","FutureTask wait interrupted during shutdown");return true;}if(timed&&now_ms()>=end){pthread_mutex_unlock(&f->lock);dvm__throw(v,"java/util/concurrent/TimeoutException","FutureTask timed out");return true;}uint64_t wait=timed?end-now_ms():50;if(wait>50)wait=50;unsigned depth=dvm_gil_unlock_all(v);struct timespec until=deadline_ms(wait);pthread_cond_timedwait(&f->changed,&f->lock,&until);pthread_mutex_unlock(&f->lock);dvm_gil_relock(v,depth);pthread_mutex_lock(&f->lock);}
 int status=f->status;dvm_ref result=f->result,exception=f->exception;pthread_mutex_unlock(&f->lock);
 if(status==FUTURE_CANCELLED)dvm__throw(v,"java/util/concurrent/CancellationException","FutureTask cancelled");else if(status==FUTURE_FAILED)dvm__throw(v,"java/util/concurrent/ExecutionException","Callable.call failed with exception @%u",exception);else out->l=result;return true;
}
static bool future_call(struct dvm *v,const char *m,const char *sig,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 bool ctor=!strcmp(m,"<init>");if(ctor)return future_ctor(v,self,a,n,sig);
 struct future_state *f=future_of(v,self);if(!f){dvm__throw(v,"java/lang/IllegalStateException","FutureTask constructor not called");return true;}
 if(!strcmp(m,"run")&&!strcmp(sig,"()V"))return future_run(v,self);
 if(!strcmp(m,"isDone")&&!strcmp(sig,"()Z")){pthread_mutex_lock(&f->lock);out->i=f->status>=FUTURE_DONE;pthread_mutex_unlock(&f->lock);return true;}
 if(!strcmp(m,"isCancelled")&&!strcmp(sig,"()Z")){pthread_mutex_lock(&f->lock);out->i=f->status==FUTURE_CANCELLED;pthread_mutex_unlock(&f->lock);return true;}
 if(!strcmp(m,"cancel")&&!strcmp(sig,"(Z)Z")){
  out->i=0;
  pthread_mutex_lock(&f->lock);if(f->status==FUTURE_NEW){f->status=FUTURE_CANCELLED;pthread_cond_broadcast(&f->changed);out->i=1;}else if(f->status==FUTURE_RUNNING&&a[0].i){pthread_mutex_unlock(&f->lock);dvm__throw(v,"java/lang/UnsupportedOperationException","interrupting a running FutureTask is not supported");return true;}pthread_mutex_unlock(&f->lock);if(out->i&&f->scheduled_queue){union dvm_value ignored={0};queue_contains(v,f->scheduled_queue,self,true,false,&ignored);}return true;
 }
 if(!strcmp(m,"getDelay")&&!strcmp(sig,"(Ljava/util/concurrent/TimeUnit;)J")){
  /* ScheduledFuture.getDelay(unit) is TimeUnit.convert(remainingNanos,
   * NANOSECONDS); truncation is toward zero, matching Java for overdue tasks.
   * Only futures that went through schedule()/scheduleAtFixedRate() carry a
   * deadline; a bare FutureTask cast to ScheduledFuture fails closed. */
  if(!f->scheduled_queue)return false;
  uint64_t scale=0;if(!unit_millis(v,a[0].l,&scale))return true;
  pthread_mutex_lock(&f->lock);uint64_t due=f->due_ms;pthread_mutex_unlock(&f->lock);
  __int128 nanos=((__int128)due-(__int128)now_ms())*1000000;
  __int128 units=nanos/(__int128)scale;
  if(units>INT64_MAX)units=INT64_MAX;if(units<INT64_MIN)units=INT64_MIN;
  out->j=(int64_t)units;return true;
 }
 if(!strcmp(m,"get")&&!strcmp(sig,"()Ljava/lang/Object;"))return future_wait(v,f,false,0,out);
 if(!strcmp(m,"get")&&!strcmp(sig,"(JLjava/util/concurrent/TimeUnit;)Ljava/lang/Object;")){
  uint64_t nanos=0;if(a[0].j<0){dvm__throw(v,"java/lang/IllegalArgumentException","negative timeout");return true;}if(!unit_millis(v,a[1].l,&nanos))return true;uint64_t ms=((uint64_t)a[0].j*nanos+999999u)/1000000u;return future_wait(v,f,true,ms,out);
 }
 if(!strcmp(m,"runAndReset")&&!strcmp(sig,"()Z")){dvm__throw(v,"java/lang/UnsupportedOperationException","FutureTask.runAndReset is not implemented");return true;}
 return false;
}

static bool queue_call(struct dvm *v,const char *c,const char *m,const char *sig,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 struct dvm_object *o=dvm__obj(v,self);int kind=(is_name(o,"java/util/concurrent/LinkedBlockingDeque")||is_name(o,"java/util/concurrent/LinkedBlockingQueue")||is_name(o,"java/util/concurrent/ArrayBlockingQueue"))?QUEUE_LINKED:is_name(o,"java/util/concurrent/SynchronousQueue")?QUEUE_SYNC:0;if(!kind)return false;
 if(!strcmp(m,"<init>")){
  size_t max=INT_MAX;bool linked_queue=!strcmp(c,"java/util/concurrent/LinkedBlockingQueue");bool array_queue=!strcmp(c,"java/util/concurrent/ArrayBlockingQueue");
  if((!strcmp(c,"java/util/concurrent/LinkedBlockingDeque")||linked_queue||array_queue)&&!strcmp(sig,"(I)V")){if(n!=1||a[0].i<=0){dvm__throw(v,"java/lang/IllegalArgumentException","blocking queue capacity must be positive");return true;}max=(size_t)a[0].i;}
  else if((!strcmp(c,"java/util/concurrent/LinkedBlockingDeque")||linked_queue)&&!strcmp(sig,"()V")){if(n)return false;}
  else if(kind==QUEUE_SYNC&&!strcmp(sig,"()V")){if(n)return false;}
  else if(kind==QUEUE_SYNC&&!strcmp(sig,"(Z)V")){if(n!=1)return false;}
  else return false;
  if(!new_queue(v,self,kind,max)&&!dvm_exception(v))dvm__throw(v,"java/lang/OutOfMemoryError","queue state");return true;
 }
 struct queue_state *q=queue_of(v,self);if(!q){dvm__throw(v,"java/lang/IllegalStateException","queue constructor not called");return true;}
 bool lb=kind==QUEUE_LINKED;bool first=!strcmp(m,"offerFirst")||!strcmp(m,"addFirst")||!strcmp(m,"putFirst")||!strcmp(m,"takeFirst")||!strcmp(m,"pollFirst")||!strcmp(m,"peekFirst")||!strcmp(m,"getFirst")||!strcmp(m,"removeFirst")||!strcmp(m,"push")||!strcmp(m,"removeFirstOccurrence");
 bool last=!first;
 if(lb&&(!strcmp(m,"offer")&&!strcmp(sig,"(Ljava/lang/Object;)Z")))return queue_put(v,q,a[0].l,false,false,0,false,out);
 if(lb&&(!strcmp(m,"add")&&!strcmp(sig,"(Ljava/lang/Object;)Z"))){queue_put(v,q,a[0].l,false,false,0,false,out);if(!dvm_exception(v)&&!out->i)dvm__throw(v,"java/lang/IllegalStateException","deque full");return true;}
 if(lb&&(!strcmp(m,"put")&&!strcmp(sig,"(Ljava/lang/Object;)V")))return queue_put(v,q,a[0].l,false,true,0,false,out);
 if(lb&&(!strcmp(m,"offerFirst")||!strcmp(m,"offerLast"))&&!strcmp(sig,"(Ljava/lang/Object;)Z"))return queue_put(v,q,a[0].l,first,false,0,false,out);
 if(lb&&(!strcmp(m,"addFirst")||!strcmp(m,"addLast"))&&!strcmp(sig,"(Ljava/lang/Object;)V")){queue_put(v,q,a[0].l,first,false,0,false,out);if(!dvm_exception(v)&& !out->i)dvm__throw(v,"java/lang/IllegalStateException","deque full");return true;}
 if(lb&&(!strcmp(m,"putFirst")||!strcmp(m,"putLast"))&&!strcmp(sig,"(Ljava/lang/Object;)V"))return queue_put(v,q,a[0].l,first,true,0,false,out);
 if((lb||kind==QUEUE_SYNC)&&!strcmp(m,"offer")&&!strcmp(sig,"(Ljava/lang/Object;)Z"))return queue_put(v,q,a[0].l,false,false,0,false,out);
 if((lb||kind==QUEUE_SYNC)&&!strcmp(m,"put")&&!strcmp(sig,"(Ljava/lang/Object;)V"))return queue_put(v,q,a[0].l,false,true,0,false,out);
 if(lb&&(!strcmp(m,"offer")&&!strcmp(sig,"(Ljava/lang/Object;JLjava/util/concurrent/TimeUnit;)Z"))){if(a[1].j<0){dvm__throw(v,"java/lang/IllegalArgumentException","negative timeout");return true;}uint64_t unit=0;if(!unit_millis(v,a[2].l,&unit))return true;uint64_t ms=((uint64_t)a[1].j*unit+999999u)/1000000u;return queue_put(v,q,a[0].l,false,true,ms,true,out);}
 if(lb&&(!strcmp(m,"offerFirst")||!strcmp(m,"offerLast"))&&!strcmp(sig,"(Ljava/lang/Object;JLjava/util/concurrent/TimeUnit;)Z")){if(a[1].j<0){dvm__throw(v,"java/lang/IllegalArgumentException","negative timeout");return true;}uint64_t unit=0;if(!unit_millis(v,a[2].l,&unit))return true;uint64_t ms=((uint64_t)a[1].j*unit+999999u)/1000000u;return queue_put(v,q,a[0].l,first,true,ms,true,out);}
 if(lb&&(!strcmp(m,"putFirst")||!strcmp(m,"putLast"))&&!strcmp(sig,"(Ljava/lang/Object;)V"))return queue_put(v,q,a[0].l,first,true,0,false,out);
 bool take=!strcmp(m,"take")||!strcmp(m,"takeFirst")||!strcmp(m,"takeLast")||!strcmp(m,"pop")||!strcmp(m,"removeFirst")||!strcmp(m,"removeLast")||!strcmp(m,"getFirst")||!strcmp(m,"getLast")||!strcmp(m,"element")||!strcmp(m,"remove");
 bool peek=!strcmp(m,"peek")||!strcmp(m,"peekFirst")||!strcmp(m,"peekLast");bool poll=!strcmp(m,"poll")||!strcmp(m,"pollFirst")||!strcmp(m,"pollLast");
 if((lb||kind==QUEUE_SYNC)&&!strcmp(m,"take")&&!strcmp(sig,"()Ljava/lang/Object;")){bool interrupted=false;out->l=queue_take(v,q,false,true,0,false,NULL,&interrupted);if(interrupted)dvm__throw(v,"java/lang/InterruptedException","queue wait interrupted during VM shutdown");return true;}
 if(lb&&take&&(!strcmp(sig,"()Ljava/lang/Object;"))){bool interrupted=false;out->l=queue_take(v,q,first,false,0,false,NULL,&interrupted);if(!out->l&&!interrupted)dvm__throw(v,"java/util/NoSuchElementException","deque empty");return true;}
 if((lb||kind==QUEUE_SYNC)&&poll&&!strcmp(sig,"()Ljava/lang/Object;")){out->l=queue_take(v,q,first,false,0,false,NULL,NULL);return true;}
 if((lb||kind==QUEUE_SYNC)&&peek&&!strcmp(sig,"()Ljava/lang/Object;")){
  pthread_mutex_lock(&q->lock);if(q->kind==QUEUE_SYNC)out->l=0;else if(q->size)out->l=q->items[(q->head+(first?0:q->size-1))%q->allocated];pthread_mutex_unlock(&q->lock);return true;
 }
 if(lb&&(!strcmp(m,"poll")&&!strcmp(sig,"(JLjava/util/concurrent/TimeUnit;)Ljava/lang/Object;"))){if(a[0].j<0){dvm__throw(v,"java/lang/IllegalArgumentException","negative timeout");return true;}uint64_t unit=0;if(!unit_millis(v,a[1].l,&unit))return true;uint64_t ms=((uint64_t)a[0].j*unit+999999u)/1000000u;bool timeout=false,interrupted=false;out->l=queue_take(v,q,false,true,ms,true,&timeout,&interrupted);if(interrupted)dvm__throw(v,"java/lang/InterruptedException","queue wait interrupted during VM shutdown");return true;}
 if(lb&&(!strcmp(m,"pollFirst")||!strcmp(m,"pollLast"))&&!strcmp(sig,"(JLjava/util/concurrent/TimeUnit;)Ljava/lang/Object;")){if(a[0].j<0){dvm__throw(v,"java/lang/IllegalArgumentException","negative timeout");return true;}uint64_t unit=0;if(!unit_millis(v,a[1].l,&unit))return true;uint64_t ms=((uint64_t)a[0].j*unit+999999u)/1000000u;bool timeout=false,interrupted=false;out->l=queue_take(v,q,first,true,ms,true,&timeout,&interrupted);if(interrupted)dvm__throw(v,"java/lang/InterruptedException","queue wait interrupted during VM shutdown");return true;}
 if((lb||kind==QUEUE_SYNC)&&(!strcmp(m,"size")&&!strcmp(sig,"()I"))){pthread_mutex_lock(&q->lock);out->i=(int)queue_count_locked(q);pthread_mutex_unlock(&q->lock);return true;}
 if((lb||kind==QUEUE_SYNC)&&(!strcmp(m,"isEmpty")&&!strcmp(sig,"()Z"))){pthread_mutex_lock(&q->lock);out->i=queue_count_locked(q)==0;pthread_mutex_unlock(&q->lock);return true;}
 if((lb||kind==QUEUE_SYNC)&&(!strcmp(m,"remainingCapacity")&&!strcmp(sig,"()I"))){pthread_mutex_lock(&q->lock);out->i=q->kind==QUEUE_SYNC?0:(int)(q->maximum-q->size);pthread_mutex_unlock(&q->lock);return true;}
 if(lb&&!strcmp(m,"clear")&&!strcmp(sig,"()V")){pthread_mutex_lock(&q->lock);memset(q->items,0,q->allocated*sizeof *q->items);q->head=q->size=0;pthread_cond_broadcast(&q->changed);pthread_mutex_unlock(&q->lock);return true;}
 if(lb&&(!strcmp(m,"contains")&&!strcmp(sig,"(Ljava/lang/Object;)Z")))return queue_contains(v,q,a[0].l,false,false,out);
 if(lb&&(!strcmp(m,"remove")&&!strcmp(sig,"(Ljava/lang/Object;)Z")))return queue_contains(v,q,a[0].l,true,false,out);
 if(lb&&(!strcmp(m,"removeFirstOccurrence")||!strcmp(m,"removeLastOccurrence"))&&!strcmp(sig,"(Ljava/lang/Object;)Z"))return queue_contains(v,q,a[0].l,true,!strcmp(m,"removeLastOccurrence"),out);
 if(lb&&(!strcmp(m,"contains")&&!strcmp(sig,"(Ljava/lang/Object;)Z"))&&kind==QUEUE_SYNC){out->i=0;return true;}
 return false;
}

static bool scheduled_create(struct dvm *v,const union dvm_value *a,int n,union dvm_value *out){
 /* AOSP ScheduledThreadPoolExecutor allows corePoolSize == 0 (only negatives
  * are rejected), and uses Integer.MAX_VALUE as maximumPoolSize together with
  * DEFAULT_KEEPALIVE_MILLIS (10 ms).  The zero-core case is reachable from
  * Executors.newScheduledThreadPool(0); the worker-start rule below still
  * pre-starts one worker for the first queued task. */
 if((n!=1&&n!=2)||a[0].i<0){dvm__throw(v,"java/lang/IllegalArgumentException","invalid scheduled pool size");return true;}
 if(n==2&&!a[1].l){dvm__throw(v,"java/lang/NullPointerException","scheduled ThreadFactory");return true;}
 struct dvm_class *pc=dvm_find_class_or_external(v,"java/util/concurrent/ScheduledThreadPoolExecutor"),*qc=dvm_find_class_or_external(v,"java/util/concurrent/LinkedBlockingQueue");
 dvm_ref pr=pc?dvm_new_object(v,pc):0,qr=qc?dvm_new_object(v,qc):0;struct queue_state *q=qr?new_queue(v,qr,QUEUE_LINKED,QUEUE_LIMIT):NULL;struct dvm_object *o=dvm__obj(v,pr);
 if(!q||!o){if(!dvm_exception(v))dvm__throw(v,"java/lang/OutOfMemoryError","scheduled executor");return true;}
 struct executor_state *p=calloc(1,sizeof *p);if(!p){dvm__throw(v,"java/lang/OutOfMemoryError","scheduled executor");return true;}
 p->vm=v;p->self=pr;p->queue=q;p->queue_ref=qr;p->core=(unsigned)a[0].i;p->maximum=INT_MAX;p->keep_alive_ms=10;p->scheduled=true;p->factory=n==2?a[1].l:0;p->next=all_executors;all_executors=p;o->data=p;o->data_destroy=executor_destroy;out->l=pr;return true;
}
/* Shared schedule() path for Runnable and Callable.  The FutureTask is the
 * real returned ScheduledFuture; its future_state carries the deadline and,
 * for periodic schedules, the re-execution policy consumed by the worker.
 * initial_ms is already clamped to >= 0 by the caller (AOSP treats a negative
 * initial delay as "now"). */
static bool scheduled_enqueue(struct dvm *v,struct executor_state *p,dvm_ref task,bool callable,
                              int periodic,uint64_t initial_ms,uint64_t period_ms,union dvm_value *out){
 struct dvm_class *fc=dvm_find_class_or_external(v,"java/util/concurrent/FutureTask");dvm_ref fr=fc?dvm_new_object(v,fc):0;
 if(!fr){dvm__throw(v,"java/lang/OutOfMemoryError","scheduled future");return true;}
 union dvm_value fa[2]={{.l=task},{.l=0}};
 bool constructed=callable?future_ctor(v,fr,fa,1,"(Ljava/util/concurrent/Callable;)V")
                          :future_ctor(v,fr,fa,2,"(Ljava/lang/Runnable;Ljava/lang/Object;)V");
 if(!constructed){if(!dvm_exception(v))dvm__throw(v,"java/lang/NoSuchMethodError","FutureTask constructor");return true;}
 if(dvm_exception(v))return true;
 struct future_state *f=future_of(v,fr);if(!f){dvm__throw(v,"java/lang/IllegalStateException","scheduled FutureTask state");return true;}
 f->scheduled_queue=p->queue;f->periodic=periodic;f->period_ms=period_ms;f->due_ms=deadline_after(initial_ms);
 pthread_mutex_lock(&p->queue->lock);
 if(p->shutdown){pthread_mutex_unlock(&p->queue->lock);return executor_reject(v,p,fr,"executor is shut down");}
 enum put_result r=queue_put_locked(p->queue,fr,false);
 /* AOSP ensurePrestart: a scheduled pool with corePoolSize == 0 still starts
  * one worker for the first task. */
 bool need=p->live<p->core||p->live==0;
 pthread_mutex_unlock(&p->queue->lock);
 if(r!=PUT_OK){dvm__throw(v,"java/util/concurrent/RejectedExecutionException","scheduled queue %s",r==PUT_OOM?"out of memory":"full");return true;}
 if(need&&!start_worker(p,0,true)){
  pthread_mutex_lock(&p->queue->lock);bool no_workers=p->live==0;pthread_mutex_unlock(&p->queue->lock);
  if(no_workers){union dvm_value ignored={0};queue_contains(v,p->queue,fr,true,false,&ignored);if(!dvm_exception(v))dvm__throw(v,"java/util/concurrent/RejectedExecutionException","ThreadFactory failed to create worker");return true;}
  if(dvm_exception(v))return true;
 }
 out->l=fr;return true;
}
static bool scheduled_submit(struct dvm *v,struct executor_state *p,const union dvm_value *a,int n,union dvm_value *out){
 if(n!=3)return false;if(!a[0].l){dvm__throw(v,"java/lang/NullPointerException","scheduled task");return true;}
 uint64_t scale=0;if(!unit_millis(v,a[2].l,&scale))return true;
 uint64_t delay=a[1].j>0?duration_ms((uint64_t)a[1].j,scale):0;
 return scheduled_enqueue(v,p,a[0].l,false,PERIODIC_NONE,delay,0,out);
}
static bool scheduled_submit_callable(struct dvm *v,struct executor_state *p,const union dvm_value *a,int n,union dvm_value *out){
 if(n!=3)return false;if(!a[0].l){dvm__throw(v,"java/lang/NullPointerException","scheduled callable");return true;}
 uint64_t scale=0;if(!unit_millis(v,a[2].l,&scale))return true;
 uint64_t delay=a[1].j>0?duration_ms((uint64_t)a[1].j,scale):0;
 return scheduled_enqueue(v,p,a[0].l,true,PERIODIC_NONE,delay,0,out);
}
/* scheduleAtFixedRate / scheduleWithFixedDelay share argument checking:
 * null task or unit -> NPE; period <= 0 -> IllegalArgumentException (AOSP
 * rejects zero exactly like a negative period); negative initialDelay is 0.
 * Nanosecond periods round up so a positive period never turns into 0 ms. */
static bool scheduled_periodic(struct dvm *v,struct executor_state *p,const union dvm_value *a,
                               int n,int periodic,union dvm_value *out){
 if(n!=4)return false;if(!a[0].l){dvm__throw(v,"java/lang/NullPointerException","scheduled task");return true;}
 uint64_t scale=0;if(!unit_millis(v,a[3].l,&scale))return true;
 if(a[2].j<=0){dvm__throw(v,"java/lang/IllegalArgumentException","period must be positive");return true;}
 uint64_t period=duration_ms((uint64_t)a[2].j,scale);if(period==0)period=1;
 uint64_t initial=a[1].j>0?duration_ms((uint64_t)a[1].j,scale):0;
 return scheduled_enqueue(v,p,a[0].l,false,periodic,initial,period,out);
}
/* Direct ScheduledThreadPoolExecutor constructors.  AOSP's four public forms
 * forward to ThreadPoolExecutor(corePoolSize, Integer.MAX_VALUE, 10 ms,
 * MILLISECONDS, DelayedWorkQueue, factory, handler); this runtime keeps a real
 * LinkedBlockingQueue and its deadline-ordered scheduled worker instead of
 * DelayedWorkQueue, which is not observable through the methods the APK uses.
 * Only the (int, ThreadFactory) and (int, ThreadFactory, handler) forms are
 * referenced by the DEX, but all four are accepted so a missing overload is
 * never silently reported as success. */
static bool scheduled_ctor(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,const char *sig){
 if(n<1||n>3)return false;int core=a[0].i;
 if(core<0){dvm__throw(v,"java/lang/IllegalArgumentException","corePoolSize < 0");return true;}
 dvm_ref factory=0,handler=0;
 if(n==2){
  if(strstr(sig,"Ljava/util/concurrent/ThreadFactory;")){factory=a[1].l;if(!factory){dvm__throw(v,"java/lang/NullPointerException","ThreadFactory");return true;}}
  else if(strstr(sig,"Ljava/util/concurrent/RejectedExecutionHandler;")){handler=a[1].l;if(!handler){dvm__throw(v,"java/lang/NullPointerException","RejectedExecutionHandler");return true;}}
  else return false;
 }else if(n==3){
  if(strcmp(sig,"(ILjava/util/concurrent/ThreadFactory;Ljava/util/concurrent/RejectedExecutionHandler;)V"))return false;
  factory=a[1].l;handler=a[2].l;
  if(!factory||!handler){dvm__throw(v,"java/lang/NullPointerException","ThreadFactory or RejectedExecutionHandler");return true;}
 }else if(strcmp(sig,"(I)V"))return false;
 struct dvm_object *o=dvm__obj(v,self);if(!o){dvm__throw(v,"java/lang/NullPointerException","ScheduledThreadPoolExecutor receiver");return true;}
 if(o->data){dvm__throw(v,"java/lang/IllegalStateException","ScheduledThreadPoolExecutor already initialized");return true;}
 struct dvm_class *qc=dvm_find_class_or_external(v,"java/util/concurrent/LinkedBlockingQueue");
 dvm_ref qr=qc?dvm_new_object(v,qc):0;struct queue_state *q=qr?new_queue(v,qr,QUEUE_LINKED,QUEUE_LIMIT):NULL;
 if(!q){if(!dvm_exception(v))dvm__throw(v,"java/lang/OutOfMemoryError","ScheduledThreadPoolExecutor queue");return true;}
 struct executor_state *p=calloc(1,sizeof *p);if(!p){dvm__throw(v,"java/lang/OutOfMemoryError","ScheduledThreadPoolExecutor state");return true;}
 p->vm=v;p->self=self;p->queue=q;p->queue_ref=qr;p->core=(unsigned)core;p->maximum=INT_MAX;
 p->keep_alive_ms=10; /* AOSP DEFAULT_KEEPALIVE_MILLIS */p->scheduled=true;p->factory=factory;p->handler=handler;
 p->next=all_executors;all_executors=p;o->data=p;o->data_destroy=executor_destroy;return true;
}
/* Executors.newCachedThreadPool: AOSP 的缓存池参数是 corePoolSize=0、
 * maximumPoolSize=Integer.MAX_VALUE、keepAliveTime=60 SECONDS、
 * SynchronousQueue、给定的 ThreadFactory。这里创建的是真实的
 * java/util/concurrent/ThreadPoolExecutor 对象，并走本文件已有的六参数
 * <init> 路径，因此 start_worker/execute/shutdown 等行为与其它线程池一致。
 * SynchronousQueue 必须先真实执行 <init>()V，executor_ctor 的 queue_of()
 * 才能从对象数据里识别出 QUEUE_SYNC；否则构造会抛 UnsupportedOperationException。
 * 失败一律抛真实异常（OOM/NPE/IllegalArgumentException/NoSuchMethodError），
 * 不返回 null 冒充成功。 */
static bool cached_create(struct dvm *v,bool with_factory,const union dvm_value *a,int n,union dvm_value *out){
 if(n!=(with_factory?1:0)){dvm__throw(v,"java/lang/IllegalArgumentException","newCachedThreadPool arity %d",n);return true;}
 /* AOSP 的无参重载等价于 newCachedThreadPool(defaultThreadFactory())：
  * 这里同样取本 VM 缓存的规范默认工厂对象，而不是传 null 绕过工厂路径。 */
 dvm_ref factory=0;
 if(with_factory){factory=a[0].l;if(!factory){dvm__throw(v,"java/lang/NullPointerException","newCachedThreadPool ThreadFactory");return true;}}
 else {factory=default_thread_factory(v);if(!factory){if(!dvm_exception(v))dvm__throw(v,"java/lang/OutOfMemoryError","newCachedThreadPool default ThreadFactory");return true;}}
 struct dvm_class *pc=dvm_find_class_or_external(v,"java/util/concurrent/ThreadPoolExecutor");
 struct dvm_class *qc=dvm_find_class_or_external(v,"java/util/concurrent/SynchronousQueue");
 dvm_ref pool=pc?dvm_new_object(v,pc):0,queue=qc?dvm_new_object(v,qc):0;
 if(!pool||!queue){dvm__throw(v,"java/lang/OutOfMemoryError","newCachedThreadPool");return true;}
 union dvm_value ignored={0};
 if(!executor_runtime_call(v,"java/util/concurrent/SynchronousQueue","<init>","()V",queue,NULL,0,&ignored)){
  if(!dvm_exception(v))dvm__throw(v,"java/lang/NoSuchMethodError","java/util/concurrent/SynchronousQueue.<init>()V");
  return true;
 }
 union dvm_value unit={0};
 if(!java_basics_static(v,"java/util/concurrent/TimeUnit","SECONDS","Ljava/util/concurrent/TimeUnit;",&unit)||!unit.l){
  if(!dvm_exception(v))dvm__throw(v,"java/lang/IllegalStateException","java/util/concurrent/TimeUnit.SECONDS");
  return true;
 }
 union dvm_value args[6]={{.i=0},{.i=INT_MAX},{.j=60},{.l=unit.l},{.l=queue},{.l=factory}};
 if(!executor_runtime_call(v,"java/util/concurrent/ThreadPoolExecutor","<init>",
    "(IIJLjava/util/concurrent/TimeUnit;Ljava/util/concurrent/BlockingQueue;Ljava/util/concurrent/ThreadFactory;)V",
    pool,args,6,&ignored)){
  if(!dvm_exception(v))dvm__throw(v,"java/lang/NoSuchMethodError","java/util/concurrent/ThreadPoolExecutor.<init>");
  return true;
 }
 sdk_trace_fprintf(stderr,"EXECUTORS_CACHED pool=created core=0 maximum=%d keepalive_ms=60000 factory=%s\n",INT_MAX,with_factory?"explicit":"default");
 out->l=pool;return true;
}
/* Executors.newFixedThreadPool 的两个重载，严格按 AOSP 参数构造：
 *   new ThreadPoolExecutor(nThreads, nThreads, 0L, TimeUnit.MILLISECONDS,
 *                          new LinkedBlockingQueue<Runnable>(), threadFactory)
 * 因此 core=max=nThreads、keepAliveTime=0、队列是无界 LinkedBlockingQueue
 * （本文件真实的 QUEUE_LINKED 设施，而非 SynchronousQueue 冒充）。worker
 * 仍由 start_worker 通过传入 ThreadFactory.newThread 创建并绑定 trampoline。
 * AOSP 校验：nThreads <= 0 -> IllegalArgumentException；
 * 显式 ThreadFactory == null -> NullPointerException。两者都不返回假池。 */
static bool fixed_create(struct dvm *v,bool with_factory,const union dvm_value *a,int n,union dvm_value *out){
 if(n!=(with_factory?2:1)){dvm__throw(v,"java/lang/IllegalArgumentException","newFixedThreadPool arity %d",n);return true;}
 int threads=a[0].i;
 if(threads<=0){dvm__throw(v,"java/lang/IllegalArgumentException","nThreads <= 0");return true;}
 dvm_ref factory=0;
 if(with_factory){factory=a[1].l;if(!factory){dvm__throw(v,"java/lang/NullPointerException","newFixedThreadPool ThreadFactory");return true;}}
 else {factory=default_thread_factory(v);if(!factory){if(!dvm_exception(v))dvm__throw(v,"java/lang/OutOfMemoryError","newFixedThreadPool default ThreadFactory");return true;}}
 struct dvm_class *pc=dvm_find_class_or_external(v,"java/util/concurrent/ThreadPoolExecutor");
 struct dvm_class *qc=dvm_find_class_or_external(v,"java/util/concurrent/LinkedBlockingQueue");
 dvm_ref pool=pc?dvm_new_object(v,pc):0,queue=qc?dvm_new_object(v,qc):0;
 if(!pool||!queue){dvm__throw(v,"java/lang/OutOfMemoryError","newFixedThreadPool");return true;}
 union dvm_value ignored={0};
 if(!executor_runtime_call(v,"java/util/concurrent/LinkedBlockingQueue","<init>","()V",queue,NULL,0,&ignored)){
  if(!dvm_exception(v))dvm__throw(v,"java/lang/NoSuchMethodError","java/util/concurrent/LinkedBlockingQueue.<init>()V");
  return true;
 }
 union dvm_value unit={0};
 if(!java_basics_static(v,"java/util/concurrent/TimeUnit","MILLISECONDS","Ljava/util/concurrent/TimeUnit;",&unit)||!unit.l){
  if(!dvm_exception(v))dvm__throw(v,"java/lang/IllegalStateException","java/util/concurrent/TimeUnit.MILLISECONDS");
  return true;
 }
 union dvm_value args[6]={{.i=threads},{.i=threads},{.j=0},{.l=unit.l},{.l=queue},{.l=factory}};
 if(!executor_runtime_call(v,"java/util/concurrent/ThreadPoolExecutor","<init>",
    "(IIJLjava/util/concurrent/TimeUnit;Ljava/util/concurrent/BlockingQueue;Ljava/util/concurrent/ThreadFactory;)V",
    pool,args,6,&ignored)){
  if(!dvm_exception(v))dvm__throw(v,"java/lang/NoSuchMethodError","java/util/concurrent/ThreadPoolExecutor.<init>");
  return true;
 }
 sdk_trace_fprintf(stderr,"EXECUTORS_FIXED pool=created core=%d maximum=%d keepalive_ms=0 queue=LinkedBlockingQueue factory=%s\n",threads,threads,with_factory?"explicit":"default");
 out->l=pool;return true;
}
/* AbstractExecutorService.submit 的真实语义：先用 FutureTask 包装任务，
 * 再走 execute 真正排队，成功后返回这个 FutureTask。三个重载共用同一实现:
 *   submit(Runnable)              -> FutureTask(runnable, null)
 *   submit(Runnable, Object)      -> FutureTask(runnable, result)
 *   submit(Callable)              -> FutureTask(callable)
 * 拒绝路径不伪造 Future：默认拒绝处理器会抛 RejectedExecutionException，
 * 自定义处理器若正常返回，execute 按 JDK 行为也算接受（任务不会被运行），
 * 此时 FutureTask 保持 NEW 状态并原样返回，与 AOSP 一致；任何异常都不清理、
 * 不吞掉 pending exception。 */
static bool executor_submit(struct dvm *v,struct executor_state *p,const char *sig,const union dvm_value *a,int n,union dvm_value *out){
 bool callable=!strcmp(sig,"(Ljava/util/concurrent/Callable;)Ljava/util/concurrent/Future;");
 bool runnable1=!strcmp(sig,"(Ljava/lang/Runnable;)Ljava/util/concurrent/Future;");
 bool runnable2=!strcmp(sig,"(Ljava/lang/Runnable;Ljava/lang/Object;)Ljava/util/concurrent/Future;");
 if(!callable&&!runnable1&&!runnable2)return false;
 if(n!=(runnable2?2:1)){dvm__throw(v,"java/lang/IllegalArgumentException","ExecutorService.submit arity %d",n);return true;}
 if(!a[0].l){dvm__throw(v,"java/lang/NullPointerException",callable?"Callable":"Runnable");return true;}
 struct dvm_class *fc=dvm_find_class_or_external(v,"java/util/concurrent/FutureTask");
 dvm_ref ft=fc?dvm_new_object(v,fc):0;
 if(!ft){dvm__throw(v,"java/lang/OutOfMemoryError","ExecutorService.submit FutureTask");return true;}
 union dvm_value fa[2]={{.l=a[0].l},{.l=runnable2?a[1].l:0}};
 const char *ctor=callable?"(Ljava/util/concurrent/Callable;)V":"(Ljava/lang/Runnable;Ljava/lang/Object;)V";
 if(!future_ctor(v,ft,fa,callable?1:2,ctor)){if(!dvm_exception(v))dvm__throw(v,"java/lang/NoSuchMethodError","FutureTask constructor");return true;}
 if(dvm_exception(v))return true;
 if(!executor_execute(v,p,ft)){if(!dvm_exception(v))dvm__throw(v,"java/lang/IllegalStateException","ExecutorService.submit dispatch");return true;}
 if(dvm_exception(v))return true;
 out->l=ft;return true;
}
static bool executor_call(struct dvm *v,const char *c,const char *m,const char *sig,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 if(!strcmp(c,"java/util/concurrent/ThreadPoolExecutor")&& !strcmp(m,"<init>"))return executor_ctor(v,self,a,n,sig);
 if(!strcmp(c,"java/util/concurrent/ScheduledThreadPoolExecutor")&&!strcmp(m,"<init>"))return scheduled_ctor(v,self,a,n,sig);
 struct executor_state *p=executor_of(v,self);if(!p)return false;
 /* DEX 引用了 schedule(Runnable,...)、schedule(Callable,...)、
  * scheduleAtFixedRate、scheduleWithFixedDelay 与 execute；四个入口都进入
  * scheduled_enqueue，由真实 worker 执行并在完成后按周期重新入队。 */
 if(p->scheduled&&!strcmp(m,"schedule")&&!strcmp(sig,"(Ljava/lang/Runnable;JLjava/util/concurrent/TimeUnit;)Ljava/util/concurrent/ScheduledFuture;"))return scheduled_submit(v,p,a,n,out);
 if(p->scheduled&&!strcmp(m,"schedule")&&!strcmp(sig,"(Ljava/util/concurrent/Callable;JLjava/util/concurrent/TimeUnit;)Ljava/util/concurrent/ScheduledFuture;"))return scheduled_submit_callable(v,p,a,n,out);
 if(p->scheduled&&!strcmp(m,"scheduleAtFixedRate")&&!strcmp(sig,"(Ljava/lang/Runnable;JJLjava/util/concurrent/TimeUnit;)Ljava/util/concurrent/ScheduledFuture;"))return scheduled_periodic(v,p,a,n,PERIODIC_FIXED_RATE,out);
 if(p->scheduled&&!strcmp(m,"scheduleWithFixedDelay")&&!strcmp(sig,"(Ljava/lang/Runnable;JJLjava/util/concurrent/TimeUnit;)Ljava/util/concurrent/ScheduledFuture;"))return scheduled_periodic(v,p,a,n,PERIODIC_FIXED_DELAY,out);
 /* AOSP ScheduledThreadPoolExecutor.execute(command) == schedule(command, 0,
  * NANOSECONDS); the returned future is discarded by the caller. */
 if(p->scheduled&&!strcmp(m,"execute")&&!strcmp(sig,"(Ljava/lang/Runnable;)V")){
  if(n!=1)return false;if(!a[0].l){dvm__throw(v,"java/lang/NullPointerException","execute command");return true;}
  return scheduled_enqueue(v,p,a[0].l,false,PERIODIC_NONE,0,0,out);
 }
 if(!strcmp(m,"execute")&&!strcmp(sig,"(Ljava/lang/Runnable;)V"))return executor_execute(v,p,a[0].l);
 if(!strcmp(m,"submit")){
  if(p->scheduled){dvm__throw(v,"java/lang/UnsupportedOperationException","scheduled executor submit is not implemented");return true;}
  return executor_submit(v,p,sig,a,n,out);
 }
 if(!strcmp(m,"shutdown")&&!strcmp(sig,"()V")){pthread_mutex_lock(&p->queue->lock);p->shutdown=true;pthread_cond_broadcast(&p->queue->changed);pthread_mutex_unlock(&p->queue->lock);return true;}
 if(!strcmp(m,"isShutdown")&&!strcmp(sig,"()Z")){pthread_mutex_lock(&p->queue->lock);out->i=p->shutdown;pthread_mutex_unlock(&p->queue->lock);return true;}
 if(!strcmp(m,"isTerminated")&&!strcmp(sig,"()Z")){pthread_mutex_lock(&p->queue->lock);out->i=p->shutdown&&p->live==0;pthread_mutex_unlock(&p->queue->lock);return true;}
 if(!strcmp(m,"getPoolSize")&&!strcmp(sig,"()I")){pthread_mutex_lock(&p->queue->lock);out->i=(int)p->live;pthread_mutex_unlock(&p->queue->lock);return true;}
 if(!strcmp(m,"getActiveCount")&&!strcmp(sig,"()I")){pthread_mutex_lock(&p->queue->lock);out->i=(int)p->active;pthread_mutex_unlock(&p->queue->lock);return true;}
 if(!strcmp(m,"getTaskCount")&&!strcmp(sig,"()J")){pthread_mutex_lock(&p->queue->lock);out->j=(int64_t)p->executed+p->active+queue_count_locked(p->queue);pthread_mutex_unlock(&p->queue->lock);return true;}
 if(!strcmp(m,"getCompletedTaskCount")&&!strcmp(sig,"()J")){pthread_mutex_lock(&p->queue->lock);out->j=p->executed;pthread_mutex_unlock(&p->queue->lock);return true;}
 if(!strcmp(m,"awaitTermination")&&!strcmp(sig,"(JLjava/util/concurrent/TimeUnit;)Z")){
  if(a[0].j<0){dvm__throw(v,"java/lang/IllegalArgumentException","negative timeout");return true;}uint64_t unit=0;if(!unit_millis(v,a[1].l,&unit))return true;uint64_t ms=((uint64_t)a[0].j*unit+999999u)/1000000u,end=now_ms()+ms;
  pthread_mutex_lock(&p->queue->lock);while(!(p->shutdown&&p->live==0)&&now_ms()<end){uint64_t wait=end-now_ms();if(wait>50)wait=50;queue_wait_locked(v,p->queue,wait);}out->i=p->shutdown&&p->live==0;pthread_mutex_unlock(&p->queue->lock);return true;
 }
 /* ThreadPoolExecutor.setKeepAliveTime(long,TimeUnit): Java 要求 time > 0，
  * 否则抛 IllegalArgumentException；正数按 TimeUnit.toNanos 的饱和语义换算，
  * 小于 1ms 的正值向上取整为 1ms，保持 allowCoreThreadTimeOut(true) 可用的
  * "非零 keep-alive" 事实。设置后唤醒空闲 worker 让新值立即生效。 */
 if(!strcmp(m,"setKeepAliveTime")&&!strcmp(sig,"(JLjava/util/concurrent/TimeUnit;)V")){
  if(n!=2||a[0].j<=0){dvm__throw(v,"java/lang/IllegalArgumentException","keepAliveTime must be positive");return true;}
  uint64_t scale=0;if(!unit_millis(v,a[1].l,&scale))return true;
  __uint128_t total=(__uint128_t)(uint64_t)a[0].j*scale;
  uint64_t ms=total/1000000u>UINT64_MAX?UINT64_MAX:(uint64_t)(total/1000000u);
  if(ms==0)ms=1;
  pthread_mutex_lock(&p->queue->lock);p->keep_alive_ms=ms;pthread_cond_broadcast(&p->queue->changed);pthread_mutex_unlock(&p->queue->lock);return true;
 }
 /* ThreadPoolExecutor.allowCoreThreadTimeOut(boolean): 真实存储标志并唤醒
  * 空闲 worker 让新的 allow_core_timeout 条件立即生效。AOSP 语义要求
  * true 时 keepAliveTime > 0，否则抛 IllegalArgumentException；参数从
  * a[0].i 读取。false 只关闭标志，不改变 core 线程存活。 */
 if(!strcmp(m,"allowCoreThreadTimeOut")&&!strcmp(sig,"(Z)V")){
  if(n!=1){dvm__throw(v,"java/lang/IllegalArgumentException","allowCoreThreadTimeOut arity %d",n);return true;}
  bool value=a&&a[0].i!=0;
  pthread_mutex_lock(&p->queue->lock);
  if(value&&!p->keep_alive_ms){pthread_mutex_unlock(&p->queue->lock);dvm__throw(v,"java/lang/IllegalArgumentException","Core threads must have nonzero keep alive times");return true;}
  if(p->allow_core_timeout!=value){p->allow_core_timeout=value;if(value)pthread_cond_broadcast(&p->queue->changed);}
  pthread_mutex_unlock(&p->queue->lock);return true;
 }
 if(!strcmp(m,"prestartCoreThread")&&!strcmp(sig,"()Z")){pthread_mutex_lock(&p->queue->lock);bool need=!p->shutdown&&p->live<p->core;pthread_mutex_unlock(&p->queue->lock);if(!need){out->i=0;return true;}out->i=start_worker(p,0,true)!=NULL;return true;}
 if(!strcmp(m,"prestartAllCoreThreads")&&!strcmp(sig,"()I")){while(1){pthread_mutex_lock(&p->queue->lock);bool need=!p->shutdown&&p->live<p->core;pthread_mutex_unlock(&p->queue->lock);if(!need)break;if(!start_worker(p,0,true))break;++out->i;}return true;}
 if(!strcmp(m,"remove")&&!strcmp(sig,"(Ljava/lang/Runnable;)Z"))return queue_contains(v,p->queue,a[0].l,true,false,out);
 return false;
}

bool executor_runtime_call(struct dvm *v,const char *c,const char *m,const char *sig,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 memset(out,0,sizeof *out);
 if(!strcmp(c,"java/util/concurrent/Executors")&&!strcmp(m,"newScheduledThreadPool")&&!strcmp(sig,"(I)Ljava/util/concurrent/ScheduledExecutorService;"))return scheduled_create(v,a,n,out);
 if(!strcmp(c,"java/util/concurrent/Executors")&&!strcmp(m,"newScheduledThreadPool")&&!strcmp(sig,"(ILjava/util/concurrent/ThreadFactory;)Ljava/util/concurrent/ScheduledExecutorService;"))return scheduled_create(v,a,n,out);
 /* AOSP Executors.newSingleThreadScheduledExecutor() is
  * newScheduledThreadPool(1, defaultThreadFactory()) and its factory
  * overload passes the caller's factory; both use the same pool shape, so
  * both delegate to scheduled_create with corePoolSize 1 (Bugly's
  * proguard.bg reached this gap in 72bt). */
 if(!strcmp(c,"java/util/concurrent/Executors")&&!strcmp(m,"newSingleThreadScheduledExecutor")&&
    (!strcmp(sig,"()Ljava/util/concurrent/ScheduledExecutorService;")||!strcmp(sig,"(Ljava/util/concurrent/ThreadFactory;)Ljava/util/concurrent/ScheduledExecutorService;"))){
  bool with_factory=!strcmp(sig,"(Ljava/util/concurrent/ThreadFactory;)Ljava/util/concurrent/ScheduledExecutorService;");
  if(n!=(with_factory?1:0)){dvm__throw(v,"java/lang/IllegalArgumentException","newSingleThreadScheduledExecutor arity %d",n);return true;}
  if(with_factory&&!a[0].l){dvm__throw(v,"java/lang/NullPointerException","scheduled ThreadFactory");return true;}
  union dvm_value args[2];args[0].i=1;
  if(with_factory)args[1]=a[0];
  return scheduled_create(v,args,with_factory?2:1,out);
 }
 if(!strcmp(c,"java/util/concurrent/Executors")&&!strcmp(m,"defaultThreadFactory")&&!strcmp(sig,"()Ljava/util/concurrent/ThreadFactory;")){
  if(n){dvm__throw(v,"java/lang/IllegalArgumentException","defaultThreadFactory arity %d",n);return true;}
  dvm_ref factory=default_thread_factory(v);if(!factory)return true;out->l=factory;return true;
 }
 if(!strcmp(c,"java/util/concurrent/Executors")&&!strcmp(m,"newCachedThreadPool")&&!strcmp(sig,"()Ljava/util/concurrent/ExecutorService;"))return cached_create(v,false,a,n,out);
 if(!strcmp(c,"java/util/concurrent/Executors")&&!strcmp(m,"newCachedThreadPool")&&!strcmp(sig,"(Ljava/util/concurrent/ThreadFactory;)Ljava/util/concurrent/ExecutorService;"))return cached_create(v,true,a,n,out);
 /* DEX 同时引用两个重载：newFixedThreadPool(I) 与
  * newFixedThreadPool(ILjava/util/concurrent/ThreadFactory;)。前者按 AOSP
  * 使用 defaultThreadFactory()，不是 null。 */
 if(!strcmp(c,"java/util/concurrent/Executors")&&!strcmp(m,"newFixedThreadPool")&&!strcmp(sig,"(I)Ljava/util/concurrent/ExecutorService;"))return fixed_create(v,false,a,n,out);
 if(!strcmp(c,"java/util/concurrent/Executors")&&!strcmp(m,"newFixedThreadPool")&&!strcmp(sig,"(ILjava/util/concurrent/ThreadFactory;)Ljava/util/concurrent/ExecutorService;"))return fixed_create(v,true,a,n,out);
 if(!strcmp(c,"java/util/concurrent/LinkedBlockingDeque")||!strcmp(c,"java/util/concurrent/LinkedBlockingQueue")||!strcmp(c,"java/util/concurrent/ArrayBlockingQueue")||!strcmp(c,"java/util/concurrent/SynchronousQueue")||queue_of(v,self))return queue_call(v,c,m,sig,self,a,n,out);
 /* Guest code constructs the rejection policies directly and passes them to
  * ThreadPoolExecutor.  Only the no-arg constructor and rejectedExecution
  * are referenced by the APK; the actual policy behavior is dispatched in
  * executor_reject by class name. */
 if(!strcmp(c,"java/util/concurrent/ThreadPoolExecutor$DiscardPolicy")||
    !strcmp(c,"java/util/concurrent/ThreadPoolExecutor$CallerRunsPolicy")||
    !strcmp(c,"java/util/concurrent/ThreadPoolExecutor$DiscardOldestPolicy")){
  if(!strcmp(m,"<init>"))return rejection_policy_ctor(v,self,a,n,out);
  if(!strcmp(m,"rejectedExecution")&&!strcmp(sig,"(Ljava/lang/Runnable;Ljava/util/concurrent/ThreadPoolExecutor;)V"))return rejection_policy_dispatch(v,self,a,n,out);
  return false;
 }
 if(!strcmp(c,"java/util/concurrent/FutureTask")||!strcmp(c,"java/util/concurrent/ScheduledFuture")||future_of(v,self))return future_call(v,m,sig,self,a,n,out);
 if(!strcmp(c,"java/util/concurrent/ThreadPoolExecutor")||
    !strcmp(c,"java/util/concurrent/ExecutorService")||
    !strcmp(c,"java/util/concurrent/Executor")||!strcmp(c,"java/util/concurrent/ScheduledExecutorService")||!strcmp(c,"java/util/concurrent/ScheduledThreadPoolExecutor"))return executor_call(v,c,m,sig,self,a,n,out);
 return false;
}

void executor_runtime_shutdown(struct dvm *v,unsigned grace){
 bool force=false;
 for(struct executor_state *p=all_executors;p;p=p->next){if(p->vm!=v)continue;uint64_t end=now_ms()+grace;struct queue_state *q=p->queue;pthread_mutex_lock(&q->lock);
  while((p->active||queue_count_locked(q))&&now_ms()<end){uint64_t wait=end-now_ms();if(wait>20)wait=20;queue_wait_locked(v,q,wait);}
  p->shutdown=true;bool busy=p->active!=0;if(queue_count_locked(q)){p->cancelled+=(unsigned)queue_count_locked(q);if(p->scheduled){for(size_t i=0;i<q->size;i++){struct future_state *f=future_of(v,q->items[(q->head+i)%q->allocated]);if(f){pthread_mutex_lock(&f->lock);if(f->status==FUTURE_NEW)f->status=FUTURE_CANCELLED;pthread_cond_broadcast(&f->changed);pthread_mutex_unlock(&f->lock);}}}memset(q->items,0,q->allocated*sizeof *q->items);q->size=q->head=0;}if(busy)force=true;pthread_cond_broadcast(&q->changed);pthread_mutex_unlock(&q->lock);
 }
 for(struct queue_state *q=all_queues;q;q=q->next)if(q->vm==v){pthread_mutex_lock(&q->lock);q->closed=true;pthread_cond_broadcast(&q->changed);pthread_mutex_unlock(&q->lock);}
 if(force)dvm_request_stop(v);
 /* A guest task can be blocked in a real Condition.await() (DiskWriter's
  * worker is one).  That path polls dvm_is_stopping() only after re-taking
  * the interpreter lock, so joining while still holding the GIL here would
  * deadlock shutdown: the joiner waits for the worker, the worker waits for
  * the GIL.  Join with the GIL released; dvm_request_stop() above already
  * set the atomic stop flag and woke network waiters. */
 unsigned join_gil_depth=dvm_gil_unlock_all(v);
 for(struct executor_state *p=all_executors;p;p=p->next){if(p->vm!=v)continue;for(struct executor_worker *w=p->workers;w;w=w->next){if(!w->started||w->joined)continue;thread_runtime_join_host(v,w->thread);w->joined=true;}}
 dvm_gil_relock(v,join_gil_depth);
 for(struct executor_state *p=all_executors;p;p=p->next)if(p->vm==v){pthread_mutex_lock(&p->queue->lock);sdk_trace_fprintf(stderr,"EXECUTOR_SHUTDOWN joined=1 executed=%u cancelled=%u busy_at_deadline=%d\n",p->executed,p->cancelled,p->active!=0);pthread_mutex_unlock(&p->queue->lock);}
 /* The global pool/queue lists keep every record for the process lifetime
  * (their memory is freed by VM teardown, not here), and lookups filter by
  * p->vm == v.  malloc reuses a destroyed dvm's address, so a later VM can
  * be handed the same pointer and would match records belonging to the
  * dead instance.  Detach this VM's records after the workers are joined;
  * a NULL vm never matches a live VM.  default_factory_ref is a pinned
  * guest reference, and the pin lives inside the dying VM, so it must be
  * cleared on the VM that owns it for the same reason. */
 for(struct executor_state *p=all_executors;p;p=p->next)if(p->vm==v)p->vm=NULL;
 for(struct queue_state *q=all_queues;q;q=q->next)if(q->vm==v)q->vm=NULL;
 if(default_factory_vm==v){default_factory_vm=NULL;default_factory_ref=0;}
}

/* 等待 worker 把 callable fixture 执行到 target 次。等待期间持有队列锁并
 * 调用 queue_wait_locked：后者会释放解释器锁，worker 才能拿到 GIL 执行
 * guest callable；不能持 GIL 死等。 */
static bool executor_selftest_wait_calls(struct dvm *v,struct executor_state *p,unsigned target,uint64_t timeout_ms){
 pthread_mutex_lock(&p->queue->lock);uint64_t end=now_ms()+timeout_ms;
 while(atomic_load(&executor_test_calls)<target&&now_ms()<end)queue_wait_locked(v,p->queue,10);
 bool reached=atomic_load(&executor_test_calls)>=target;
 pthread_mutex_unlock(&p->queue->lock);return reached;
}
/* 周期任务在两次执行之间处于 NEW；RUNNING 窗口内的 cancel 按现有 FutureTask
 * 语义返回 false。这里只重试状态竞争，不伪造取消成功；cancel 成功后任务已
 * 从 scheduled queue 移除且不会再被重排。 */
static bool executor_selftest_cancel_when_new(struct dvm *v,struct executor_state *p,dvm_ref future){
 union dvm_value out={0};
 for(unsigned i=0;i<4000;i++){
  union dvm_value arg={.i=0};out.i=0;
  if(!future_call(v,"cancel","(Z)Z",future,&arg,1,&out))return false;
  if(out.i)return true;
  pthread_mutex_lock(&p->queue->lock);queue_wait_locked(v,p->queue,2);pthread_mutex_unlock(&p->queue->lock);
 }
 return false;
}
static void executor_selftest_idle(struct dvm *v,struct executor_state *p,uint64_t ms){
 pthread_mutex_lock(&p->queue->lock);uint64_t end=now_ms()+ms;
 while(now_ms()<end)queue_wait_locked(v,p->queue,10);
 pthread_mutex_unlock(&p->queue->lock);
}

bool executor_runtime_selftest(struct dvm *v,unsigned *checks){
 bool ok=true;union dvm_value out={0},args[5]={{.i=1},{.i=1},{.j=0},{0},{0}};
#define EXEC_CHECK(expr) do{bool pass=!!(expr);if(dvm_exception(v)){pass=false;dvm_clear_exception(v);}if(checks)++*checks;if(!pass){ok=false;sdk_trace_fprintf(stderr,"EXECUTOR_SELFTEST_FAIL line=%d expression=%s\n",__LINE__,#expr);}}while(0)
 struct dvm_class *queue_class=dvm_find_class_or_external(v,"java/util/concurrent/LinkedBlockingDeque");
 struct dvm_class *pool_class=dvm_find_class_or_external(v,"java/util/concurrent/ThreadPoolExecutor");
 struct dvm_class *future_class=dvm_find_class_or_external(v,"java/util/concurrent/FutureTask");
 struct dvm_class *callable_class=dvm_find_class_or_external(v,"java/util/concurrent/ExecutorRuntimeSelftestCallable");
 struct dvm_class *runnable_class=dvm_find_class_or_external(v,"java/util/concurrent/ExecutorRuntimeSelftestRunnable");
 dvm_ref queue=dvm_new_object(v,queue_class),pool=dvm_new_object(v,pool_class),future=dvm_new_object(v,future_class);
 dvm_ref callable=dvm_new_object(v,callable_class),unit=0;
 dvm_ref selftest_runnable=dvm_new_object(v,runnable_class);
 if(!queue||!pool||!future||!callable||!selftest_runnable||
    !install_executor_test_callable(v,callable_class)||!install_executor_test_runnable(v,runnable_class)){ok=false;goto done;}
 struct executor_test_value *value=calloc(1,sizeof *value);if(!value){dvm__throw(v,"java/lang/OutOfMemoryError","executor selftest value");ok=false;goto done;}
 value->result=dvm_new_string(v,"executor-result");struct dvm_object *callable_object=dvm__obj(v,callable);callable_object->data=value;callable_object->data_destroy=free;
 if(!value->result){ok=false;goto done;}
 atomic_store(&executor_test_calls,0);
 EXEC_CHECK(executor_runtime_call(v,"java/util/concurrent/LinkedBlockingDeque","<init>","()V",queue,NULL,0,&out));
 bool got_unit=java_basics_static(v,"java/util/concurrent/TimeUnit","SECONDS","Ljava/util/concurrent/TimeUnit;",&out);unit=out.l;EXEC_CHECK(got_unit&&unit);
 args[3].l=unit;args[4].l=queue;
 EXEC_CHECK(executor_runtime_call(v,"java/util/concurrent/ThreadPoolExecutor","<init>","(IIJLjava/util/concurrent/TimeUnit;Ljava/util/concurrent/BlockingQueue;)V",pool,args,5,&out));
 union dvm_value callable_arg={.l=callable};
 EXEC_CHECK(executor_runtime_call(v,"java/util/concurrent/FutureTask","<init>","(Ljava/util/concurrent/Callable;)V",future,&callable_arg,1,&out));
 union dvm_value task_arg={.l=future};
 EXEC_CHECK(executor_runtime_call(v,"java/util/concurrent/ExecutorService","execute","(Ljava/lang/Runnable;)V",pool,&task_arg,1,&out));
 union dvm_value wait_args[2]={{.j=1},{.l=unit}};
 EXEC_CHECK(atomic_load(&executor_test_calls)==0);
 bool got_result=executor_runtime_call(v,"java/util/concurrent/Future","get","(JLjava/util/concurrent/TimeUnit;)Ljava/lang/Object;",future,wait_args,2,&out);EXEC_CHECK(got_result&&out.l==value->result);
 EXEC_CHECK(atomic_load(&executor_test_calls)==1);
 bool done_state=executor_runtime_call(v,"java/util/concurrent/Future","isDone","()Z",future,NULL,0,&out);EXEC_CHECK(done_state&&out.i);
 /* submit 必须真实返回 Future 并真实排队执行，而不只是 execute 的别名。 */
 unsigned submitted_before=atomic_load(&executor_test_calls);
 bool submitted=executor_runtime_call(v,"java/util/concurrent/ExecutorService","submit","(Ljava/util/concurrent/Callable;)Ljava/util/concurrent/Future;",pool,&callable_arg,1,&out);
 dvm_ref submitted_future=submitted?out.l:0;EXEC_CHECK(submitted&&submitted_future);
 bool submitted_got=submitted_future&&executor_runtime_call(v,"java/util/concurrent/Future","get","(JLjava/util/concurrent/TimeUnit;)Ljava/lang/Object;",submitted_future,wait_args,2,&out);
 EXEC_CHECK(submitted_got&&out.l==value->result);
 EXEC_CHECK(atomic_load(&executor_test_calls)==submitted_before+1);
 EXEC_CHECK(executor_runtime_call(v,"java/util/concurrent/ExecutorService","shutdown","()V",pool,NULL,0,&out));
 bool terminated=executor_runtime_call(v,"java/util/concurrent/ExecutorService","awaitTermination","(JLjava/util/concurrent/TimeUnit;)Z",pool,wait_args,2,&out);EXEC_CHECK(terminated&&out.i);
 {
  struct dvm_class *sfc=dvm_find_class_or_external(v,"test/ScheduledFactoryFixture");
  bool installed=fixture_install(sfc,"newThread","(Ljava/lang/Runnable;)Ljava/lang/Thread;",fixture_factory,'L');EXEC_CHECK(installed);
  dvm_ref factory=installed?dvm_new_object(v,sfc):0;union dvm_value ca[2]={{.i=1},{.l=factory}};
  EXEC_CHECK(executor_runtime_call(v,"java/util/concurrent/Executors","newScheduledThreadPool","(ILjava/util/concurrent/ThreadFactory;)Ljava/util/concurrent/ScheduledExecutorService;",0,ca,2,&out));dvm_ref sp=out.l;
  dvm_ref inner=dvm_new_object(v,future_class);EXEC_CHECK(future_ctor(v,inner,&callable_arg,1,"(Ljava/util/concurrent/Callable;)V"));
  EXEC_CHECK(java_basics_static(v,"java/util/concurrent/TimeUnit","MILLISECONDS","Ljava/util/concurrent/TimeUnit;",&out));dvm_ref ms=out.l;
  union dvm_value sa[3]={{.l=inner},{.j=120},{.l=ms}};unsigned before=atomic_load(&executor_test_calls);uint64_t started=now_ms();fixture_thread=0;
  EXEC_CHECK(executor_runtime_call(v,"java/util/concurrent/ScheduledExecutorService","schedule","(Ljava/lang/Runnable;JLjava/util/concurrent/TimeUnit;)Ljava/util/concurrent/ScheduledFuture;",sp,sa,3,&out));dvm_ref sf=out.l;EXEC_CHECK(fixture_thread!=0);
  EXEC_CHECK(atomic_load(&executor_test_calls)==before);
  union dvm_value wa[2]={{.j=2},{.l=unit}};EXEC_CHECK(future_call(v,"get","(JLjava/util/concurrent/TimeUnit;)Ljava/lang/Object;",sf,wa,2,&out));
  EXEC_CHECK(now_ms()-started>=100&&atomic_load(&executor_test_calls)==before+1);
  /* BLogThreadFactory 路径：newThread 返回前已 setPriority(10)，线程仍必须
   * 能被 start_worker 绑定并真实执行任务；Java priority 存储为 10。 */
  union dvm_value prio_out={0};
  bool got_priority=fixture_thread&&thread_runtime_call(v,"java/lang/Thread","getPriority","()I",fixture_thread,NULL,0,&prio_out);
  EXEC_CHECK(got_priority&&prio_out.i==10);
  sa[1].j=1000;EXEC_CHECK(executor_runtime_call(v,"java/util/concurrent/ScheduledExecutorService","schedule","(Ljava/lang/Runnable;JLjava/util/concurrent/TimeUnit;)Ljava/util/concurrent/ScheduledFuture;",sp,sa,3,&out));dvm_ref cancelled=out.l;union dvm_value cancelarg={.i=0};
  EXEC_CHECK(future_call(v,"cancel","(Z)Z",cancelled,&cancelarg,1,&out)&&out.i);
  EXEC_CHECK(executor_runtime_call(v,"java/util/concurrent/ExecutorService","shutdown","()V",sp,NULL,0,&out));
 }
 {
  /* rebuild72an 真机 MISSING_API:
   * ScheduledExecutorService.scheduleWithFixedDelay。这里不是只返回一个
   * Future：worker 会真实执行 fixture，至少 2 次；cancel 成功后 150ms 内不
   * 允许再执行；period<=0 抛 IllegalArgumentException；initialDelay=0 与
   * 负数 initialDelay 都按立即执行。同时覆盖 DEX 引用的
   * scheduleAtFixedRate、schedule(Callable,...)、ScheduledThreadPoolExecutor
   * .execute 和直接构造器，避免后续 MISSING_API 级联。 */
  dvm_ref ms_unit=0;
  bool got_ms=java_basics_static(v,"java/util/concurrent/TimeUnit","MILLISECONDS","Ljava/util/concurrent/TimeUnit;",&out);
  ms_unit=out.l;EXEC_CHECK(got_ms&&ms_unit);
  union dvm_value sc1[1]={{.i=1}};
  EXEC_CHECK(executor_runtime_call(v,"java/util/concurrent/Executors","newScheduledThreadPool","(I)Ljava/util/concurrent/ScheduledExecutorService;",0,sc1,1,&out));
  dvm_ref sp=out.l;struct dvm_object *spo=sp?dvm__obj(v,sp):NULL;struct executor_state *sps=spo?spo->data:NULL;
  bool scheduled_shape=sps&&sps->scheduled&&sps->core==1&&sps->maximum==INT_MAX&&sps->keep_alive_ms==10;
  EXEC_CHECK(sp&&scheduled_shape);
  if(sps){
   /* Runnable fixture，而不是 FutureTask(Callable)：Java 的 FutureTask.run()
    * 完成后再次 run() 是 no-op，把它当周期命令只能测出“callable 执行一次”，
    * 无法证明 worker 会按 period 重复调用 Runnable.run()。这里直接用每次
    * run() 都计数的 fixture，断言的是调度器真实重排行为。 */
   union dvm_value pa[4]={{.l=selftest_runnable},{.j=0},{.j=40},{.l=ms_unit}};
   unsigned before=atomic_load(&executor_test_calls);
   bool sched=executor_runtime_call(v,"java/util/concurrent/ScheduledExecutorService","scheduleWithFixedDelay","(Ljava/lang/Runnable;JJLjava/util/concurrent/TimeUnit;)Ljava/util/concurrent/ScheduledFuture;",sp,pa,4,&out);
   dvm_ref pf=out.l;struct dvm_object *pfo=pf?dvm__obj(v,pf):NULL;struct future_state *pfs=pfo?pfo->data:NULL;
   bool meta=pfs&&pfs->periodic==PERIODIC_FIXED_DELAY&&pfs->period_ms==40&&pfs->scheduled_queue==sps->queue;
   EXEC_CHECK(sched&&pf&&meta);
   EXEC_CHECK(pf&&future_call(v,"isDone","()Z",pf,NULL,0,&out)&&!out.i);
   EXEC_CHECK(executor_selftest_wait_calls(v,sps,before+2,3000));
   bool cancelled=pf&&executor_selftest_cancel_when_new(v,sps,pf);
   EXEC_CHECK(cancelled);
   if(cancelled){
    unsigned stopped=atomic_load(&executor_test_calls);
    executor_selftest_idle(v,sps,150);
    EXEC_CHECK(atomic_load(&executor_test_calls)==stopped);
    EXEC_CHECK(future_call(v,"isCancelled","()Z",pf,NULL,0,&out)&&out.i);
    EXEC_CHECK(future_call(v,"isDone","()Z",pf,NULL,0,&out)&&out.i);
   }
   /* fixed-rate：负数 initialDelay 按 0，period 40ms，至少执行 2 次后取消。
    * 同样使用普通 Runnable fixture，每次 run() 都必须被真实调用并计数。 */
   union dvm_value ra[4]={{.l=selftest_runnable},{.j=-25},{.j=40},{.l=ms_unit}};
   unsigned rate_before=atomic_load(&executor_test_calls);
   bool rate_sched=executor_runtime_call(v,"java/util/concurrent/ScheduledExecutorService","scheduleAtFixedRate","(Ljava/lang/Runnable;JJLjava/util/concurrent/TimeUnit;)Ljava/util/concurrent/ScheduledFuture;",sp,ra,4,&out);
   dvm_ref rf=out.l;struct dvm_object *rfo=rf?dvm__obj(v,rf):NULL;struct future_state *rfs=rfo?rfo->data:NULL;
   bool rate_meta=rfs&&rfs->periodic==PERIODIC_FIXED_RATE&&rfs->period_ms==40;
   EXEC_CHECK(rate_sched&&rf&&rate_meta);
   EXEC_CHECK(executor_selftest_wait_calls(v,sps,rate_before+2,3000));
   bool rate_cancelled=rf&&executor_selftest_cancel_when_new(v,sps,rf);
   EXEC_CHECK(rate_cancelled);
   if(rate_cancelled){
    unsigned rate_stopped=atomic_load(&executor_test_calls);
    executor_selftest_idle(v,sps,150);
    EXEC_CHECK(atomic_load(&executor_test_calls)==rate_stopped);
   }
   /* period 0 与负数都必须是 IllegalArgumentException（AOSP 语义），null
    * TimeUnit 是 NullPointerException；这些调用不得入队或创建 worker。 */
   dvm_ref badft=dvm_new_object(v,future_class);
   EXEC_CHECK(badft&&future_ctor(v,badft,&callable_arg,1,"(Ljava/util/concurrent/Callable;)V"));
   if(badft&&!dvm_exception(v)){
    union dvm_value zero[4]={{.l=badft},{.j=0},{.j=0},{.l=ms_unit}};
    bool zr=executor_runtime_call(v,"java/util/concurrent/ScheduledExecutorService","scheduleWithFixedDelay","(Ljava/lang/Runnable;JJLjava/util/concurrent/TimeUnit;)Ljava/util/concurrent/ScheduledFuture;",sp,zero,4,&out);
    bool zi=exception_is(v,"java/lang/IllegalArgumentException");if(dvm_exception(v))dvm_clear_exception(v);
    EXEC_CHECK(zr&&zi);
    union dvm_value neg[4]={{.l=badft},{.j=0},{.j=-1},{.l=ms_unit}};
    bool nr=executor_runtime_call(v,"java/util/concurrent/ScheduledExecutorService","scheduleAtFixedRate","(Ljava/lang/Runnable;JJLjava/util/concurrent/TimeUnit;)Ljava/util/concurrent/ScheduledFuture;",sp,neg,4,&out);
    bool ni=exception_is(v,"java/lang/IllegalArgumentException");if(dvm_exception(v))dvm_clear_exception(v);
    EXEC_CHECK(nr&&ni);
    union dvm_value nu[4]={{.l=badft},{.j=0},{.j=40},{.l=0}};
    bool ur=executor_runtime_call(v,"java/util/concurrent/ScheduledExecutorService","scheduleWithFixedDelay","(Ljava/lang/Runnable;JJLjava/util/concurrent/TimeUnit;)Ljava/util/concurrent/ScheduledFuture;",sp,nu,4,&out);
    bool ui=exception_is(v,"java/lang/NullPointerException");if(dvm_exception(v))dvm_clear_exception(v);
    EXEC_CHECK(ur&&ui);
   }
   /* schedule(Callable,...) 必须真实调用 callable 并把结果交给 get()。 */
   union dvm_value cca[3]={{.l=callable},{.j=10},{.l=ms_unit}};
   unsigned callable_before=atomic_load(&executor_test_calls);
   bool callable_sched=executor_runtime_call(v,"java/util/concurrent/ScheduledExecutorService","schedule","(Ljava/util/concurrent/Callable;JLjava/util/concurrent/TimeUnit;)Ljava/util/concurrent/ScheduledFuture;",sp,cca,3,&out);
   dvm_ref callable_future=out.l;EXEC_CHECK(callable_sched&&callable_future);
   if(callable_future){
    bool callable_got=future_call(v,"get","(JLjava/util/concurrent/TimeUnit;)Ljava/lang/Object;",callable_future,wait_args,2,&out);
    EXEC_CHECK(callable_got&&out.l==value->result);
    EXEC_CHECK(atomic_load(&executor_test_calls)==callable_before+1);
   }
   /* ScheduledThreadPoolExecutor.execute == schedule(task,0,NANOSECONDS)。 */
   dvm_ref eft=dvm_new_object(v,future_class);
   EXEC_CHECK(eft&&future_ctor(v,eft,&callable_arg,1,"(Ljava/util/concurrent/Callable;)V"));
   if(eft&&!dvm_exception(v)){
    union dvm_value ea={.l=eft};unsigned before=atomic_load(&executor_test_calls);
    EXEC_CHECK(executor_runtime_call(v,"java/util/concurrent/ScheduledExecutorService","execute","(Ljava/lang/Runnable;)V",sp,&ea,1,&out));
    EXEC_CHECK(executor_selftest_wait_calls(v,sps,before+1,2000));
   }
   /* getDelay(TimeUnit) 必须来自真实 deadline：1000ms 一次性任务的剩余时间
    * 在 (0,1000] 毫秒之间，而不是伪造的 0。 */
   dvm_ref gft=dvm_new_object(v,future_class);
   EXEC_CHECK(gft&&future_ctor(v,gft,&callable_arg,1,"(Ljava/util/concurrent/Callable;)V"));
   if(gft&&!dvm_exception(v)){
    union dvm_value ga[3]={{.l=gft},{.j=1000},{.l=ms_unit}};
    bool gs=executor_runtime_call(v,"java/util/concurrent/ScheduledExecutorService","schedule","(Ljava/lang/Runnable;JLjava/util/concurrent/TimeUnit;)Ljava/util/concurrent/ScheduledFuture;",sp,ga,3,&out);
    dvm_ref gf=out.l;EXEC_CHECK(gs&&gf);
    if(gf){
     union dvm_value du={.l=ms_unit};
     bool gd=future_call(v,"getDelay","(Ljava/util/concurrent/TimeUnit;)J",gf,&du,1,&out);
     EXEC_CHECK(gd&&out.j>0&&out.j<=1000);
     union dvm_value carg={.i=0};EXEC_CHECK(future_call(v,"cancel","(Z)Z",gf,&carg,1,&out)&&out.i);
    }
   }
  }
  /* corePoolSize=0 是 AOSP 允许的；首个任务仍必须由 ensurePrestart 规则
   * 启动一个真实 worker 并执行，而不是永远留在队列里。 */
  union dvm_value zc[1]={{.i=0}};
  EXEC_CHECK(executor_runtime_call(v,"java/util/concurrent/Executors","newScheduledThreadPool","(I)Ljava/util/concurrent/ScheduledExecutorService;",0,zc,1,&out));
  dvm_ref sp0=out.l;struct dvm_object *sp0o=sp0?dvm__obj(v,sp0):NULL;struct executor_state *sp0s=sp0o?sp0o->data:NULL;
  bool zero_core=sp0s&&sp0s->core==0&&sp0s->scheduled;
  EXEC_CHECK(sp0&&zero_core);
  if(sp0s){
   dvm_ref zft=dvm_new_object(v,future_class);
   EXEC_CHECK(zft&&future_ctor(v,zft,&callable_arg,1,"(Ljava/util/concurrent/Callable;)V"));
   if(zft&&!dvm_exception(v)){
    union dvm_value za[3]={{.l=zft},{.j=0},{.l=ms_unit}};
    unsigned before=atomic_load(&executor_test_calls);
    EXEC_CHECK(executor_runtime_call(v,"java/util/concurrent/ScheduledExecutorService","schedule","(Ljava/lang/Runnable;JLjava/util/concurrent/TimeUnit;)Ljava/util/concurrent/ScheduledFuture;",sp0,za,3,&out));
    EXEC_CHECK(executor_selftest_wait_calls(v,sp0s,before+1,2000));
   }
   EXEC_CHECK(executor_runtime_call(v,"java/util/concurrent/ExecutorService","shutdown","()V",sp0,NULL,0,&out));
  }
  /* 直接构造 ScheduledThreadPoolExecutor：DEX 引用了
   * (ILjava/util/concurrent/ThreadFactory;)V 与三参数形式。构造后必须能真实
   * 执行任务；setKeepAliveTime 更新真实字段并拒绝 0。 */
  dvm_ref direct=dvm_new_object(v,dvm_find_class_or_external(v,"java/util/concurrent/ScheduledThreadPoolExecutor"));
  dvm_ref df=default_thread_factory(v);
  EXEC_CHECK(direct&&df);
  if(direct&&df){
   union dvm_value dc[2]={{.i=1},{.l=df}};
   bool ctor2=executor_runtime_call(v,"java/util/concurrent/ScheduledThreadPoolExecutor","<init>","(ILjava/util/concurrent/ThreadFactory;)V",direct,dc,2,&out);
   EXEC_CHECK(ctor2);
   struct dvm_object *directo=dvm__obj(v,direct);struct executor_state *directs=directo?directo->data:NULL;
   bool direct_shape=directs&&directs->scheduled&&directs->core==1&&directs->maximum==INT_MAX&&directs->keep_alive_ms==10&&directs->factory==df;
   EXEC_CHECK(direct_shape);
   EXEC_CHECK(executor_runtime_call(v,"java/util/concurrent/ExecutorService","shutdown","()V",direct,NULL,0,&out));
  }
  dvm_ref direct3=dvm_new_object(v,dvm_find_class_or_external(v,"java/util/concurrent/ScheduledThreadPoolExecutor"));
  dvm_ref handler=dvm_new_object(v,dvm_find_class_or_external(v,"java/util/concurrent/ThreadPoolExecutor$DiscardPolicy"));
  EXEC_CHECK(direct3&&handler);
  if(direct3&&handler){
   EXEC_CHECK(executor_runtime_call(v,"java/util/concurrent/ThreadPoolExecutor$DiscardPolicy","<init>","()V",handler,NULL,0,&out));
   union dvm_value dc3[3]={{.i=1},{.l=df},{.l=handler}};
   bool ctor3=executor_runtime_call(v,"java/util/concurrent/ScheduledThreadPoolExecutor","<init>","(ILjava/util/concurrent/ThreadFactory;Ljava/util/concurrent/RejectedExecutionHandler;)V",direct3,dc3,3,&out);
   EXEC_CHECK(ctor3);
   struct dvm_object *d3o=dvm__obj(v,direct3);struct executor_state *d3s=d3o?d3o->data:NULL;
   EXEC_CHECK(d3s&&d3s->handler==handler);
   if(d3s){
    union dvm_value ka[2]={{.j=5},{.l=ms_unit}};
    bool keep_set=executor_runtime_call(v,"java/util/concurrent/ThreadPoolExecutor","setKeepAliveTime","(JLjava/util/concurrent/TimeUnit;)V",direct3,ka,2,&out);
    EXEC_CHECK(keep_set&&d3s->keep_alive_ms==5);
    union dvm_value kz[2]={{.j=0},{.l=ms_unit}};
    bool keep_zero=executor_runtime_call(v,"java/util/concurrent/ThreadPoolExecutor","setKeepAliveTime","(JLjava/util/concurrent/TimeUnit;)V",direct3,kz,2,&out);
    bool keep_iae=exception_is(v,"java/lang/IllegalArgumentException");if(dvm_exception(v))dvm_clear_exception(v);
    EXEC_CHECK(keep_zero&&keep_iae);
    /* 直接构造池上的 execute 必须真实运行；这同时覆盖
     * ScheduledThreadPoolExecutor.execute(Runnable) 的 DEX 引用。 */
    dvm_ref deft=dvm_new_object(v,future_class);
    EXEC_CHECK(deft&&future_ctor(v,deft,&callable_arg,1,"(Ljava/util/concurrent/Callable;)V"));
    if(deft&&!dvm_exception(v)){
     union dvm_value dea={.l=deft};unsigned before=atomic_load(&executor_test_calls);
     EXEC_CHECK(executor_runtime_call(v,"java/util/concurrent/ScheduledThreadPoolExecutor","execute","(Ljava/lang/Runnable;)V",direct3,&dea,1,&out));
     EXEC_CHECK(executor_selftest_wait_calls(v,d3s,before+1,2000));
    }
   }
   EXEC_CHECK(executor_runtime_call(v,"java/util/concurrent/ExecutorService","shutdown","()V",direct3,NULL,0,&out));
  }
  if(sp)EXEC_CHECK(executor_runtime_call(v,"java/util/concurrent/ExecutorService","shutdown","()V",sp,NULL,0,&out));
 }
 {
  /* defaultThreadFactory + newCachedThreadPool 端到端：缓存的默认工厂必须
   * 返回同一个真实对象；缓存池的 worker 必须经该工厂的 newThread 创建并成功
   * 绑定 trampoline，否则 execute/submit 会抛 UnsupportedOperationException。 */
  bool got_default=executor_runtime_call(v,"java/util/concurrent/Executors","defaultThreadFactory","()Ljava/util/concurrent/ThreadFactory;",0,NULL,0,&out);
  dvm_ref df=got_default?out.l:0;EXEC_CHECK(got_default&&df);
  struct dvm_object *dfo=df?dvm__obj(v,df):NULL;
  struct dvm_method *dfm=dfo&&dfo->cls?dvm_find_method(v,dfo->cls,"newThread","(Ljava/lang/Runnable;)Ljava/lang/Thread;"):NULL;
  /* 默认工厂的 newThread 安装失败时 executor_ctor 会回退到内置 worker 路径。
   * 这种 fallback 绝不能让 cached 池用例静默通过，否则真机无法区分「工厂
   * 路径真实工作」与「factory 被正规化成 0」。 */
  EXEC_CHECK(dfm!=NULL);
  bool got_again=df&&executor_runtime_call(v,"java/util/concurrent/Executors","defaultThreadFactory","()Ljava/util/concurrent/ThreadFactory;",0,NULL,0,&out);
  EXEC_CHECK(got_again&&out.l==df);
  union dvm_value cargs[1]={{.l=df}};
  bool got_cached=df&&executor_runtime_call(v,"java/util/concurrent/Executors","newCachedThreadPool","(Ljava/util/concurrent/ThreadFactory;)Ljava/util/concurrent/ExecutorService;",0,cargs,1,&out);
  dvm_ref cp=got_cached?out.l:0;EXEC_CHECK(got_cached&&cp);
  struct dvm_object *cpo=cp?dvm__obj(v,cp):NULL;struct executor_state *cps=cpo?cpo->data:NULL;
  bool cached_factory_kept=cps&&cps->vm==v&&cps->self==cp&&cps->factory==df;
  EXEC_CHECK(cached_factory_kept);
  unsigned cached_before=atomic_load(&executor_test_calls);
  bool cached_sub=cp&&executor_runtime_call(v,"java/util/concurrent/ExecutorService","submit","(Ljava/util/concurrent/Callable;)Ljava/util/concurrent/Future;",cp,&callable_arg,1,&out);
  dvm_ref cached_future=cached_sub?out.l:0;EXEC_CHECK(cached_sub&&cached_future);
  bool cached_got=cached_future&&executor_runtime_call(v,"java/util/concurrent/Future","get","(JLjava/util/concurrent/TimeUnit;)Ljava/lang/Object;",cached_future,wait_args,2,&out);
  bool cached_value=cached_got&&out.l==value->result;EXEC_CHECK(cached_value);
  /* callable fixture 计数只在 worker 内部执行任务时递增，配合 factory 仍被
   * 保留（未被正规化成 0）可证明默认工厂的 newThread 真正创建并绑定了 worker。 */
  bool cached_calls=atomic_load(&executor_test_calls)==cached_before+1;EXEC_CHECK(cached_calls);
  bool cached_worker=cps&&cps->workers&&cps->workers->started;
  EXEC_CHECK(cached_worker);
  EXEC_CHECK(cp&&executor_runtime_call(v,"java/util/concurrent/ExecutorService","shutdown","()V",cp,NULL,0,&out));
  bool cached_term=cp&&executor_runtime_call(v,"java/util/concurrent/ExecutorService","awaitTermination","(JLjava/util/concurrent/TimeUnit;)Z",cp,wait_args,2,&out);EXEC_CHECK(cached_term&&out.i);
  sdk_trace_fprintf(stderr,"EXECUTORS_CACHED_SELFTEST default_factory=1 factory_kept=%d worker_started=%d submit_get=%d callable_calls=%d fallback=%d\n",cached_factory_kept?1:0,cached_worker?1:0,cached_value?1:0,cached_calls?1:0,cached_factory_kept?0:1);
 }
 {
  /* DiscardPolicy 是真实拒绝策略：rejectedExecution 必须静默丢弃任务。
   * 以 shutdown 后的 execute 触发拒绝路径，execute 正常返回、无异常，
   * 任务不执行，FutureTask 保持未完成。构造本身也必须成功（72z 的
   * MISSING_API ThreadPoolExecutor$DiscardPolicy.<init>()V 不再出现）。 */
  dvm_ref policy=dvm_new_object(v,dvm_find_class_or_external(v,"java/util/concurrent/ThreadPoolExecutor$DiscardPolicy"));
  EXEC_CHECK(policy!=0);
  EXEC_CHECK(policy&&executor_runtime_call(v,"java/util/concurrent/ThreadPoolExecutor$DiscardPolicy","<init>","()V",policy,NULL,0,&out));
  dvm_ref dq=dvm_new_object(v,queue_class),dp=dvm_new_object(v,pool_class);
  EXEC_CHECK(dq&&dp);
  EXEC_CHECK(executor_runtime_call(v,"java/util/concurrent/LinkedBlockingDeque","<init>","()V",dq,NULL,0,&out));
  union dvm_value da[6]={{.i=1},{.i=1},{.j=0},{.l=unit},{.l=dq},{.l=policy}};
  EXEC_CHECK(executor_runtime_call(v,"java/util/concurrent/ThreadPoolExecutor","<init>","(IIJLjava/util/concurrent/TimeUnit;Ljava/util/concurrent/BlockingQueue;Ljava/util/concurrent/RejectedExecutionHandler;)V",dp,da,6,&out));
  EXEC_CHECK(executor_runtime_call(v,"java/util/concurrent/ExecutorService","shutdown","()V",dp,NULL,0,&out));
  dvm_ref dt=dvm_new_object(v,future_class);EXEC_CHECK(future_ctor(v,dt,&callable_arg,1,"(Ljava/util/concurrent/Callable;)V"));
  union dvm_value darg={.l=dt};unsigned dbefore=atomic_load(&executor_test_calls);
  EXEC_CHECK(executor_runtime_call(v,"java/util/concurrent/ExecutorService","execute","(Ljava/lang/Runnable;)V",dp,&darg,1,&out));
  EXEC_CHECK(!dvm_exception(v));
  EXEC_CHECK(atomic_load(&executor_test_calls)==dbefore);
  EXEC_CHECK(future_call(v,"isDone","()Z",dt,NULL,0,&out)&&!out.i);
  /* 其余两个被 DEX 引用的策略类同样只在 guest 中构造；验证构造路径被
   * runtime 认领而不是 MISSING_API。 */
  dvm_ref caller=dvm_new_object(v,dvm_find_class_or_external(v,"java/util/concurrent/ThreadPoolExecutor$CallerRunsPolicy"));
  dvm_ref oldest=dvm_new_object(v,dvm_find_class_or_external(v,"java/util/concurrent/ThreadPoolExecutor$DiscardOldestPolicy"));
  EXEC_CHECK(caller&&executor_runtime_call(v,"java/util/concurrent/ThreadPoolExecutor$CallerRunsPolicy","<init>","()V",caller,NULL,0,&out));
  EXEC_CHECK(oldest&&executor_runtime_call(v,"java/util/concurrent/ThreadPoolExecutor$DiscardOldestPolicy","<init>","()V",oldest,NULL,0,&out));
  sdk_trace_fprintf(stderr,"REJECTION_POLICY_SELFTEST discard=discard_only caller_runs=ctor_only discard_oldest=ctor_only\n");
 }
 {
  /* Executors.newFixedThreadPool 的两个真实重载。默认重载必须保留
   * defaultThreadFactory（不是 null），显式重载必须保留传入工厂；两者都
   * 构造 core=max=n、keepAlive=0 的真实 ThreadPoolExecutor + 无界
   * LinkedBlockingQueue。 */
  dvm_ref fdf=default_thread_factory(v);EXEC_CHECK(fdf!=0);
  union dvm_value fa1[1]={{.i=2}};
  bool got_fixed_default=executor_runtime_call(v,"java/util/concurrent/Executors","newFixedThreadPool","(I)Ljava/util/concurrent/ExecutorService;",0,fa1,1,&out);
  dvm_ref fixed_default=got_fixed_default?out.l:0;EXEC_CHECK(got_fixed_default&&fixed_default);
  struct dvm_object *fdo=fixed_default?dvm__obj(v,fixed_default):NULL;struct executor_state *fds=fdo?fdo->data:NULL;
  bool fixed_shape=fds&&fds->vm==v&&fds->self==fixed_default&&fds->core==2&&fds->maximum==2&&fds->keep_alive_ms==0&&!fds->allow_core_timeout;
  EXEC_CHECK(fixed_shape);
  bool fixed_queue=fds&&fds->queue&&fds->queue->kind==QUEUE_LINKED&&fds->queue->maximum==(size_t)INT_MAX;
  EXEC_CHECK(fixed_queue);
  EXEC_CHECK(fds&&fds->factory==fdf);
  union dvm_value fa2[2]={{.i=1},{.l=fdf}};
  bool got_fixed_explicit=executor_runtime_call(v,"java/util/concurrent/Executors","newFixedThreadPool","(ILjava/util/concurrent/ThreadFactory;)Ljava/util/concurrent/ExecutorService;",0,fa2,2,&out);
  dvm_ref fixed_explicit=got_fixed_explicit?out.l:0;EXEC_CHECK(got_fixed_explicit&&fixed_explicit);
  struct dvm_object *feo=fixed_explicit?dvm__obj(v,fixed_explicit):NULL;struct executor_state *fes=feo?feo->data:NULL;
  bool fixed_explicit_shape=fes&&fes->vm==v&&fes->self==fixed_explicit&&fes->core==1&&fes->maximum==1&&fes->keep_alive_ms==0&&fes->factory==fdf;
  EXEC_CHECK(fixed_explicit_shape);
  unsigned fixed_before=atomic_load(&executor_test_calls);
  bool fixed_submit=fixed_explicit&&executor_runtime_call(v,"java/util/concurrent/ExecutorService","submit","(Ljava/util/concurrent/Callable;)Ljava/util/concurrent/Future;",fixed_explicit,&callable_arg,1,&out);
  dvm_ref fixed_future=fixed_submit?out.l:0;EXEC_CHECK(fixed_submit&&fixed_future);
  bool fixed_got=fixed_future&&executor_runtime_call(v,"java/util/concurrent/Future","get","(JLjava/util/concurrent/TimeUnit;)Ljava/lang/Object;",fixed_future,wait_args,2,&out);
  EXEC_CHECK(fixed_got&&out.l==value->result);
  EXEC_CHECK(atomic_load(&executor_test_calls)==fixed_before+1);
  EXEC_CHECK(fes&&fes->workers&&fes->workers->started&&fes->workers->self);
  bool fixed_shutdown=executor_runtime_call(v,"java/util/concurrent/ExecutorService","shutdown","()V",fixed_explicit,NULL,0,&out);
  bool fixed_default_shutdown=executor_runtime_call(v,"java/util/concurrent/ExecutorService","shutdown","()V",fixed_default,NULL,0,&out);
  EXEC_CHECK(fixed_shutdown&&fixed_default_shutdown);

  /* 参数校验必须抛真实且类型正确的异常。 */
  union dvm_value bad_n[1]={{.i=0}};
  bool bad_n_ret=executor_runtime_call(v,"java/util/concurrent/Executors","newFixedThreadPool","(I)Ljava/util/concurrent/ExecutorService;",0,bad_n,1,&out);
  bool bad_n_iae=exception_is(v,"java/lang/IllegalArgumentException");if(dvm_exception(v))dvm_clear_exception(v);
  EXEC_CHECK(bad_n_ret&&bad_n_iae);
  union dvm_value bad_factory[2]={{.i=1},{.l=0}};
  bool bad_factory_ret=executor_runtime_call(v,"java/util/concurrent/Executors","newFixedThreadPool","(ILjava/util/concurrent/ThreadFactory;)Ljava/util/concurrent/ExecutorService;",0,bad_factory,2,&out);
  bool bad_factory_npe=exception_is(v,"java/lang/NullPointerException");if(dvm_exception(v))dvm_clear_exception(v);
  EXEC_CHECK(bad_factory_ret&&bad_factory_npe);

  /* fixed 池 keepAliveTime=0：AOSP 明确拒绝 allowCoreThreadTimeOut(true)。 */
  union dvm_value allow_true={.i=1};
  bool fixed_allow_ret=executor_runtime_call(v,"java/util/concurrent/ThreadPoolExecutor","allowCoreThreadTimeOut","(Z)V",fixed_default,&allow_true,1,&out);
  bool fixed_allow_iae=exception_is(v,"java/lang/IllegalArgumentException");if(dvm_exception(v))dvm_clear_exception(v);
  EXEC_CHECK(fixed_allow_ret&&fixed_allow_iae);

  /* 正 keepAliveTime 的 core 线程：默认 false 时执行完任务仍存活；设置 true
   * 后必须由 worker 自己按 keepAlive 退出，getPoolSize 变 0。等待时调用
   * queue_wait_locked 会释放 GIL，worker 才能重新拿到 GIL 完成退出。 */
  dvm_ref tq=dvm_new_object(v,queue_class),tp=dvm_new_object(v,pool_class);
  EXEC_CHECK(tq&&tp);
  EXEC_CHECK(executor_runtime_call(v,"java/util/concurrent/LinkedBlockingDeque","<init>","()V",tq,NULL,0,&out));
  union dvm_value ms_unit={0};
  bool got_ms=java_basics_static(v,"java/util/concurrent/TimeUnit","MILLISECONDS","Ljava/util/concurrent/TimeUnit;",&ms_unit);
  EXEC_CHECK(got_ms&&ms_unit.l);
  union dvm_value targs[5]={{.i=1},{.i=1},{.j=60},{.l=ms_unit.l},{.l=tq}};
  EXEC_CHECK(executor_runtime_call(v,"java/util/concurrent/ThreadPoolExecutor","<init>","(IIJLjava/util/concurrent/TimeUnit;Ljava/util/concurrent/BlockingQueue;)V",tp,targs,5,&out));
  bool timeout_submit=executor_runtime_call(v,"java/util/concurrent/ExecutorService","submit","(Ljava/util/concurrent/Callable;)Ljava/util/concurrent/Future;",tp,&callable_arg,1,&out);
  dvm_ref timeout_future=timeout_submit?out.l:0;EXEC_CHECK(timeout_submit&&timeout_future);
  bool timeout_got=timeout_future&&executor_runtime_call(v,"java/util/concurrent/Future","get","(JLjava/util/concurrent/TimeUnit;)Ljava/lang/Object;",timeout_future,wait_args,2,&out);
  EXEC_CHECK(timeout_got&&out.l==value->result);
  struct dvm_object *tpo=dvm__obj(v,tp);struct executor_state *tps=tpo?tpo->data:NULL;
  EXEC_CHECK(tps&&tps->live==1&&!tps->allow_core_timeout);
  if(tps){
   pthread_mutex_lock(&tps->queue->lock);uint64_t alive_until=now_ms()+150;
   while(tps->live&&now_ms()<alive_until)queue_wait_locked(v,tps->queue,10);
   bool core_survived=tps->live==1;pthread_mutex_unlock(&tps->queue->lock);
   EXEC_CHECK(core_survived);
   bool allow_ret=executor_runtime_call(v,"java/util/concurrent/ThreadPoolExecutor","allowCoreThreadTimeOut","(Z)V",tp,&allow_true,1,&out);
   EXEC_CHECK(allow_ret&&!dvm_exception(v));
   pthread_mutex_lock(&tps->queue->lock);
   bool core_flag=tps->allow_core_timeout;
   uint64_t gone_until=now_ms()+1000;
   while(tps->live&&now_ms()<gone_until)queue_wait_locked(v,tps->queue,10);
   bool core_gone=tps->live==0;pthread_mutex_unlock(&tps->queue->lock);
   EXEC_CHECK(core_flag);
   EXEC_CHECK(core_gone);
  }
  sdk_trace_fprintf(stderr,"EXECUTORS_FIXED_SELFTEST default_overload=1 explicit_factory=1 core_max_equal=1 keepalive_zero=1 linked_queue=1 allow_core_timeout_reject_zero=1 allow_core_timeout_reap=1\n");
 }
done:
 executor_runtime_shutdown(v,100);
 bool joined=true;for(struct executor_state *p=all_executors;p;p=p->next)if(p->vm==v)for(struct executor_worker *w=p->workers;w;w=w->next)if(w->started&&!w->joined)joined=false;
 EXEC_CHECK(joined);
 /* Synthetic DVM callback fixtures exercise dispatch; they are not SDK classes. */
 struct dvm_class *fc=dvm_find_class_or_external(v,"test/ExecutorFactoryFixture"),*hc=dvm_find_class_or_external(v,"test/ExecutorRejectFixture");
 bool fixtures=fixture_install(fc,"newThread","(Ljava/lang/Runnable;)Ljava/lang/Thread;",fixture_factory,'L')&&fixture_install(hc,"rejectedExecution","(Ljava/lang/Runnable;Ljava/util/concurrent/ThreadPoolExecutor;)V",fixture_reject,'V');
 EXEC_CHECK(fixtures);
 if(fixtures){
  dvm_ref fq=dvm_new_object(v,queue_class),fp=dvm_new_object(v,pool_class),f=dvm_new_object(v,fc),h=dvm_new_object(v,hc),ft=dvm_new_object(v,future_class);
  EXEC_CHECK(executor_runtime_call(v,"java/util/concurrent/LinkedBlockingDeque","<init>","()V",fq,NULL,0,&out));
  union dvm_value fa[7]={{.i=1},{.i=1},{.j=0},{.l=unit},{.l=fq},{.l=f},{.l=h}};
  EXEC_CHECK(executor_runtime_call(v,"java/util/concurrent/ThreadPoolExecutor","<init>","(IIJLjava/util/concurrent/TimeUnit;Ljava/util/concurrent/BlockingQueue;Ljava/util/concurrent/ThreadFactory;Ljava/util/concurrent/RejectedExecutionHandler;)V",fp,fa,7,&out));
  EXEC_CHECK(executor_runtime_call(v,"java/util/concurrent/FutureTask","<init>","(Ljava/util/concurrent/Callable;)V",ft,&callable_arg,1,&out));
  union dvm_value task={.l=ft};fixture_thread=0;atomic_store(&fixture_rejections,0);
  EXEC_CHECK(executor_runtime_call(v,"java/util/concurrent/ExecutorService","execute","(Ljava/lang/Runnable;)V",fp,&task,1,&out)&&fixture_thread);
  bool got=executor_runtime_call(v,"java/util/concurrent/Future","get","(JLjava/util/concurrent/TimeUnit;)Ljava/lang/Object;",ft,wait_args,2,&out);EXEC_CHECK(got&&out.l==value->result);
  EXEC_CHECK(executor_runtime_call(v,"java/util/concurrent/ExecutorService","shutdown","()V",fp,NULL,0,&out));
  EXEC_CHECK(executor_runtime_call(v,"java/util/concurrent/ExecutorService","execute","(Ljava/lang/Runnable;)V",fp,&task,1,&out)&&atomic_load(&fixture_rejections)==1);
  executor_runtime_shutdown(v,100);
 }

 if(!ok||dvm_exception(v)){sdk_trace_fprintf(stderr,"EXECUTOR_SELFTEST FAIL exception=%d\n",dvm_exception(v)!=0);dvm_clear_exception(v);}
 else sdk_trace_fprintf(stderr,"EXECUTOR_SELFTEST PASS real_pthread=1 independent_queue=1 future_completion=1 shutdown_join=1 scheduled_delay=1 scheduled_factory=1 scheduled_cancel=1\n");
#undef EXEC_CHECK
 return ok;
}
