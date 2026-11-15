#include "latch_runtime.h"
#include "java_basics.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>
#include <stdatomic.h>
extern int sdk_trace_fprintf(FILE*,const char*,...);

#define LATCH_POLL_MS 50
static _Atomic(struct dvm *) latch_stop_vm;
void latch_runtime_stop(struct dvm *v){atomic_store(&latch_stop_vm,v);}
void latch_runtime_resume(void){atomic_store(&latch_stop_vm,NULL);}

typedef struct {pthread_mutex_t mutex;pthread_cond_t cond;int count;
                int waiting; /* selftest observability; protected by mutex */} latch;
/* The selftest has a local dvm_ref named latch; use a second typedef so the
 * storage type is not shadowed in that scope. */
typedef latch latch_state;
static void latch_free(void *p){
 latch *l=p;if(!l)return;pthread_cond_destroy(&l->cond);pthread_mutex_destroy(&l->mutex);free(l);
}
static latch *latch_of(struct dvm *v,dvm_ref self){
 struct dvm_object *o=dvm__obj(v,self);return o&&o->data_destroy==latch_free?o->data:NULL;
}
static struct timespec deadline_ms(uint64_t ms){
 struct timespec t;clock_gettime(CLOCK_REALTIME,&t);
 t.tv_sec+=(time_t)(ms/1000u);t.tv_nsec+=(long)(ms%1000u)*1000000L;
 if(t.tv_nsec>=1000000000L){++t.tv_sec;t.tv_nsec-=1000000000L;}return t;
}
static uint64_t monotonic_ms(void){
 struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);
 return (uint64_t)t.tv_sec*1000u+(uint64_t)t.tv_nsec/1000000u;
}
/* Real VM teardown (dvm_is_stopping) and the selftest-only hook are distinct
 * sources.  The hook exists so a selftest can exercise the bounded wake/exit
 * path without calling dvm_request_stop(), which would tear down the VM that
 * is running the selftest. */
static bool latch_stop_requested(struct dvm *v){
 return dvm_is_stopping(v)||atomic_load_explicit(&latch_stop_vm,memory_order_acquire)==v;
}
/* Waits are bounded: every iteration wakes at least every LATCH_POLL_MS so a
 * VM stop (or the test-only latch_runtime_stop hook) cannot leave a worker
 * blocked forever, even when no countDown will ever arrive. A clean countDown
 * still wakes the waiter immediately; the poll only bounds the stop latency.
 * Timeouts <= 0 return without waiting: count==0 succeeds, count>0 fails. */
