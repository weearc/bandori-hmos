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
 bool shutdown;
 struct executor_worker *workers;
 struct executor_state *next;
};
struct future_state {
 dvm_ref callable,runnable,result,exception;
 int status;
 pthread_mutex_t lock;
 pthread_cond_t changed;
};
struct executor_test_value { dvm_ref result; };
static struct queue_state *all_queues;
static struct executor_state *all_executors;
static void *executor_worker_main(void *arg);
static bool executor_worker_run(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out);
static bool worker_trampoline(struct dvm *v,struct executor_worker *w){
 struct dvm_class *c=dvm_find_class_or_external(v,"java/util/concurrent/ExecutorRuntimeWorker");
 if(!c)return false;
 if(!c->nmethods){struct dvm_method *m=calloc(1,sizeof *m);if(!m)return false;m->cls=c;m->name="run";m->sig=strdup("()V");m->builtin=executor_worker_run;m->ret_kind='V';c->methods=m;c->nmethods=1;}
 w->trampoline=dvm_new_object(v,c);struct dvm_object *o=dvm__obj(v,w->trampoline);if(!o)return false;o->data=w;return true;
}
static _Atomic unsigned executor_test_calls;
static dvm_ref fixture_thread;
static _Atomic unsigned fixture_rejections;
static bool fixture_factory(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 (void)self;if(n!=1)return false;struct dvm_class *c=dvm_find_class_or_external(v,"java/lang/Thread");out->l=dvm_new_object(v,c);fixture_thread=out->l;union dvm_value ignored={0};return thread_runtime_call(v,"java/lang/Thread","<init>","(Ljava/lang/Runnable;)V",out->l,a,1,&ignored);
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

static uint64_t now_ms(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000u+(uint64_t)t.tv_nsec/1000000u;}
static struct timespec deadline_ms(uint64_t ms){struct timespec t;clock_gettime(CLOCK_REALTIME,&t);t.tv_sec+=(time_t)(ms/1000u);t.tv_nsec+=(long)(ms%1000u)*1000000L;if(t.tv_nsec>=1000000000L){++t.tv_sec;t.tv_nsec-=1000000000L;}return t;}
static uint64_t deadline_after(uint64_t duration){uint64_t now=now_ms();return duration>UINT64_MAX-now?UINT64_MAX:now+duration;}
static uint64_t duration_ms(uint64_t count,uint64_t nanos){__uint128_t total=(__uint128_t)count*nanos+999999u;total/=1000000u;return total>UINT64_MAX?UINT64_MAX:(uint64_t)total;}
static bool is_name(struct dvm_object *o,const char *name){return o&&o->cls&&o->cls->name&&!strcmp(o->cls->name,name);}
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

static struct executor_state *executor_of(struct dvm *v,dvm_ref self){struct dvm_object *o=dvm__obj(v,self);struct executor_state *p=o&&is_name(o,"java/util/concurrent/ThreadPoolExecutor")?o->data:NULL;return p&&p->vm==v&&p->self==self?p:NULL;}
static struct future_state *future_of(struct dvm *v,dvm_ref self){struct dvm_object *o=dvm__obj(v,self);return o&&is_name(o,"java/util/concurrent/FutureTask")?o->data:NULL;}
static bool invoke_runnable(struct dvm *v,dvm_ref runnable){
 struct dvm_object *o=dvm__obj(v,runnable);if(!o){dvm__throw(v,"java/lang/NullPointerException","executor task");return false;}
 if(is_name(o,"java/util/concurrent/FutureTask")){union dvm_value ignored={0};return executor_runtime_call(v,"java/util/concurrent/FutureTask","run","()V",runnable,NULL,0,&ignored);}
 struct dvm_method *m=dvm_find_method(v,o->cls,"run","()V");if(!m){sdk_trace_missing_api(o->cls?o->cls->name:"?","run","()V");dvm__throw(v,"java/lang/NoSuchMethodError","Runnable.run");return false;}
 union dvm_value out={0};return dvm_call(v,m,runnable,NULL,0,&out);
}
static struct executor_worker *start_worker(struct executor_state *p,dvm_ref first_task,bool require_space){
 struct dvm *v=p->vm;struct executor_worker *w=calloc(1,sizeof *w);if(!w){dvm__throw(v,"java/lang/OutOfMemoryError","executor worker state");return NULL;}
 if(p->factory){
  if(!worker_trampoline(v,w)){free(w);dvm__throw(v,"java/lang/OutOfMemoryError","executor worker trampoline");return NULL;}
  struct dvm_object *fo=dvm__obj(v,p->factory);struct dvm_method *fm=fo?dvm_find_method(v,fo->cls,"newThread","(Ljava/lang/Runnable;)Ljava/lang/Thread;"):NULL;
  if(!fm){free(w);dvm__throw(v,"java/lang/NoSuchMethodError","ThreadFactory.newThread");return NULL;}
  union dvm_value arg={.l=w->trampoline},ret={0};if(!dvm_call(v,fm,p->factory,&arg,1,&ret)||dvm_exception(v)){free(w);return NULL;}
  if(!ret.l){free(w);return NULL;}w->self=ret.l;
 }else{struct dvm_class *thread_class=dvm_find_class_or_external(v,"java/lang/Thread");w->self=thread_class?dvm_new_object(v,thread_class):0;}if(!w->self){free(w);dvm__throw(v,"java/lang/OutOfMemoryError","executor Thread");return NULL;}
 w->pool=p;w->first_task=first_task;if(!w->trampoline&&!worker_trampoline(v,w)){free(w);dvm__throw(v,"java/lang/OutOfMemoryError","worker trampoline");return NULL;}
 pthread_mutex_lock(&p->queue->lock);
 if(p->shutdown||p->live>=p->maximum||(require_space&&p->live&&p->live>=p->core)){pthread_mutex_unlock(&p->queue->lock);free(w);return NULL;}
 if(p->factory&&!thread_runtime_executor_bind(v,w->self,w->trampoline)){pthread_mutex_unlock(&p->queue->lock);free(w);return NULL;}
 w->alive=true;w->started=true;w->next=p->workers;p->workers=w;++p->live;
 int rc=pthread_create(&w->thread,NULL,executor_worker_main,w);
 if(rc){if(p->factory)thread_runtime_executor_finished(v,w->self);p->workers=w->next;--p->live;w->alive=w->started=false;pthread_mutex_unlock(&p->queue->lock);free(w);dvm__throw(v,"java/lang/OutOfMemoryError","executor pthread_create error %d",rc);return NULL;}
 pthread_cond_broadcast(&p->queue->changed);pthread_mutex_unlock(&p->queue->lock);return w;
}
static bool executor_worker_run(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 (void)a;(void)n;(void)out;struct dvm_object *o=dvm__obj(v,self);struct executor_worker *w=o?o->data:NULL;if(!w)return false;struct executor_state *p=w->pool;struct queue_state *q=p->queue;
 dvm_ref first=w->first_task;w->first_task=0;
 for(;;){dvm_ref task=first;first=0;pthread_mutex_lock(&q->lock);
  if(!task){for(;;){task=queue_pop_locked(q,true);if(task)break;if(p->shutdown||q->closed||dvm_is_stopping(v))break;
   if(p->keep_alive_ms&&p->live>p->core){bool timeout=queue_wait_locked(v,q,p->keep_alive_ms);if(timeout&&!queue_count_locked(q)){--p->live;w->alive=false;pthread_cond_broadcast(&q->changed);pthread_mutex_unlock(&q->lock);return true;}}
   else {if(q->kind==QUEUE_SYNC)++q->waiting_takers;queue_wait_locked(v,q,50);if(q->kind==QUEUE_SYNC&&q->waiting_takers)--q->waiting_takers;}
  }}
  if(!task){--p->live;w->alive=false;pthread_cond_broadcast(&q->changed);pthread_mutex_unlock(&q->lock);break;}
  ++p->active;pthread_mutex_unlock(&q->lock);
  dvm_set_step_limit(v,100000);bool ok=!dvm_is_stopping(v)&&invoke_runnable(v,task);bool failed=!ok||dvm_exception(v);
  if(failed){sdk_trace_async_failure(v,"executor");dvm_clear_exception(v);}
  pthread_mutex_lock(&q->lock);if(p->active)--p->active;++p->executed;pthread_cond_broadcast(&q->changed);pthread_mutex_unlock(&q->lock);
 }
 return true;
}
static void *executor_worker_main(void *arg){
 struct executor_worker *w=arg;struct dvm *v=w->pool->vm;
 dvm__mark_bytecode_thread();dvm_gil_acquire(v);dvm__tstate_reset(v);v->cur_thread=w->self;v->quiet_uncaught=true;
 if(w->trampoline){union dvm_value out={0};struct dvm_method *m=dvm_find_method(v,dvm_object_class(v,w->trampoline),"run","()V");if(m)dvm_call(v,m,w->trampoline,NULL,0,&out);}
 else {struct dvm_object fake={0};(void)fake; /* default worker has a trampoline too */ union dvm_value out={0};executor_worker_run(v,w->trampoline,NULL,0,&out);}
 if(w->pool->factory)thread_runtime_executor_finished(v,w->self);dvm_gil_release(v);return NULL;
}
static bool executor_ctor(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,const char *sig){
 if(n<5||n>7)return false;int core=a[0].i,maximum=a[1].i;int64_t keep=a[2].j;
 if(core<0||maximum<=0||core>maximum||keep<0||!a[3].l||!a[4].l){dvm__throw(v,"java/lang/IllegalArgumentException","invalid ThreadPoolExecutor configuration");return true;}
 dvm_ref handler=0,factory=0;
 if(n>=6){
  if(!a[5].l||(n==7&&!a[6].l)){dvm__throw(v,"java/lang/NullPointerException","ThreadPoolExecutor factory or rejection handler");return true;}
  if(n==7){factory=a[5].l;handler=a[6].l;}
  else if(strstr(sig,"Ljava/util/concurrent/ThreadFactory;"))factory=a[5].l;
  else if(strstr(sig,"Ljava/util/concurrent/RejectedExecutionHandler;"))handler=a[5].l;
  else return false;
 }
 struct queue_state *q=queue_of(v,a[4].l);if(!q){dvm__throw(v,"java/lang/UnsupportedOperationException","ThreadPoolExecutor requires a supported queue object");return true;}
 uint64_t nanos=0;if(!unit_millis(v,a[3].l,&nanos))return true;if((uint64_t)keep>UINT64_MAX/nanos){dvm__throw(v,"java/lang/IllegalArgumentException","keep-alive overflow");return true;}
 struct dvm_object *o=dvm__obj(v,self);if(!o){dvm__throw(v,"java/lang/NullPointerException","ThreadPoolExecutor receiver");return true;}if(o->data){dvm__throw(v,"java/lang/IllegalStateException","ThreadPoolExecutor already initialized");return true;}
 struct executor_state *p=calloc(1,sizeof *p);if(!p){dvm__throw(v,"java/lang/OutOfMemoryError","ThreadPoolExecutor state");return true;}
 p->vm=v;p->self=self;p->handler=handler;p->factory=factory;p->queue_ref=a[4].l;p->queue=q;p->core=(unsigned)core;p->maximum=(unsigned)maximum;p->keep_alive_ms=((uint64_t)keep*nanos)/1000000u;
 p->next=all_executors;all_executors=p;o->data=p;o->data_destroy=executor_destroy;return true;
}
static bool executor_reject(struct dvm *v,struct executor_state *p,dvm_ref task,const char *reason){
 if(!p->handler){dvm__throw(v,"java/util/concurrent/RejectedExecutionException","%s",reason);return true;}
 struct dvm_object *o=dvm__obj(v,p->handler);
 struct dvm_method *m=o?dvm_find_method(v,o->cls,"rejectedExecution","(Ljava/lang/Runnable;Ljava/util/concurrent/ThreadPoolExecutor;)V"):NULL;
 if(!m){dvm__throw(v,"java/lang/NoSuchMethodError","RejectedExecutionHandler.rejectedExecution");return true;}
 union dvm_value args[2]={{.l=task},{.l=p->self}},out={0};
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
 pthread_mutex_lock(&f->lock);f->result=result;f->exception=exception;f->status=success?FUTURE_DONE:FUTURE_FAILED;pthread_cond_broadcast(&f->changed);pthread_mutex_unlock(&f->lock);return true;
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
  pthread_mutex_lock(&f->lock);if(f->status==FUTURE_NEW){f->status=FUTURE_CANCELLED;pthread_cond_broadcast(&f->changed);out->i=1;}else if(f->status==FUTURE_RUNNING&&a[0].i){pthread_mutex_unlock(&f->lock);dvm__throw(v,"java/lang/UnsupportedOperationException","interrupting a running FutureTask is not supported");return true;}pthread_mutex_unlock(&f->lock);return true;
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

static bool executor_call(struct dvm *v,const char *c,const char *m,const char *sig,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 if(!strcmp(c,"java/util/concurrent/ThreadPoolExecutor")&& !strcmp(m,"<init>"))return executor_ctor(v,self,a,n,sig);
 struct executor_state *p=executor_of(v,self);if(!p)return false;
 if(!strcmp(m,"execute")&&!strcmp(sig,"(Ljava/lang/Runnable;)V"))return executor_execute(v,p,a[0].l);
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
 if(!strcmp(m,"prestartCoreThread")&&!strcmp(sig,"()Z")){pthread_mutex_lock(&p->queue->lock);bool need=!p->shutdown&&p->live<p->core;pthread_mutex_unlock(&p->queue->lock);if(!need){out->i=0;return true;}out->i=start_worker(p,0,true)!=NULL;return true;}
 if(!strcmp(m,"prestartAllCoreThreads")&&!strcmp(sig,"()I")){while(1){pthread_mutex_lock(&p->queue->lock);bool need=!p->shutdown&&p->live<p->core;pthread_mutex_unlock(&p->queue->lock);if(!need)break;if(!start_worker(p,0,true))break;++out->i;}return true;}
 if(!strcmp(m,"remove")&&!strcmp(sig,"(Ljava/lang/Runnable;)Z"))return queue_contains(v,p->queue,a[0].l,true,false,out);
 return false;
}

bool executor_runtime_call(struct dvm *v,const char *c,const char *m,const char *sig,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 memset(out,0,sizeof *out);
 if(!strcmp(c,"java/util/concurrent/LinkedBlockingDeque")||!strcmp(c,"java/util/concurrent/LinkedBlockingQueue")||!strcmp(c,"java/util/concurrent/ArrayBlockingQueue")||!strcmp(c,"java/util/concurrent/SynchronousQueue")||queue_of(v,self))return queue_call(v,c,m,sig,self,a,n,out);
 if(!strcmp(c,"java/util/concurrent/FutureTask")||future_of(v,self))return future_call(v,m,sig,self,a,n,out);
 if(!strcmp(c,"java/util/concurrent/ThreadPoolExecutor")||
    !strcmp(c,"java/util/concurrent/ExecutorService")||
    !strcmp(c,"java/util/concurrent/Executor"))return executor_call(v,c,m,sig,self,a,n,out);
 return false;
}

void executor_runtime_shutdown(struct dvm *v,unsigned grace){
 bool force=false;
 for(struct executor_state *p=all_executors;p;p=p->next){if(p->vm!=v)continue;uint64_t end=now_ms()+grace;struct queue_state *q=p->queue;pthread_mutex_lock(&q->lock);
  while((p->active||queue_count_locked(q))&&now_ms()<end){uint64_t wait=end-now_ms();if(wait>20)wait=20;queue_wait_locked(v,q,wait);}
  p->shutdown=true;bool busy=p->active!=0;if(queue_count_locked(q)){p->cancelled+=(unsigned)queue_count_locked(q);memset(q->items,0,q->allocated*sizeof *q->items);q->size=q->head=0;}if(busy)force=true;pthread_cond_broadcast(&q->changed);pthread_mutex_unlock(&q->lock);
 }
 for(struct queue_state *q=all_queues;q;q=q->next)if(q->vm==v){pthread_mutex_lock(&q->lock);q->closed=true;pthread_cond_broadcast(&q->changed);pthread_mutex_unlock(&q->lock);}
 if(force)dvm_request_stop(v);
 for(struct executor_state *p=all_executors;p;p=p->next){if(p->vm!=v)continue;for(struct executor_worker *w=p->workers;w;w=w->next){if(!w->started||w->joined)continue;thread_runtime_join_host(v,w->thread);w->joined=true;}}
 for(struct executor_state *p=all_executors;p;p=p->next)if(p->vm==v){pthread_mutex_lock(&p->queue->lock);sdk_trace_fprintf(stderr,"EXECUTOR_SHUTDOWN joined=1 executed=%u cancelled=%u busy_at_deadline=%d\n",p->executed,p->cancelled,p->active!=0);pthread_mutex_unlock(&p->queue->lock);}
}

bool executor_runtime_selftest(struct dvm *v,unsigned *checks){
 bool ok=true;union dvm_value out={0},args[5]={{.i=1},{.i=1},{.j=0},{0},{0}};
#define EXEC_CHECK(expr) do{bool pass=!!(expr);if(dvm_exception(v)){pass=false;dvm_clear_exception(v);}if(checks)++*checks;if(!pass){ok=false;sdk_trace_fprintf(stderr,"EXECUTOR_SELFTEST_FAIL line=%d expression=%s\n",__LINE__,#expr);}}while(0)
 struct dvm_class *queue_class=dvm_find_class_or_external(v,"java/util/concurrent/LinkedBlockingDeque");
 struct dvm_class *pool_class=dvm_find_class_or_external(v,"java/util/concurrent/ThreadPoolExecutor");
 struct dvm_class *future_class=dvm_find_class_or_external(v,"java/util/concurrent/FutureTask");
 struct dvm_class *callable_class=dvm_find_class_or_external(v,"java/util/concurrent/ExecutorRuntimeSelftestCallable");
 dvm_ref queue=dvm_new_object(v,queue_class),pool=dvm_new_object(v,pool_class),future=dvm_new_object(v,future_class);
 dvm_ref callable=dvm_new_object(v,callable_class),unit=0;
 if(!queue||!pool||!future||!callable||!install_executor_test_callable(v,callable_class)){ok=false;goto done;}
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
 EXEC_CHECK(executor_runtime_call(v,"java/util/concurrent/ExecutorService","shutdown","()V",pool,NULL,0,&out));
 bool terminated=executor_runtime_call(v,"java/util/concurrent/ExecutorService","awaitTermination","(JLjava/util/concurrent/TimeUnit;)Z",pool,wait_args,2,&out);EXEC_CHECK(terminated&&out.i);
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
 else sdk_trace_fprintf(stderr,"EXECUTOR_SELFTEST PASS checks=12 real_pthread=1 independent_queue=1 future_completion=1 shutdown_join=1\n");
#undef EXEC_CHECK
 return ok;
}
