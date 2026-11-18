/* Optional pooled consumer. Queue normalization/injection remain unchanged.
 * No FFRT wait/yield while VM TLS/GIL is active; ordinary OS sleep only after
 * releasing the GIL. One long-lived pool task, not a new pthread per input. */
#include <ffrt/task.h>
#include "gc_observ_core.h"
extern int sdk_app_vm_is_foreground(void);
extern void gco_emit(unsigned,uint64_t,int,int,int,int,uint64_t,uint64_t) __attribute__((weak));
static atomic_int touch_worker_enabled,touch_worker_stop,touch_worker_running;
static atomic_uint touch_drain_owner;
static void drain_touches_impl(state *,bool);
static void touch_worker_run(void *arg){
 state *s=*(state **)((char*)arg+sizeof(ffrt_function_header_t));
 while(!atomic_load_explicit(&touch_worker_stop,memory_order_acquire)){
  bool enabled=atomic_load_explicit(&touch_worker_enabled,memory_order_acquire);
  if(enabled){
   pthread_mutex_lock(&touch_lock);bool pending=touch_head!=touch_tail;pthread_mutex_unlock(&touch_lock);
   if(pending){
    uint64_t began=perf_now();unsigned empty=0;int outcome=0;
    if(atomic_compare_exchange_strong_explicit(&touch_drain_owner,&empty,2,memory_order_acquire,memory_order_relaxed)){
     outcome=1;
     if(dvm_gil_try_acquire(s->vm)){
      outcome=3;
      if(!atomic_load(&touch_worker_stop)&&frame_touch_state==s&&sdk_app_vm_is_foreground()&&!dvm_exception(s->vm)&&!jni_bridge_pending(s->bridge)){
       outcome=2;drain_touches_impl(s,true);
      }
      dvm_gil_release(s->vm);
     }
     atomic_store_explicit(&touch_drain_owner,0,memory_order_release);
    }
    /* a:0 owner busy,1 GIL busy,2 dispatched,3 lifecycle/exception guard. */
    if(gco_emit)gco_emit(GCO_REJECT,0,6,outcome,0,0,began,perf_now());
   }
  }
  struct timespec delay={0,enabled?1000000:100000000};nanosleep(&delay,NULL);
 }
 atomic_store_explicit(&touch_worker_running,0,memory_order_release);
}
static void touch_worker_destroy(void *arg){(void)arg;}
static bool touch_worker_start(state *s){
 if(atomic_exchange(&touch_worker_running,1))return false;
 ffrt_function_header_t *f=ffrt_alloc_auto_managed_function_storage_base(ffrt_function_kind_general);
 if(!f){atomic_store(&touch_worker_running,0);atomic_store(&touch_worker_enabled,0);return false;}
 memset(f,0,sizeof *f);f->exec=touch_worker_run;f->destroy=touch_worker_destroy;
 *(state **)((char*)f+sizeof *f)=s;atomic_store(&touch_worker_stop,0);
 ffrt_task_attr_t attr;ffrt_task_attr_init(&attr);ffrt_task_attr_set_name(&attr,"touch-drain");
 ffrt_task_attr_set_qos(&attr,ffrt_qos_user_initiated);
 ffrt_task_attr_set_stack_size(&attr,1024*1024);
 ffrt_submit_base(f,NULL,NULL,&attr);ffrt_task_attr_destroy(&attr);return true;
}
static void touch_worker_finish(state *s){
 atomic_store(&touch_worker_enabled,0);atomic_store_explicit(&touch_worker_stop,1,memory_order_release);
 /* The state is stack-owned: never return/free it while the pooled task can
  * reference it. Drop every GIL level before joining, matching existing order. */
 unsigned depth=dvm_gil_held(s->vm)?dvm_gil_unlock_all(s->vm):0;
 while(atomic_load_explicit(&touch_worker_running,memory_order_acquire)){
  struct timespec delay={0,1000000};nanosleep(&delay,NULL);
 }
 if(depth)dvm_gil_relock(s->vm,depth);
}