static bool await_latch(struct dvm *v,latch *l,long timeout_ms,union dvm_value *out){
 bool timed=timeout_ms>=0;uint64_t end=0;
 if(timed)end=monotonic_ms()+(uint64_t)timeout_ms;
 pthread_mutex_lock(&l->mutex);
 while(l->count>0){
  if(latch_stop_requested(v)){pthread_mutex_unlock(&l->mutex);dvm__throw(v,"java/lang/InterruptedException","latch wait during VM shutdown");return true;}
  uint64_t slice=LATCH_POLL_MS;
  if(timed){
   uint64_t now_ms=monotonic_ms();
   if(now_ms>=end){pthread_mutex_unlock(&l->mutex);if(out)out->i=0;return true;}
   if(end-now_ms<slice)slice=end-now_ms;
  }
  unsigned depth=dvm_gil_unlock_all(v);
  struct timespec until=deadline_ms(slice?slice:1);
  ++l->waiting;
  pthread_cond_timedwait(&l->cond,&l->mutex,&until);
  --l->waiting;
  pthread_mutex_unlock(&l->mutex);
  dvm_gil_relock(v,depth);
  pthread_mutex_lock(&l->mutex);
 }
 pthread_mutex_unlock(&l->mutex);
 if(out)out->i=1;
 return true;
}
#define IS(a,b) (!strcmp(m,a)&&!strcmp(sig,b))
bool latch_runtime_call(struct dvm *v,const char *c,const char *m,const char *sig,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 (void)n;
 if(strcmp(c,"java/util/concurrent/CountDownLatch"))return false;
 struct dvm_object *o=self?dvm__obj(v,self):NULL;latch *l=o&&o->data_destroy==latch_free?o->data:NULL;
 if(IS("<init>","(I)V")){
  if(l){dvm__throw(v,"java/lang/IllegalStateException","CountDownLatch already initialized");return true;}
  if(a[0].i<0){dvm__throw(v,"java/lang/IllegalArgumentException","count < 0");return true;}
  l=calloc(1,sizeof *l);if(!l){dvm__throw(v,"java/lang/OutOfMemoryError","CountDownLatch");return true;}
  if(pthread_mutex_init(&l->mutex,NULL)||pthread_cond_init(&l->cond,NULL)){free(l);dvm__throw(v,"java/lang/IllegalStateException","CountDownLatch sync");return true;}
  l->count=a[0].i;o->data=l;o->data_destroy=latch_free;return true;
 }
 if(!l){dvm__throw(v,"java/lang/IllegalStateException","CountDownLatch constructor not called");return true;}
 if(IS("countDown","()V")){
  pthread_mutex_lock(&l->mutex);if(l->count>0&&--l->count==0)pthread_cond_broadcast(&l->cond);pthread_mutex_unlock(&l->mutex);return true;
 }
 if(IS("getCount","()J")){pthread_mutex_lock(&l->mutex);out->j=l->count;pthread_mutex_unlock(&l->mutex);return true;}
 if(IS("await","()V"))return await_latch(v,l,-1,NULL);
 if(IS("await","(JLjava/util/concurrent/TimeUnit;)Z")){
  uint64_t nanos=0;
  if(!java_basics_timeunit_nanos(v,a[1].l,&nanos)){dvm__throw(v,"java/lang/IllegalArgumentException","TimeUnit");return true;}
  /* Java's CountDownLatch.await(timeout, unit) passes through
   * TimeUnit.toNanos(), which saturates at Long.MAX_VALUE, then treats every
   * non-positive nanos value as "try once, do not wait".  Negative and zero
   * timeouts therefore must not throw. */
  if(a[0].j<=0)return await_latch(v,l,0,out);
  uint64_t timeout=(uint64_t)a[0].j,ms;
  if(timeout>((uint64_t)INT64_MAX)/nanos)ms=((uint64_t)INT64_MAX)/1000000u+1u;
  else{uint64_t total=timeout*nanos;ms=total/1000000u+(total%1000000u!=0);}
  if(!ms)ms=1;
  return await_latch(v,l,(long)ms,out);
 }
 return false;
}
typedef struct {latch *l;int delay_ms;} wake_arg;
static void *wake_latch(void *p){
 wake_arg *w=p;struct timespec t={.tv_sec=w->delay_ms/1000,.tv_nsec=(long)(w->delay_ms%1000)*1000000L};nanosleep(&t,NULL);
 pthread_mutex_lock(&w->l->mutex);if(w->l->count>0){--w->l->count;pthread_cond_broadcast(&w->l->cond);}pthread_mutex_unlock(&w->l->mutex);return NULL;
}

static dvm_ref latch_new_count(struct dvm *v,int count){
 dvm_ref r=dvm_new_object(v,dvm_find_class_or_external(v,"java/util/concurrent/CountDownLatch"));
 union dvm_value a={.i=count},out={0};
 if(!r||!latch_runtime_call(v,"java/util/concurrent/CountDownLatch","<init>","(I)V",r,&a,1,&out))return 0;
 return r;
}
static bool latch_await_timed(struct dvm *v,dvm_ref l,dvm_ref unit,long long ms,union dvm_value *out){
 union dvm_value a[2]={{.j=ms},{.l=unit}};
 return latch_runtime_call(v,"java/util/concurrent/CountDownLatch","await","(JLjava/util/concurrent/TimeUnit;)Z",l,a,2,out)&&!dvm_exception(v);
}
typedef struct {
 struct dvm *v;dvm_ref ref;
 pthread_mutex_t done_mutex;pthread_cond_t done_cond;
 bool done,call_ret,saw_exception;
} latch_block_task;
static void *latch_block_worker(void *p){
 latch_block_task *t=p;
 dvm_gil_acquire(t->v);
 union dvm_value out={0};
 bool call_ret=latch_runtime_call(t->v,"java/util/concurrent/CountDownLatch","await","()V",t->ref,NULL,0,&out);
 bool saw_exception=dvm_exception(t->v)!=0;
 dvm_gil_release(t->v);
 pthread_mutex_lock(&t->done_mutex);
 t->call_ret=call_ret;t->saw_exception=saw_exception;t->done=true;
 pthread_cond_broadcast(&t->done_cond);
 pthread_mutex_unlock(&t->done_mutex);
 return NULL;
}
/* Bounded join: wait for the worker's completion signal rather than calling
 * pthread_join() with no time limit. */
static bool latch_wait_done(struct dvm *v,latch_block_task *t,int ms){
 unsigned depth=dvm_gil_unlock_all(v);
 struct timespec until=deadline_ms((uint64_t)ms);
 pthread_mutex_lock(&t->done_mutex);
 while(!t->done){
  if(pthread_cond_timedwait(&t->done_cond,&t->done_mutex,&until)!=0)break;
 }
 bool done=t->done;
 pthread_mutex_unlock(&t->done_mutex);
 dvm_gil_relock(v,depth);
 return done;
}
typedef struct {latch *l;struct dvm *v;int wait_ms;} latch_stop_probe;
/* The stopper only flips the selftest hook.  It deliberately does not call
 * dvm_request_stop(), so the real VM teardown path stays distinguishable. */
static void *latch_stop_worker(void *p){
 latch_stop_probe *s=p;
 uint64_t end=monotonic_ms()+(uint64_t)s->wait_ms;
 bool saw_wait=false;
 while(monotonic_ms()<end){
  pthread_mutex_lock(&s->l->mutex);
  saw_wait=s->l->waiting>0;
  pthread_mutex_unlock(&s->l->mutex);
  if(saw_wait)break;
  struct timespec pause={.tv_sec=0,.tv_nsec=1000000L};
  nanosleep(&pause,NULL);
 }
 if(saw_wait)latch_runtime_stop(s->v);
 return (void *)(uintptr_t)(saw_wait?1u:0u);
}

bool latch_runtime_selftest(struct dvm *v){
 union dvm_value out={0};
 dvm_ref latch=dvm_new_object(v,dvm_find_class_or_external(v,"java/util/concurrent/CountDownLatch"));
 if(!latch)return false;
 union dvm_value two={.i=2};
 if(!latch_runtime_call(v,"java/util/concurrent/CountDownLatch","<init>","(I)V",latch,&two,1,&out))return false;
 if(!latch_runtime_call(v,"java/util/concurrent/CountDownLatch","getCount","()J",latch,NULL,0,&out)||out.j!=2)return false;
 if(!latch_runtime_call(v,"java/util/concurrent/CountDownLatch","countDown","()V",latch,NULL,0,&out))return false;
 if(!latch_runtime_call(v,"java/util/concurrent/CountDownLatch","getCount","()J",latch,NULL,0,&out)||out.j!=1)return false;
 if(!latch_runtime_call(v,"java/util/concurrent/CountDownLatch","countDown","()V",latch,NULL,0,&out))return false;
 if(!latch_runtime_call(v,"java/util/concurrent/CountDownLatch","await","()V",latch,NULL,0,&out)||dvm_exception(v))return false;
 if(!latch_runtime_call(v,"java/util/concurrent/CountDownLatch","getCount","()J",latch,NULL,0,&out)||out.j)return false;
 /* Positive count: a second thread must be able to countDown while await
  * is actually blocked (the old selftest only awaited a zero latch). */
 dvm_ref blocked=latch_new_count(v,1);
 latch_state *l=latch_of(v,blocked);if(!l)return false;
 wake_arg w={.l=l,.delay_ms=100};pthread_t worker;
 if(pthread_create(&worker,NULL,wake_latch,&w)!=0)return false;
 bool awaited=latch_runtime_call(v,"java/util/concurrent/CountDownLatch","await","()V",blocked,NULL,0,&out)&&!dvm_exception(v);
 pthread_join(worker,NULL);
 if(!awaited||!latch_runtime_call(v,"java/util/concurrent/CountDownLatch","getCount","()J",blocked,NULL,0,&out)||out.j)return false;

 union dvm_value unit={0};
 if(!java_basics_static(v,"java/util/concurrent/TimeUnit","MILLISECONDS","Ljava/util/concurrent/TimeUnit;",&unit)||!unit.l)return false;
 /* Timed await: count 0 succeeds immediately for negative, zero, positive
  * and overflowing timeouts; no exception for non-positive time. */
 dvm_ref zero=latch_new_count(v,0);
 if(!zero)return false;
 if(!latch_await_timed(v,zero,unit.l,-1,&out)||out.i!=1||dvm_exception(v))return false;
 if(!latch_await_timed(v,zero,unit.l,0,&out)||out.i!=1||dvm_exception(v))return false;
 if(!latch_await_timed(v,zero,unit.l,10,&out)||out.i!=1||dvm_exception(v))return false;
 if(!latch_await_timed(v,zero,unit.l,INT64_MAX,&out)||out.i!=1||dvm_exception(v))return false;
 /* Positive count: negative and zero must return false without waiting and
  * without an IllegalArgumentException. */
 dvm_ref timed=latch_new_count(v,1);
 if(!timed)return false;
 if(!latch_await_timed(v,timed,unit.l,-1,&out)||out.i||dvm_exception(v))return false;
 if(!latch_runtime_call(v,"java/util/concurrent/CountDownLatch","getCount","()J",timed,NULL,0,&out)||out.j!=1)return false;
 if(!latch_await_timed(v,timed,unit.l,0,&out)||out.i||dvm_exception(v))return false;
 if(!latch_runtime_call(v,"java/util/concurrent/CountDownLatch","getCount","()J",timed,NULL,0,&out)||out.j!=1)return false;
 /* Positive time leaves the latch counted and returns false on timeout. */
 if(!latch_await_timed(v,timed,unit.l,10,&out)||out.i||dvm_exception(v))return false;
 if(!latch_runtime_call(v,"java/util/concurrent/CountDownLatch","getCount","()J",timed,NULL,0,&out)||out.j!=1)return false;
 /* Overflowing the nanosecond product must saturate (like TimeUnit.toNanos),
  * not turn into a negative/immediate timeout.  A countDown after 100ms
  * proves the wait is still live. */
 dvm_ref overflow=latch_new_count(v,1);
 latch_state *ol=latch_of(v,overflow);if(!ol)return false;
 wake_arg ow={.l=ol,.delay_ms=100};pthread_t overflow_worker;
 if(pthread_create(&overflow_worker,NULL,wake_latch,&ow)!=0)return false;
 bool overflow_ok=latch_await_timed(v,overflow,unit.l,INT64_MAX,&out)&&out.i&&!dvm_exception(v);
 pthread_join(overflow_worker,NULL);
 if(!overflow_ok||!latch_runtime_call(v,"java/util/concurrent/CountDownLatch","getCount","()J",overflow,NULL,0,&out)||out.j)return false;

 /* Stop hook, entry path: the flag is already set before await starts. */
 latch_runtime_resume();
 dvm_ref stopped=latch_new_count(v,1);
 if(!stopped)return false;
 latch_runtime_stop(v);
 bool stopped_ok=latch_runtime_call(v,"java/util/concurrent/CountDownLatch","await","()V",stopped,NULL,0,&out)&&dvm_exception(v);
 latch_runtime_resume();dvm_clear_exception(v);
 if(!stopped_ok)return false;

 /* Stop after blocking: the worker must be inside pthread_cond_timedwait
  * (confirmed by l->waiting under the latch mutex), then another thread sets
  * the selftest hook and the main thread waits for a bounded exit + join.
  * This does not call dvm_request_stop(); real VM teardown is a separate
  * branch in await_latch and is not exercised here. */
 latch_runtime_resume();
 dvm_ref blocked_stop=latch_new_count(v,1);
 latch_state *sl=latch_of(v,blocked_stop);if(!sl)return false;
 latch_block_task task={0};task.v=v;task.ref=blocked_stop;
 if(pthread_mutex_init(&task.done_mutex,NULL)||pthread_cond_init(&task.done_cond,NULL))return false;
 pthread_t block_worker,stop_worker;
 if(pthread_create(&block_worker,NULL,latch_block_worker,&task)!=0)return false;
 latch_stop_probe probe={.l=sl,.v=v,.wait_ms=3000};
 if(pthread_create(&stop_worker,NULL,latch_stop_worker,&probe)!=0){
  latch_runtime_stop(v);pthread_join(block_worker,NULL);latch_runtime_resume();dvm_clear_exception(v);
  pthread_mutex_destroy(&task.done_mutex);pthread_cond_destroy(&task.done_cond);return false;
 }
 bool finished=latch_wait_done(v,&task,4000);
 if(!finished){latch_runtime_stop(v);finished=latch_wait_done(v,&task,1000);}
 void *stop_result=NULL;
 pthread_join(stop_worker,&stop_result);
 if(!finished){/* Never free the latch under a still-blocked worker. */
  pthread_detach(block_worker);return false;
 }
 pthread_join(block_worker,NULL);
 latch_runtime_resume();dvm_clear_exception(v);
 pthread_mutex_destroy(&task.done_mutex);pthread_cond_destroy(&task.done_cond);
 bool saw_blocked=(uintptr_t)stop_result!=0;
 if(!saw_blocked||!task.call_ret||!task.saw_exception)return false;

 sdk_trace_fprintf(stderr,"LATCH_RUNTIME_SELFTEST PASS constructor=1 countDown=1 await_zero=1 positive_wait=1 timed_timeout=1 timed_nonpositive=1 timed_overflow=1 stop_immediate_hook=1 stop_after_blocked_hook=1 getCount_long=1\n");
 return true;
}
