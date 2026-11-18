#define _GNU_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#include "unity_player_probe.h"
#include "native_fault_probe.h"
#include "android_dlfcn.h"
#include "elf_loader.h"
#include "jni_bridge.h"
#include "android_basics.h"
#include "activity_runtime.h"
#include "application_lifecycle.h"
#include "platform_runtime.h"
#include "collections.h"
#include "socket_runtime.h"
#include "intent_runtime.h"
#include "egl_window_runtime.h"
#include "perf_attr.h"
#include "dvm/dvm_internal.h"
#include <time.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/syscall.h>
#include <errno.h>
#include <sys/stat.h>
#include <hilog/log.h>
#include "multitouch_packet.h"
#ifndef LUNARIA_CONSTRUCTOR_CANDIDATE
#define LUNARIA_CONSTRUCTOR_CANDIDATE "unlabeled"
#endif
extern bool jni_forname_selftest(JNIEnv *env);
extern bool jni_activity_get_intent_selftest(struct dvm *,dvm_ref,dvm_ref);
extern void dvm_main_looper_tick(struct dvm *vm);
static atomic_flag attempted=ATOMIC_FLAG_INIT;
static int (*activity_state_host)(void *,int *,int *);
static void *activity_state_user;
static pthread_mutex_t state_host_lock=PTHREAD_MUTEX_INITIALIZER;
void sdk_set_unity_activity_state_host(int (*query)(void *,int *,int *),void *user){pthread_mutex_lock(&state_host_lock);activity_state_host=query;activity_state_user=user;pthread_mutex_unlock(&state_host_lock);}
static bool foreground_observation(struct dvm *vm,int *showing,int *focused){
 pthread_mutex_lock(&state_host_lock);int (*query)(void *,int *,int *)=activity_state_host;void *user=activity_state_user;pthread_mutex_unlock(&state_host_lock);
 if(!query)return false;
 unsigned depth=dvm_gil_held(vm)?dvm_gil_unlock_all(vm):0;
 int status=query(user,showing,focused);
 if(depth)dvm_gil_relock(vm,depth);
 if(status==0)perf_background(!*showing);
 return status==0&&(*showing==0||*showing==1)&&(*focused==0||*focused==1);
}

typedef struct {char fault_path[4096];FILE *file;struct dvm*vm;struct jni_bridge*bridge;dvm_ref activity;int ctor_ok,create_ok,start_ok,resume_ok,focus_ok;unsigned init_enter,init_return;dvm_ref init_receiver,init_context;uint32_t limited_before;unsigned long long submission_baseline;} state;

#ifdef LUNARIA_TASK8A
#include "task8a_watch.inc"
#endif

static void require_record(state*s,int n);
/* ---------------- 宿主触摸注入（XComponent → Unity nativeInjectEvent） ----------------
 * 队列由宿主 XComponent 的 Touch 回调（ArkTS 主线程）写入，VM worker 在
 * frame_wait 的 100ms tick 中消费：构造 MotionEvent 快照并调用
 * UnityPlayer.nativeInjectEvent(InputEvent,int)。该方法由 libunity 通过
 * RegisterNatives 注册（sig (Landroid/view/InputEvent;I)Z），dvm 的
 * call_native hook 会进入真实实现。 */
#define TOUCH_QUEUE_CAP touch_capacity
static unsigned touch_capacity=64;
static atomic_int play_touch_front,play_touch_preserve,play_touch_timestamps;
static atomic_uint_fast64_t play_drains,play_drained,play_unpaired;
typedef struct { int action,pointer_id; float x,y; int64_t event_ms;uint64_t host_ns,drain_ns; int multi;unsigned trace_slot;mt_packet packet; } queued_touch;
static atomic_int multitouch_enabled;
#include "input_pipeline_observer.inc"
#include "touch_drain_budget.inc"
static void multitouch_poll(void);
static void multitouch_native_done(struct dvm *,dvm_ref,int);
#if defined(LUNARIA_TASK6F_PREFLIGHT) || defined(LUNARIA_TASK6K_OBSERVER)
static void multitouch_unity_flush(void);
#endif
extern int dvm_gil_frame_touch_enabled(void);
extern bool dvm_gil_try_acquire(struct dvm *);
static state *frame_touch_state; /* Published/cleared under GIL; never used by host producer. */
static uint64_t frame_touch_last,touch_retry_count,touch_full_count;
static _Thread_local bool frame_touch_busy;
static bool touch_draining; /* GIL-protected, stays set across a native call's GIL drop. */
static struct {struct dvm *vm;dvm_ref event;uint64_t host_ns;int action;} touch_pending[256];
static unsigned touch_pending_next;
void sdk_input_native_touch_done(struct dvm *vm,dvm_ref event,int accepted){
 multitouch_native_done(vm,event,accepted);
 for(unsigned i=0;i<256;i++)if(touch_pending[i].vm==vm&&touch_pending[i].event==event){
  uint64_t now=perf_now();static pid_t pid;bool first=pid!=getpid();pid=getpid();
  input_obs_record(INPUT_NATIVE,0,touch_pending[i].action,accepted?1:0,0,now);
  FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/gil-touch-native-latency.csv",first?"w":"a");
  if(f){if(first)fputs("pid,host_ns,native_return_ns,action,latency_ms,accepted\n",f);fprintf(f,"%d,%llu,%llu,%d,%.6f,%d\n",getpid(),(unsigned long long)touch_pending[i].host_ns,(unsigned long long)now,touch_pending[i].action,(now-touch_pending[i].host_ns)/1e6,accepted);fclose(f);}
  touch_pending[i].event=0;break;
 }
}
static pthread_mutex_t touch_lock=PTHREAD_MUTEX_INITIALIZER;
static queued_touch touch_queue[4096];
static unsigned touch_head,touch_tail;
static uint64_t touch_queued,touch_dropped;
static _Atomic uint64_t touch_injected,touch_failed;
static _Atomic long touch_consumer_tid;
static void input_obs_queue_snapshot(uint64_t *q){
 pthread_mutex_lock(&touch_lock);q[0]=touch_queued;q[3]=touch_dropped;q[4]=(touch_tail+TOUCH_QUEUE_CAP-touch_head)%TOUCH_QUEUE_CAP;q[5]=TOUCH_QUEUE_CAP;q[6]=touch_full_count;pthread_mutex_unlock(&touch_lock);
 q[1]=atomic_load(&touch_injected);q[2]=atomic_load(&touch_failed);q[7]=__atomic_load_n(&touch_retry_count,__ATOMIC_RELAXED);
}
#include "touch_worker.inc"
#include "enqueue_drain_observer.inc"
void sdk_input_play_snapshot(uint64_t *values){
 pthread_mutex_lock(&touch_lock);values[0]=touch_queued;values[3]=touch_dropped;values[4]=(touch_tail+TOUCH_QUEUE_CAP-touch_head)%TOUCH_QUEUE_CAP;pthread_mutex_unlock(&touch_lock);
 values[1]=atomic_load(&touch_injected);values[2]=atomic_load(&touch_failed);values[5]=atomic_load(&play_drains);values[6]=atomic_load(&play_drained);values[7]=atomic_load(&play_unpaired);
}
static void play_touch_controls(void){
 const char *names[]={"play-touch-front","play-touch-preserve","play-touch-timestamps"};atomic_int *flags[]={&play_touch_front,&play_touch_preserve,&play_touch_timestamps};
 for(unsigned i=0;i<3;i++){char path[256];snprintf(path,sizeof path,"/data/storage/el2/base/haps/entry/files/sdkdex/%s",names[i]);FILE *f=fopen(path,"r");int n=0;if(f){(void)fscanf(f,"%d",&n);fclose(f);}if(i!=1)atomic_store(flags[i],n==1);else{
  pthread_mutex_lock(&touch_lock);if(touch_head==touch_tail){touch_head=touch_tail=0;touch_capacity=n==1||atomic_load(&multitouch_enabled)?4096:64;atomic_store(flags[i],n==1);}pthread_mutex_unlock(&touch_lock);
 }}
}
extern void dvm_gil_consumer_status(long,char *,size_t);
/* Read-only GC diagnostic: snapshot queue counters under the producer lock. */
__attribute__((visibility("default"))) bool sdk_input_gc_touch_snapshot(uint64_t *queued,uint64_t *dropped){
 if(!queued||!dropped)return false;pthread_mutex_lock(&touch_lock);*queued=touch_queued;*dropped=touch_dropped;pthread_mutex_unlock(&touch_lock);return true;
}
extern dvm_ref sdk_motion_event_create(struct dvm*,int,int,float,float,int,int64_t,int64_t);
extern bool sdk_motion_event_set_device_id(struct dvm*,dvm_ref,int);
static bool touch_getter_dump(struct dvm *vm,dvm_ref event){
 static const struct {const char *name,*sig;} getters[]={
  {"getAction","()I"},{"getActionMasked","()I"},{"getActionIndex","()I"},{"getPointerCount","()I"},
  {"getPointerId","(I)I"},{"getX","()F"},{"getY","()F"},{"getRawX","()F"},{"getRawY","()F"},
  {"getPressure","()F"},{"getSize","()F"},{"getDownTime","()J"},{"getEventTime","()J"},
  {"getSource","()I"},{"getDeviceId","()I"},{"getFlags","()I"},{"getMetaState","()I"}
 };
 struct dvm_class *c=dvm_object_class(vm,event);
 for(unsigned i=0;i<sizeof getters/sizeof getters[0];i++){
  struct dvm_method *m=dvm_find_method(vm,c,getters[i].name,getters[i].sig);union dvm_value zero={0},out={0};
  bool ok=m&&dvm_call(vm,m,event,&zero,m->arg_count,&out)&&!dvm_exception(vm);
  FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt","a");
  if(f){fprintf(f,"TOUCH_GETTER diagnostic=1 name=%s sig=%s ok=%d ",getters[i].name,getters[i].sig,ok);
   if(m&&m->ret_kind=='F')fprintf(f,"value=%.4f",out.f);
   else if(m&&m->ret_kind=='J')fprintf(f,"value=%lld",(long long)out.j);
   else fprintf(f,"value=%d",out.i);
   fprintf(f," exception=%u\n",dvm_exception(vm));fclose(f);}
  if(!ok)return false;
 }
 return true;
}
static bool play_touch_selftest(struct dvm *vm){
 FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/play-stall-selftest.txt","w");if(!f)return false;
 bool pass=true;int durations[]={300,800,1500};
 for(unsigned i=0;i<3;i++){
  dvm_ref event=sdk_motion_event_create(vm,1,0,1,1,4098,10000,10000+durations[i]);if(event)dvm_pin(vm,event);
  struct dvm_class *c=event?dvm_object_class(vm,event):NULL;
  struct dvm_method *a=c?dvm_find_method(vm,c,"getDownTime","()J"):NULL,*b=c?dvm_find_method(vm,c,"getEventTime","()J"):NULL;
  union dvm_value down={0},end={0};bool ok=a&&b&&dvm_call(vm,a,event,NULL,0,&down)&&dvm_call(vm,b,event,NULL,0,&end)&&!dvm_exception(vm)&&down.j==10000&&end.j==10000+durations[i];
  fprintf(f,"pid=%d duration_ms=%d down_ms=%lld event_ms=%lld error_ms=%lld result=%s scope=MotionEvent_getters_not_device_gesture\n",getpid(),durations[i],(long long)down.j,(long long)end.j,(long long)(end.j-down.j-durations[i]),ok?"PASS":"FAIL");pass=pass&&ok;if(event)dvm_unpin(vm,event);if(!ok)break;
 }
 fprintf(f,"pid=%d result=%s frame_front_default=0 preserve_default=0 timestamps_default=0 moving_enabled=0\n",getpid(),pass?"PASS":"FAIL");fclose(f);return pass;
}
__attribute__((visibility("default"))) int sdk_input_queue_touch(int action,int pointer_id,float x,float y,int64_t event_time_ns){
 int64_t event_ms=event_time_ns>0?event_time_ns/1000000:0;
 pthread_mutex_lock(&touch_lock);
 unsigned next=(touch_tail+1)%TOUCH_QUEUE_CAP;
 if(next==touch_head){
  touch_full_count++;
  if(atomic_load(&play_touch_preserve)){touch_dropped++;pthread_mutex_unlock(&touch_lock);return -1;} /* Explicit overflow, never evict an earlier DOWN/UP. */
  /* Keep the most recent 31 plus this incoming event. Count all evictions;
   * full queue diagnostics must never block the producer on the GIL. */
  unsigned evicted=0;while((touch_tail+TOUCH_QUEUE_CAP-touch_head)%TOUCH_QUEUE_CAP>31){touch_head=(touch_head+1)%TOUCH_QUEUE_CAP;evicted++;}touch_dropped+=evicted;
  static uint64_t last_warn;struct timespec now;clock_gettime(CLOCK_MONOTONIC,&now);uint64_t ms=(uint64_t)now.tv_sec*1000+now.tv_nsec/1000000;int warn=!last_warn||ms-last_warn>=5000;if(warn)last_warn=ms;
  uint64_t dropped=touch_dropped;
  if(warn){pthread_mutex_unlock(&touch_lock);char status[256];dvm_gil_consumer_status(atomic_load(&touch_consumer_tid),status,sizeof status);FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/gil-touch.txt","a");if(f){fprintf(f,"TOUCHQ full=1 mono_ms=%llu evicted=%u dropped=%llu retained_recent=32 %s\n",(unsigned long long)ms,evicted,(unsigned long long)dropped,status);fclose(f);}pthread_mutex_lock(&touch_lock);next=(touch_tail+1)%TOUCH_QUEUE_CAP;
   /* Producer callbacks may overlap while diagnostics run. */
   if(next==touch_head){touch_head=(touch_head+1)%TOUCH_QUEUE_CAP;touch_dropped++;}
  }
 }
 queued_touch *q=&touch_queue[touch_tail];
 q->action=action;q->pointer_id=pointer_id;q->x=x;q->y=y;q->event_ms=event_ms;q->multi=0;
 q->host_ns=perf_now();
 input_obs_record(INPUT_ENQUEUE,0,action,0,q->host_ns,q->host_ns);
 touch_tail=next;touch_queued++;
 pthread_mutex_unlock(&touch_lock);
 ed_notify();
 if(perf_trace_level()>=2)OH_LOG_Print(LOG_APP,LOG_INFO,0xFF00,"NativeProbeTouch","TOUCHQ host=0 action=%{public}d id=%{public}d queued=%{public}llu dropped=%{public}llu",action,pointer_id,(unsigned long long)touch_queued,(unsigned long long)touch_dropped);
 return 0;
}
#include "multitouch_runtime.inc"
#include "multitouch_selftest.inc"
static bool inject_touch(struct dvm *vm,dvm_ref player,const queued_touch *q,int64_t down_ms,int *consumed){
 if(q->multi)return multitouch_inject(vm,player,q,consumed);
 struct dvm_class *up=dvm_find_class(vm,"com/unity3d/player/UnityPlayer");
 struct dvm_method *m=up?dvm_find_method(vm,up,"onTouchEvent","(Landroid/view/MotionEvent;)Z"):NULL;
 if(!m){if(consumed)*consumed=0;return false;}
 dvm_ref me=sdk_motion_event_create(vm,q->action,q->pointer_id,q->x,q->y,4098 /*InputDevice.SOURCE_TOUCHSCREEN*/,down_ms,q->event_ms);
 if(!me||dvm_exception(vm)){if(consumed)*consumed=0;return false;}
 unsigned timing=touch_pending_next++%256;touch_pending[timing].vm=vm;touch_pending[timing].event=me;touch_pending[timing].host_ns=q->host_ns;touch_pending[timing].action=q->action;
 {int w=0,h=0;int valid=egl_window_runtime_touch_dimensions(&w,&h);FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt","a");if(f){fprintf(f,"TOUCH_SURFACE valid=%d width=%d height=%d x=%.3f y=%.3f\n",valid,w,h,q->x,q->y);fclose(f);}}
 dvm_pin(vm,me);
 int device_id=-1;
 struct dvm_class *devices=dvm_find_class(vm,"android/view/InputDevice");
 struct dvm_method *ids=devices?dvm_find_method(vm,devices,"getDeviceIds","()[I"):NULL;
 if(ids){union dvm_value list={0};if(!dvm_call(vm,ids,0,NULL,0,&list)||dvm_exception(vm)){dvm_unpin(vm,me);return false;}
  if(list.l&&dvm_array_length(vm,list.l)>0){int32_t *data=dvm_array_data(vm,list.l);if(data)device_id=data[0];}}
 if(!sdk_motion_event_set_device_id(vm,me,device_id)||!touch_getter_dump(vm,me)){dvm_unpin(vm,me);return false;}
 OH_LOG_Print(LOG_APP,LOG_INFO,0xFF00,"NativeProbeTouch","TOUCH_DEVICE id=%{public}d getDeviceIds_available=%{public}d fallback_virtual=%{public}d",device_id,ids?1:0,device_id==-1?1:0);
 /* 第二参数是 displayId（UnityPlayer.mCurrentFocusDisplayId，正常为 0），
  * 不是 input source；source 已写入 MotionEvent（4098=SOURCE_TOUCHSCREEN）。 */
 union dvm_value args[1]={ {.l=me} }, out={0};
 uint64_t input_call_begin=perf_now();
 bool ok=dvm_call(vm,m,player,args,1,&out)&&!dvm_exception(vm);
 input_obs_record(INPUT_GUEST,0,q->action,ok?(out.i?1:0):-7,input_call_begin,perf_now());
 {FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/tap-noop-touch.txt","a");if(f){fprintf(f,"pid=%d mono_ms=%llu action=%d id=%d x=%.1f y=%.1f event_ms=%lld down_ms=%lld route=original_Java_onTouchEvent ok=%d consumed=%d exception=%u queued=%llu injected_before=%llu failed=%llu dropped=%llu\n",getpid(),(unsigned long long)(perf_now()/1000000),q->action,q->pointer_id,q->x,q->y,(long long)q->event_ms,(long long)down_ms,ok,ok?out.i:0,dvm_exception(vm),(unsigned long long)touch_queued,(unsigned long long)touch_injected,(unsigned long long)touch_failed,(unsigned long long)touch_dropped);fclose(f);}}
 {FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt","a");if(f){fprintf(f,"TOUCH_ROUTE route=original_Java_onTouchEvent ok=%d consumed=%d exception=%u java_thread=%u\n",ok,ok?out.i:0,dvm_exception(vm),vm->cur_thread);fclose(f);}}
 dvm_unpin(vm,me);
 if(consumed)*consumed=out.i?1:0;
 return ok;
}
static void touch_snapshot_file(void){
 static uint64_t last_q=UINT64_MAX,last_i=UINT64_MAX,last_f=UINT64_MAX,last_d=UINT64_MAX;
 pthread_mutex_lock(&touch_lock);uint64_t q=touch_queued,d=touch_dropped;pthread_mutex_unlock(&touch_lock);
 uint64_t i=atomic_load(&touch_injected),failed=atomic_load(&touch_failed);
 if(q==last_q&&i==last_i&&failed==last_f&&d==last_d)return;
 FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/viewgroup-touch.txt","a");
 if(f){fprintf(f,"pid=%d epoch=%lld injected=%llu queued=%llu failed=%llu dropped=%llu\n",getpid(),(long long)time(NULL),(unsigned long long)i,(unsigned long long)q,(unsigned long long)failed,(unsigned long long)d);fclose(f);last_q=q;last_i=i;last_f=failed;last_d=d;}
}
static void drain_touches_impl(state *s,bool independent){
 if(touch_draining)return;
 if(atomic_load(&multitouch_enabled)&&(dvm_exception(s->vm)||jni_bridge_pending(s->bridge)))return;
 if(!independent&&!atomic_load(&ed_every_frame)&&!atomic_load(&ed_wake)&&dvm_gil_frame_touch_enabled()&&!frame_touch_busy&&frame_touch_last&&perf_now()-frame_touch_last<=100000000ull)return;
 touch_draining=true;
 struct touch_batch batch=touch_batch_begin(perf_now());
 atomic_fetch_add(&play_drains,1);
 atomic_store(&touch_consumer_tid,(long)syscall(SYS_gettid));
 static int64_t current_down_ms;
 static struct {int id;int64_t down;int active;} presses[32];
 for(;;){
  queued_touch q;
  pthread_mutex_lock(&touch_lock);
  if(touch_head==touch_tail){pthread_mutex_unlock(&touch_lock);break;}
  /* Test before popping: the remainder stays in exactly the same FIFO. */
  if(touch_batch_limit(&batch,perf_now())){pthread_mutex_unlock(&touch_lock);break;}
  q=touch_queue[touch_head];touch_head=(touch_head+1)%TOUCH_QUEUE_CAP;
  q.drain_ns=perf_now();
  input_obs_queue_wait(q.multi?q.packet.enqueue_ns:q.host_ns,q.drain_ns);
  pthread_mutex_unlock(&touch_lock);
  if(q.multi){
   dvm_ref player=0;struct dvm_class *ac=dvm_find_class(s->vm,"com/unity3d/player/UnityPlayerActivity");union dvm_value pv={0};
   if(s->activity&&ac&&dvm__get_field_from(s->vm,s->activity,ac,"mUnityPlayer","Lcom/unity3d/player/UnityPlayer;",&pv)&&!dvm_exception(s->vm))player=pv.l;
   int consumed=0;bool ok=inject_touch(s->vm,player,&q,q.packet.down_ns/1000000,&consumed);
   atomic_fetch_add(&play_drained,1);if(ok)touch_injected++;else touch_failed++;
   touch_batch_event(&batch,q.drain_ns,perf_now());
   if(dvm_exception(s->vm)||jni_bridge_pending(s->bridge))break;
   continue;
  }
  int action=q.action&0xff;
  if(action==0)current_down_ms=q.event_ms; /* ACTION_DOWN */
  unsigned press=(unsigned)q.pointer_id%32;
  if(atomic_load(&play_touch_timestamps)){
   if(action==0){if(presses[press].active)atomic_fetch_add(&play_unpaired,1);presses[press].id=q.pointer_id;presses[press].down=q.event_ms;presses[press].active=1;}
   if(!presses[press].active||presses[press].id!=q.pointer_id)atomic_fetch_add(&play_unpaired,1);
   current_down_ms=presses[press].active&&presses[press].id==q.pointer_id?presses[press].down:q.event_ms;
  }
  dvm_ref player=0;
  struct dvm_class *ac=dvm_find_class(s->vm,"com/unity3d/player/UnityPlayerActivity");
  union dvm_value pv={0};
  if(s->activity&&ac&&dvm__get_field_from(s->vm,s->activity,ac,"mUnityPlayer","Lcom/unity3d/player/UnityPlayer;",&pv)&&!dvm_exception(s->vm)&&pv.l)player=pv.l;
  int consumed=0;
  /* Inject TAP 的 DOWN/UP 共用同一 event_ns：UP 的 eventTime 至少比
   * downTime 晚 80ms，避免引擎把零时长点击判为无效。 */
  queued_touch qq=q;
  if(!atomic_load(&play_touch_timestamps)&&(action==1||action==3)&&current_down_ms&&qq.event_ms<=current_down_ms)qq.event_ms=current_down_ms+80;
  bool ok=player&&inject_touch(s->vm,player,&qq,current_down_ms?current_down_ms:qq.event_ms,&consumed);
  atomic_fetch_add(&play_drained,1);
  if(ok)touch_injected++;else touch_failed++;
  touch_batch_event(&batch,q.drain_ns,perf_now());
  if(action==1||action==3)current_down_ms=0; /* UP/CANCEL */
  if(atomic_load(&play_touch_timestamps)&&(action==1||action==3))presses[press].active=0;
  /* Inject TAP 合成的 DOWN/UP 使用同一 event_ms；若同一 tick 连续注入，
   * Unity 的 uGUI 会把按下与抬起合并。DOWN 之后若队首是同一 event_ms 的
   * UP，本 tick 停止，让 UP 在下一个 100ms tick 注入。 */
  if(action==0&&!atomic_load(&play_touch_timestamps)){
   int split=0;
   pthread_mutex_lock(&touch_lock);
   if(touch_head!=touch_tail&&(touch_queue[touch_head].action&0xff)==1&&touch_queue[touch_head].event_ms==q.event_ms)split=1;
   pthread_mutex_unlock(&touch_lock);
   if(split)break;
  }
 }
 touch_batch_finish(&batch,perf_now());
 if(batch.events&&!independent){
  /* Keep the existing reentrancy guard set across this normal GIL handoff.
   * No touch_lock is held and no event is partly dispatched at this point.
   * Restore the exact nesting depth using the runtime's existing lock order. */
  struct dvm *vm=s->vm;
  unsigned depth=dvm_gil_held(vm)?dvm_gil_unlock_all(vm):0;
  if(depth){
   if(atomic_load_explicit(&input_obs_enabled,memory_order_relaxed))atomic_fetch_add_explicit(&touch_batch_yields,1,memory_order_relaxed);
   dvm_gil_relock(vm,depth);
  }
 }
 touch_draining=false;
}
static void drain_touches(state *s){
 if(atomic_load_explicit(&touch_worker_enabled,memory_order_acquire))return;
 unsigned empty=0;
 if(!atomic_compare_exchange_strong_explicit(&touch_drain_owner,&empty,1,memory_order_acquire,memory_order_relaxed))return;
 drain_touches_impl(s,false);
 atomic_store_explicit(&touch_drain_owner,0,memory_order_release);
}
/* Called at the real nativeRender boundary. A contended caller retains the
 * queue for a later frame. The ArkTS/XComponent producer never takes GIL. */
void sdk_input_frame_touch(struct dvm *vm){
 if((!atomic_load(&ed_every_frame)&&!dvm_gil_frame_touch_enabled()&&!atomic_load(&play_touch_front)&&!atomic_load(&multitouch_enabled))||frame_touch_busy)return;
 bool owned=!dvm_gil_held(vm);
 if(owned&&!dvm_gil_try_acquire(vm)){input_obs_gil(0);__atomic_fetch_add(&touch_retry_count,1,__ATOMIC_RELAXED);return;}
 input_obs_gil(1);
 if(frame_touch_state&&frame_touch_state->vm==vm&&!dvm_exception(vm)&&!jni_bridge_pending(frame_touch_state->bridge)){
  frame_touch_busy=true;frame_touch_last=perf_now();drain_touches(frame_touch_state);frame_touch_busy=false;
 }
 if(owned)dvm_gil_release(vm);
}
/* Read-only known-data snapshots for the original Unity binary. Check mapped
 * readable ranges; never dereference contents as objects or alter state. */
static void directory_global_snapshot(struct jni_bridge *bridge,const char *tag){
 uintptr_t base=0;FILE *maps=fopen("/proc/self/maps","r");if(!maps)return;
 char line[512];while(fgets(line,sizeof line,maps)){unsigned long long a=0,b=0,off=0;char perm[5]={0};
  if(strstr(line,"libunity.so")&&sscanf(line,"%llx-%llx %4s %llx",&a,&b,perm,&off)==4&&off==0){base=(uintptr_t)a;break;}}
 fclose(maps);if(!base)return;
 const uintptr_t offsets[]={0x1888d20,0x1888d48,0x1883300,0x18c4cb8,0x18c4cc8,0x18c22e8,0x18c2490,0x18c24a0,0x18857a0,0x18857c8,0x1884c80,0x187d4e0};
 const size_t lengths[]={40,8,8,16,8,16,16,16,64,64,8,8};
 FILE *out=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt","a");if(!out)return;
 fprintf(out,"UNITY_DIRECTORY_HOST_VM tag=%s vm=%p\n",tag,(void*)jni_bridge_java_vm(bridge));
 for(size_t i=0;i<sizeof offsets/sizeof offsets[0];i++){
  uintptr_t p=base+offsets[i];int readable=0;maps=fopen("/proc/self/maps","r");
  if(maps){while(fgets(line,sizeof line,maps)){unsigned long long a=0,b=0;char perm[5]={0};
    if(sscanf(line,"%llx-%llx %4s",&a,&b,perm)==3&&perm[0]=='r'&&p>=a&&p<b&&lengths[i]<=b-p){readable=1;break;}}
   fclose(maps);}
  fprintf(out,"UNITY_DIRECTORY_GLOBAL tag=%s base=0x%llx off=0x%llx readable=%d bytes=",tag,(unsigned long long)base,(unsigned long long)offsets[i],readable);
  if(readable){const volatile unsigned char *data=(const volatile unsigned char*)p;for(size_t n=0;n<lengths[i];n++)fprintf(out,"%02x",(unsigned)data[n]);}
  fputc('\n',out);
  if(readable&&(offsets[i]==0x1883300||offsets[i]==0x18c22e8)){
   for(size_t slot=0;slot<lengths[i]/8;slot++){
    uintptr_t child=0;memcpy(&child,(const void*)(p+slot*8),8);int child_ok=0;
    maps=fopen("/proc/self/maps","r");
    if(maps){while(fgets(line,sizeof line,maps)){unsigned long long a=0,b=0;char perm[5]={0};
      if(sscanf(line,"%llx-%llx %4s",&a,&b,perm)==3&&perm[0]=='r'&&child>=a&&child<b&&32<=b-child){child_ok=1;break;}}fclose(maps);}
    fprintf(out,"UNITY_DIRECTORY_CHILD tag=%s owner_off=0x%llx slot=%zu address=0x%llx readable=%d bytes=",tag,(unsigned long long)offsets[i],slot,(unsigned long long)child,child_ok);
    if(child_ok){const volatile unsigned char *data=(const volatile unsigned char*)child;for(size_t n=0;n<32;n++)fprintf(out,"%02x",(unsigned)data[n]);}
    fputc('\n',out);
   }
  }
  if(readable&&offsets[i]==0x1884c80){
   uintptr_t obj=0;memcpy(&obj,(const void*)p,8);int ook=0;maps=fopen("/proc/self/maps","r");
   if(maps&&obj){while(fgets(line,sizeof line,maps)){unsigned long long a=0,b=0;char perm[5]={0};
     if(sscanf(line,"%llx-%llx %4s",&a,&b,perm)==3&&perm[0]=='r'&&obj>=a&&obj<b&&0x60<=b-obj){ook=1;break;}}}
   if(maps)fclose(maps);
   fprintf(out,"UNITY_INTERNAL_DATA_PATH tag=%s object=0x%llx readable=%d",tag,(unsigned long long)obj,ook);
   if(ook){uintptr_t sp=0;size_t sl=0;memcpy(&sp,(const void*)(obj+0x50),8);memcpy(&sl,(const void*)(obj+0x58),8);
     int sok=0;maps=fopen("/proc/self/maps","r");
     if(maps&&sl<=4096){while(fgets(line,sizeof line,maps)){unsigned long long a=0,b=0;char perm[5]={0};
       if(sscanf(line,"%llx-%llx %4s",&a,&b,perm)==3&&perm[0]=='r'&&sp>=a&&sp<b&&sl<=b-sp){sok=1;break;}}}
     if(maps)fclose(maps);
     fprintf(out," data=0x%llx len=%zu readable=%d text=",(unsigned long long)sp,sl,sok);
     if(sok){const volatile unsigned char *data=(const volatile unsigned char*)sp;size_t show=sl<200?sl:200;for(size_t n=0;n<show;n++)fputc((data[n]>=32&&data[n]<127)?(int)data[n]:'.',out);}}
   fputc(10,out);
  }
  if(readable&&(offsets[i]==0x18857a0||offsets[i]==0x18857c8)){
   uintptr_t dp=0;size_t dlen=0;memcpy(&dp,(const void*)p,8);memcpy(&dlen,(const void*)(p+8),8);
   int dok=0;maps=fopen("/proc/self/maps","r");
   if(maps&&dlen<=4096){while(fgets(line,sizeof line,maps)){unsigned long long a=0,b=0;char perm[5]={0};
     if(sscanf(line,"%llx-%llx %4s",&a,&b,perm)==3&&perm[0]=='r'&&dp>=a&&dp<b&&dlen<=b-dp){dok=1;break;}}}
   if(maps)fclose(maps);
   fprintf(out,"UNITY_DIRECTORY_STRING tag=%s off=0x%llx data=0x%llx len=%zu readable=%d text=",tag,(unsigned long long)offsets[i],(unsigned long long)dp,dlen,dok);
   if(dok){const volatile unsigned char *data=(const volatile unsigned char*)dp;size_t show=dlen<200?dlen:200;for(size_t n=0;n<show;n++)fputc((data[n]>=32&&data[n]<127)?(int)data[n]:'.',out);}
   fputc('\n',out);
  }
 }
 fclose(out);
}
/* GiveChanceToAttachDebugger 的触发源观测：
 * 0x8387fc = getenv("UNITY_GIVE_CHANCE_TO_ATTACH_DEBUGGER") != NULL
 *            || 0x830d94(&0x1876020, 0) != 0
 * 这里只读取该配置对象与两处进程内标志，不改写引擎状态。 */
static int probe_ptr_text(uintptr_t p,char *out,size_t cap){
    if(!out||cap<2)return 0;out[0]=0;if(!p)return 0;FILE*m=fopen("/proc/self/maps","r");if(!m)return 0;
    int ok=0;char line[512];
    while(fgets(line,sizeof line,m)){
        unsigned long long a=0,b=0;char perm[5]={0};
        if(sscanf(line,"%llx-%llx %4s",&a,&b,perm)!=3)continue;
        if(p>=a&&p<b&&perm[0]=='r'){
            size_t room=(size_t)(b-p);if(room>=cap)room=cap-1;
            memcpy(out,(void*)p,room);out[room]=0;
            size_t n=strlen(out);if(n>=cap)n=cap-1;
            for(size_t i=0;i<n;i++){unsigned char ch=(unsigned char)out[i];if(ch<0x20||ch>0x7e)out[i]='?';}
            out[n]=0;ok=1;break;
        }
    }
    fclose(m);return ok;
}
static int probe_range_perm(uintptr_t p,size_t len,char need){
    FILE*m=fopen("/proc/self/maps","r");if(!m)return 0;int ok=0;char line[512];
    while(fgets(line,sizeof line,m)){
        unsigned long long a=0,b=0;char perm[5]={0};
        if(sscanf(line,"%llx-%llx %4s",&a,&b,perm)!=3)continue;
        if(p>=a&&p+len<=b&&strchr(perm,need)){ok=1;break;}
    }
    fclose(m);return ok;
}
static uintptr_t unity_exec_base(void){
    FILE*m=fopen("/proc/self/maps","r");if(!m)return 0;uintptr_t base=0;char line[512];
    while(fgets(line,sizeof line,m)){
        unsigned long long a=0,b=0,off=0;char perm[5]={0};
        if(strstr(line,"libunity.so")&&sscanf(line,"%llx-%llx %4s %llx",&a,&b,perm,&off)==4&&off==0){
            if(probe_range_perm((uintptr_t)a+0x9eeda4,4,'x')){base=(uintptr_t)a;break;}
        }
    }
    fclose(m);return base;
}
static void unity_attach_debug_snapshot(const char *tag){
    uintptr_t base=unity_exec_base();if(!base)return;
    const unsigned long long *cfg=(const unsigned long long*)(base+0x1876020);
    unsigned long long q0=*(volatile const unsigned long long*)(cfg);
    unsigned long long q1=*(volatile const unsigned long long*)(cfg+1);
    unsigned long long q2=*(volatile const unsigned long long*)(cfg+2);
    unsigned b18=*(volatile const unsigned char*)(base+0x1876038);
    unsigned a0=*(volatile const unsigned char*)(base+0x18760a0);
    unsigned a1=*(volatile const unsigned char*)(base+0x18760a1);
    char s1[160],s2[160];
    int r1=probe_ptr_text((uintptr_t)q1,s1,sizeof s1);
    int r2=probe_ptr_text((uintptr_t)q2,s2,sizeof s2);
    unsigned long long obj_1883300=*(volatile const unsigned long long*)(base+0x1883300);
    unsigned long long obj_1884c80=*(volatile const unsigned long long*)(base+0x1884c80);
    char dump[2*32+1];dump[0]=0;
    if(obj_1884c80&&probe_range_perm((uintptr_t)obj_1884c80,0x80,'r')){
        static const char hx[]="0123456789abcdef";
        const volatile unsigned char*p=(const volatile unsigned char*)(uintptr_t)obj_1884c80;
        for(int i=0;i<32;i++){dump[i*2]=hx[p[0x40+i]>>4];dump[i*2+1]=hx[p[0x40+i]&15];}
        dump[64]=0;
    }
    char dump3300[2*48+1];dump3300[0]=0;
    if(obj_1883300&&probe_range_perm((uintptr_t)obj_1883300,0x60,'r')){
        static const char hx[]="0123456789abcdef";
        const volatile unsigned char*p=(const volatile unsigned char*)(uintptr_t)obj_1883300;
        for(int i=0;i<48;i++){dump3300[i*2]=hx[p[i]>>4];dump3300[i*2+1]=hx[p[i]&15];}
        dump3300[96]=0;
    }
    const char *env=getenv("UNITY_GIVE_CHANCE_TO_ATTACH_DEBUGGER");
    int should=-1,cfgval=-1;
    /* Observe memory only. Calling original configuration functions here can
     * initialize or mutate engine state from the diagnostic worker thread. */
    FILE*out=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt","a");if(!out)return;
    fprintf(out,"UNITY_ATTACH_DEBUG tag=%s base=0x%llx q0=0x%llx q1=0x%llx q2=0x%llx b18=%u flag_a0=%u flag_a1=%u r1=%d s1=%s r2=%d s2=%s env=%p envtext=%.80s should=%d cfgval=%d slot_1883300=0x%llx slot_1884c80=0x%llx obj40=%s obj3300=%s\n",
            tag,(unsigned long long)base,q0,q1,q2,b18,a0,a1,r1,r1?s1:"-",r2,r2?s2:"-",(void*)env,env?env:"(null)",should,cfgval,obj_1883300,obj_1884c80,dump,dump3300);
    fclose(out);
}
/* nativeRender 不返回时，从独立线程周期记录 UnityMain 的调度状态：区分忙等
 * （state=R、wchan 空）与阻塞（state=S/D、wchan 非空）。只读 /proc，不改引擎。 */
static void *native_render_watchdog(void *arg){
    int tid=(int)(intptr_t)arg;
    { FILE*o=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt","a");
      if(o){fprintf(o,"NATIVE_RENDER_WATCH_START tid=%d\n",tid);fclose(o);} }
    for(int i=0;i<20;i++){
        if(i){struct timespec ts={.tv_sec=0,.tv_nsec=200000000};nanosleep(&ts,NULL);}
        char path[128],line[512]={0},state='?';
        snprintf(path,sizeof path,"/proc/self/task/%d/stat",tid);
        FILE*f=fopen(path,"r");
        if(f){if(fgets(line,sizeof line,f)){char*rp=strrchr(line,')');if(rp&&rp[1]==' '&&rp[2])state=rp[2];}fclose(f);}
        char wchan[96]={0};
        snprintf(path,sizeof path,"/proc/self/task/%d/wchan",tid);
        f=fopen(path,"r");
        if(f){if(fgets(wchan,sizeof wchan,f)){wchan[strcspn(wchan,"\n")]=0;}fclose(f);}
        FILE*o=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt","a");
        if(o){fprintf(o,"NATIVE_RENDER_WATCH i=%d tid=%d state=%c wchan=%s\n",i,tid,state,wchan);fclose(o);}
    }
    return NULL;
}
/* jni_bridge 的 NATIVE_CALL 追踪点在引擎线程上运行（observer 是 TLS，那里
 * 为空），因此从这里导出启动入口，由 jni_bridge 直接调用。 */
void unity_player_probe_build_class(void){
    uintptr_t base=unity_exec_base();if(!base)return;
    char hex[2*32+1];hex[0]=0;
    if(probe_range_perm(base+0x18c3598,32,'r')){
        static const char hx[]="0123456789abcdef";
        const volatile unsigned char*p=(const volatile unsigned char*)(base+0x18c3598);
        for(int i=0;i<32;i++){hex[i*2]=hx[p[i]>>4];hex[i*2+1]=hx[p[i]&15];}
        hex[64]=0;
    }
    unsigned long long lvl2=0,inner=0;
    if(probe_range_perm(base+0x18c3598+8,8,'r'))
        lvl2=*(volatile const unsigned long long*)(base+0x18c3598+8);
    if(lvl2&&probe_range_perm((uintptr_t)lvl2,8,'r'))
        inner=*(volatile const unsigned long long*)(uintptr_t)lvl2;
    unsigned long long hook=0,cls=0;
    if(probe_range_perm(base+0x18c4cd0,8,'r'))
        hook=*(volatile const unsigned long long*)(base+0x18c4cd0);
    if(hook){
        typedef unsigned long long (*get_cls_fn)(void*);
        cls=((get_cls_fn)(base+0x1686288))((void*)(base+0x18c3598));
    }
    FILE*f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt","a");
    if(f){fprintf(f,"BUILD_CLASS_PROBE raw=%s lvl2=0x%llx inner=0x%llx hook=0x%llx jclass=0x%llx\n",hex,lvl2,inner,hook,cls);fclose(f);}
}
/* Process-wide JNI boundary evidence, including calls from guest workers
 * outside the bootstrap thread's TLS observer. No lifecycle work is run. */
void unity_player_probe_init_jni(int entering,dvm_ref self,dvm_ref context){
    static atomic_uint calls;
    unsigned count=entering?atomic_fetch_add(&calls,1)+1:atomic_load(&calls);
    int saved_errno=errno;
    OH_LOG_Print(LOG_APP,LOG_INFO,0xFF00,"NativeProbeLife","UNITY_INIT_JNI phase=%{public}s calls=%{public}u pid=%{public}d tid=%{public}ld receiver=%{public}u context=%{public}u mono_ns=%{public}llu",entering?"enter":"return",count,getpid(),(long)syscall(SYS_gettid),self,context,(unsigned long long)perf_now());
    errno=saved_errno;
}
void unity_player_probe_on_native_render(void){
#ifdef LUNARIA_TASK8A
    task8a_beat(1);
#endif
    static _Atomic unsigned started;
    if(atomic_fetch_add(&started,1)!=0)return;
    OH_LOG_Print(LOG_APP,LOG_INFO,0xFF00,"NativeProbeLife","UNITY_FIRST_NATIVE_RENDER pid=%{public}d tid=%{public}ld mono_ns=%{public}llu",getpid(),(long)syscall(SYS_gettid),(unsigned long long)perf_now());
    unity_player_probe_build_class();
    pthread_t th;
    int rc=pthread_create(&th,NULL,native_render_watchdog,(void*)(intptr_t)syscall(SYS_gettid));
    { int e=errno; FILE*o=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt","a");
      if(o){fprintf(o,"NATIVE_RENDER_WATCHDOG_CREATE rc=%d errno=%d\n",rc,rc?e:0);fclose(o);} }
    if(rc==0)pthread_detach(th);
}
/* Diagnostics are buffered/enqueued; only the diagnostic worker owns disk
 * persistence. Never flush/fsync once per record while executing guest code. */
static _Atomic unsigned probe_diagnostic_failures;
static void require_record(state*s,int n){(void)s;if(n<0)atomic_fetch_add(&probe_diagnostic_failures,1);}
static int init_event(const char*path,size_t index,uintptr_t address,int entering,void*u){state*s=u;require_record(s,fprintf(s->file,"event=elf_constructor_%s path=%s index=%zu address=0x%llx thread=%llu\n",entering?"enter":"returned",path,index,(unsigned long long)address,(unsigned long long)(uintptr_t)pthread_self()));return 0;}
static int dl_event(const char*event,const char*path,uintptr_t address,int value,void*u){state*s=u;require_record(s,fprintf(s->file,"event=dl_%s path=%s address=0x%llx value=%d thread=%llu\n",event,path,(unsigned long long)address,value,(unsigned long long)(uintptr_t)pthread_self()));return 0;}
static void log_threads(state*s,const char*tag){
 /* These optional /proc snapshots used to flush/fsync once per thread while
  * holding GIL, every five seconds. Only phase B explicitly restores them. */
 if(!perf_ffrt_snapshots_enabled())return;
 uint64_t began=perf_ffrt_native_begin(1);
 DIR *d=opendir("/proc/self/task");if(!d){perf_ffrt_native_end(1,began);return;}
 struct dirent *e;int seen=0;
 while((e=readdir(d))&&seen<64){
  if(e->d_name[0]=='.')continue;
  char path[128],comm[64]={0},line[512]={0},channel[128]="unavailable",state='?';
  if(snprintf(path,sizeof path,"/proc/self/task/%s/comm",e->d_name)>=(int)sizeof path)continue;
  FILE *f=fopen(path,"r");if(!f)continue;int have=fgets(comm,sizeof comm,f)!=NULL;fclose(f);if(!have)continue;
  comm[strcspn(comm,"\n")]=0;
  snprintf(path,sizeof path,"/proc/self/task/%s/stat",e->d_name);f=fopen(path,"r");
  if(f){if(fgets(line,sizeof line,f)){char *rp=strrchr(line,')');if(rp&&rp[1]==' '&&rp[2])state=rp[2];}fclose(f);}
  snprintf(path,sizeof path,"/proc/self/task/%s/wchan",e->d_name);f=fopen(path,"r");
  if(f){if(fgets(channel,sizeof channel,f))channel[strcspn(channel,"\n")]=0;fclose(f);}
  if(!strcmp(comm,"UnityMain")){
   snprintf(path,sizeof path,"/proc/self/task/%s/syscall",e->d_name);f=fopen(path,"r");
   if(f){if(fgets(line,sizeof line,f)){line[strcspn(line,"\n")]=0;require_record(s,fprintf(s->file,"event=thread_syscall tag=%s tid=%s comm=%s syscall=%s\n",tag,e->d_name,comm,line));}fclose(f);}
  }
  require_record(s,fprintf(s->file,"event=thread_list tag=%s tid=%s comm=%s state=%c wchan=%s\n",tag,e->d_name,comm,state,channel));seen++;
 }
 closedir(d);
 perf_ffrt_native_end(1,began);
}
/* Read-only original IL2CPP fields, on the existing UnityMain nativeRender
 * return hook. Never invoke managed business methods or initialize a class. */
static struct {
 void *(*domain)(void);const void **(*assemblies)(void *,size_t *);
 const void *(*image)(const void *);void *(*klass)(const void *,const char *,const char *);
 void *(*field)(void *,const char *);void *(*parent)(void *);void *(*field_owner)(void *);
 void *(*object_class)(void *);void *(*static_data)(void *);
 size_t (*offset)(void *);void (*get)(void *,void *,void *);
 void *title,*client_config;unsigned tick,samples;int attempted,ready;
} title_observer;
static int title_read(void *object,const char *name,void *value){
 if(!object)return 0;
 void *c=title_observer.object_class(object);
 for(unsigned i=0;c&&i<16;i++,c=title_observer.parent(c)){
  void *f=title_observer.field(c,name);if(f){title_observer.get(object,f,value);return 1;}
 }
 return 0;
}
#if defined(LUNARIA_TASK6F_PREFLIGHT) || defined(LUNARIA_TASK6K_OBSERVER)
#include "task6f_download_preflight.inc"
#endif
void unity_player_probe_observe_title_state(void){
#if defined(LUNARIA_TASK6F_PREFLIGHT) || defined(LUNARIA_TASK6K_OBSERVER)
 task6f_preflight_tick();
#endif
 if(perf_jitter_quiet())return;
 if(egl_window_runtime_successful_submissions()<300||(++title_observer.tick>900&&title_observer.tick%60)||title_observer.samples>=990)return;
 title_observer.samples++;
 FILE *out=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt","a");if(!out)return;
 if(!title_observer.attempted){
  title_observer.attempted=1;char error[256]={0};
  elf_loader_t *l=elf_loader_acquire_initialized_cached("/data/storage/el1/bundle/libs/arm64/libil2cpp.so",error,sizeof error);
  if(l){
#define TITLE_API(member,symbol) title_observer.member=(void*)elf_loader_own_executable_symbol(l,symbol)
   TITLE_API(domain,"il2cpp_domain_get");TITLE_API(assemblies,"il2cpp_domain_get_assemblies");
   TITLE_API(image,"il2cpp_assembly_get_image");TITLE_API(klass,"il2cpp_class_from_name");
   TITLE_API(field,"il2cpp_class_get_field_from_name");TITLE_API(parent,"il2cpp_class_get_parent");
   TITLE_API(field_owner,"il2cpp_field_get_parent");
   TITLE_API(object_class,"il2cpp_object_get_class");TITLE_API(static_data,"il2cpp_class_get_static_field_data");
   TITLE_API(offset,"il2cpp_field_get_offset");TITLE_API(get,"il2cpp_field_get_value");
#undef TITLE_API
   title_observer.ready=title_observer.domain&&title_observer.assemblies&&title_observer.image&&title_observer.klass&&title_observer.field&&title_observer.parent&&title_observer.object_class&&title_observer.static_data&&title_observer.offset&&title_observer.get;
   elf_loader_close(l);
  }
  fprintf(out,"TITLE_OBSERVER setup=%d cached_only=1 error=%s\n",title_observer.ready,error);
 }
 if(!title_observer.ready){fclose(out);return;}
 if(!title_observer.title||!title_observer.client_config){
  void *d=title_observer.domain();size_t count=0;const void **assemblies=d?title_observer.assemblies(d,&count):NULL;
  if(assemblies&&count<512)for(size_t i=0;i<count;i++){
   const void *image=title_observer.image(assemblies[i]);
   void *c=image?title_observer.klass(image,"","TitleController"):NULL;
   if(c)title_observer.title=c;
   void *config=image?title_observer.klass(image,"","ClientConfig"):NULL;
   if(config)title_observer.client_config=config;
   if(title_observer.title&&title_observer.client_config)break;
  }
 }
 void *c=title_observer.title,*instance=NULL;
 void *sf=c?title_observer.field(c,"instance"):NULL;
 void *data=c?title_observer.static_data(c):NULL;
 size_t offset=sf?title_observer.offset(sf):SIZE_MAX;
 if(data&&offset<4096)memcpy(&instance,(char*)data+offset,sizeof instance);
 int process=-1,asset=-1;unsigned char auto_login=0,pressed=0;void *button=NULL,*listeners=NULL,*enumerator=NULL;int listener_count=-1,button_state=-1;
 int p=title_read(instance,"processState",&process),a=title_read(instance,"assetBundleLoadState",&asset);
 void *auto_field=c?title_observer.field(c,"isAutoLoginFinished"):NULL;size_t auto_offset=auto_field?title_observer.offset(auto_field):SIZE_MAX;int u=0;
 if(data&&auto_offset<4096){memcpy(&auto_login,(char*)data+auto_offset,1);u=1;}
 int b=title_read(instance,"titleButton",&button);
 int pr=title_read(button,"isPressed",&pressed),bs=title_read(button,"mState",&button_state);
 int lc=title_read(button,"onClick",&listeners)&&title_read(listeners,"_size",&listener_count);
 int en=title_read(instance,"enumerator",&enumerator);struct timespec stamp={0};clock_gettime(CLOCK_MONOTONIC,&stamp);
 void *config=title_observer.client_config;
 void *sdk_field=config?title_observer.field(config,"GameSDKInited"):NULL;
 void *config_data=config?title_observer.static_data(config):NULL;
 size_t sdk_offset=sdk_field?title_observer.offset(sdk_field):SIZE_MAX;int sdk_seen=0;unsigned char sdk_inited=0;
 if(config_data&&sdk_offset<4096){memcpy(&sdk_inited,(char*)config_data+sdk_offset,1);sdk_seen=1;}
 fprintf(out,"CLIENT_CONFIG sample=%u sdk_inited_found=%d GameSDKInited=%u readonly=1\n",title_observer.samples,sdk_seen,(unsigned)sdk_inited);
 fprintf(out,"TITLE_AUTOLOGIN sample=%u storage=static offset=%zu found=%d value=%u readonly=1\n",title_observer.samples,auto_offset,u,(unsigned)auto_login);
 /* Observe the exact array consulted by original WrappersManagerImpl.IsPatched.
  * No managed invocation, class initialization, or patch-table mutation. */
 static void *patch_class;
 if(!patch_class){void *d=title_observer.domain();size_t n=0;const void **as=d?title_observer.assemblies(d,&n):NULL;
  if(as&&n<512)for(size_t i=0;i<n&&!patch_class;i++){const void *im=title_observer.image(as[i]);if(im)patch_class=title_observer.klass(im,"IFix","ILFixDynamicMethodWrapper");}}
 void *patch_field=patch_class?title_observer.field(patch_class,"wrapperArray"):NULL,*patch_data=patch_class?title_observer.static_data(patch_class):NULL,*patch_array=NULL;
 size_t patch_offset=patch_field?title_observer.offset(patch_field):SIZE_MAX,patch_count=0;
 if(patch_data&&patch_offset<4096)memcpy(&patch_array,(char*)patch_data+patch_offset,sizeof patch_array);
 if(patch_array)memcpy(&patch_count,(char*)patch_array+24,sizeof patch_count);
 fprintf(out,"TITLE_PATCH_TABLE sample=%u class_found=%d field_found=%d data_found=%d array_found=%d length=%zu readonly=1\n",title_observer.samples,patch_class!=NULL,patch_field!=NULL,patch_data!=NULL,patch_array!=NULL,patch_count);
 static void *account_class;
 if(!account_class){void *d=title_observer.domain();size_t n=0;const void **as=d?title_observer.assemblies(d,&n):NULL;
  if(as&&n<512)for(size_t i=0;i<n&&!account_class;i++){const void *im=title_observer.image(as[i]);if(im)account_class=title_observer.klass(im,"CE","UserAccountManager");}}
 void *account_field=account_class?title_observer.field(account_class,"instance"):NULL;
 if(!account_field&&account_class)account_field=title_observer.field(account_class,"_instance");
 void *account_owner=account_field&&title_observer.field_owner?title_observer.field_owner(account_field):NULL;
 void *account_static=account_owner?title_observer.static_data(account_owner):NULL,*account=NULL,*account_cache=NULL;
 size_t account_offset=account_field?title_observer.offset(account_field):SIZE_MAX;
 if(account_static&&account_offset<4096)memcpy(&account,(char*)account_static+account_offset,sizeof account);
 int account_state=-1,account_seen=title_read(account,"userAccountLoadState",&account_state),cache_seen=title_read(account,"userAccountDataCache",&account_cache);
 fprintf(out,"TITLE_ACCOUNT sample=%u class_found=%d singleton_field=%d singleton_owner=%d static_data=%d instance=%d state=%d/%d cache=%d/%d readonly=1\n",title_observer.samples,account_class!=NULL,account_field!=NULL,account_owner!=NULL,account_static!=NULL,account!=NULL,account_seen,account_state,cache_seen,account_cache!=NULL);
 const unsigned patch_ids[]={0xd6dd,0xd6e2,0xd723,0xd6f9};
 for(unsigned i=0;i<4;i++){void *wrapper=NULL;unsigned id=patch_ids[i];int seen=patch_array&&patch_count<1000000&&id<patch_count;
  if(seen)memcpy(&wrapper,(char*)patch_array+32+(size_t)id*sizeof(void*),sizeof wrapper);
  fprintf(out,"TITLE_PATCH sample=%u id=%u field_offset=%zu length=%zu seen=%d patched=%d readonly=1\n",title_observer.samples,id,patch_offset,patch_count,seen,wrapper!=NULL);}
 fprintf(out,"TITLE_STATE_TIME sample=%u mono_ms=%lld submissions=%llu enumerator_found=%d enumerator=%p\n",title_observer.samples,(long long)stamp.tv_sec*1000+stamp.tv_nsec/1000000,egl_window_runtime_successful_submissions(),en,enumerator);
 fprintf(out,"TITLE_STATE tid=%ld instance=%p button=%p process=%d/%d asset=%d/%d auto_login=%d/%u pressed=%d/%u button_state=%d/%d listeners=%d/%d readonly=1\n",(long)syscall(SYS_gettid),instance,button,p,process,a,asset,u,(unsigned)auto_login,pr,(unsigned)pressed,bs,button_state,lc,listener_count);
 fclose(out);
}
static int native_event(const char*cls,const char*method,const char*sig,dvm_ref self,const union dvm_value*args,int n,int entering,uintptr_t function_address,void*u){state*s=u;
 if(cls&&method&&!strcmp(cls,"com/unity3d/player/UnityPlayer")&&
    (!strcmp(method,"nativeRender")||!strcmp(method,"nativeRenderOverlay")||!strcmp(method,"nativeResume")||!strcmp(method,"nativePause")||!strcmp(method,"nativeFocusChanged")||!strcmp(method,"nativeRecreateGfxState")||!strcmp(method,"nativeSetSurface"))){
  if(perf_trace_level()>=2)require_record(s,fprintf(s->file,"event=unity_native_call name=%s sig=%s entering=%d thread=%llu\n",method,sig?sig:"?",entering,(unsigned long long)(uintptr_t)pthread_self()));
  if(perf_trace_level()>=2&&entering&&!strcmp(method,"nativeRender"))unity_attach_debug_snapshot("nativeRender_enter");
  return 0;
 }
 if(strcmp(cls,"com/unity3d/player/UnityPlayer")||strcmp(method,"initJni")||strcmp(sig,"(Landroid/content/Context;)V"))return 0;if(entering){int fault_ready=native_fault_probe_begin(s->fault_path,LUNARIA_CONSTRUCTOR_CANDIDATE);require_record(s,fprintf(s->file,"event=fault_capture_ready ready=%d\n",fault_ready));s->init_enter++;s->init_receiver=self;s->init_context=n>0&&args?args[0].l:0;}else {s->init_return++;directory_global_snapshot(s->bridge,"initjni_returned");/* Keep diagnostic fatal capture armed for the real engine threads. */}require_record(s,fprintf(s->file,"event=initJni_%s address=0x%llx receiver=%u context=%u thread=%llu pending=%d exception=%u limited=%u\n",entering?"enter":"returned",(unsigned long long)function_address,self,n>0&&args?args[0].l:0,(unsigned long long)(uintptr_t)pthread_self(),jni_bridge_pending(s->bridge),dvm_exception(s->vm),jni_bridge_limited_count(s->bridge)));return 0;}
static bool lifecycle_call(state *s,const char *name,const char *sig,const union dvm_value *args,int nargs){
 extern void activity_runtime_lifecycle_snapshot(struct dvm *,int *,int *,int *);
 int life0,start0,resume0,life1,start1,resume1;activity_runtime_lifecycle_snapshot(s->vm,&life0,&start0,&resume0);
#ifdef LUNARIA_TASK8A
 task8a_beat(2);atomic_store(&task8a_lifecycle_stage,name);
#endif
 struct dvm_class *cls=dvm_object_class(s->vm,s->activity);
 struct dvm_method *method=cls?dvm_find_method(s->vm,cls,name,sig):NULL;
 require_record(s,fprintf(s->file,"event=activity_callback_enter name=%s receiver=%u resolved=%d bool_arg=%d mono_ms=%lld tid=%ld\n",name,s->activity,method!=NULL,args&&nargs==1&&sig&&!strcmp(sig,"(Z)V")?args[0].i:-1,(long long)({struct timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);(long long)ts.tv_sec*1000+ts.tv_nsec/1000000;}),(long)syscall(SYS_gettid)));
 union dvm_value out={0};bool ok=method&&dvm_call(s->vm,method,s->activity,args,nargs,&out)&&!dvm_exception(s->vm)&&!jni_bridge_pending(s->bridge);
 char error[1024]="-";if(dvm_exception(s->vm))dvm_describe_exception(s->vm,dvm_exception(s->vm),error,sizeof error);
 activity_runtime_lifecycle_snapshot(s->vm,&life1,&start1,&resume1);
 require_record(s,fprintf(s->file,"event=activity_state name=%s before=%d,%d,%d after=%d,%d,%d steps=%llu submissions=%llu mono_ns=%llu\n",name,life0,start0,resume0,life1,start1,resume1,(unsigned long long)s->vm->steps,egl_window_runtime_successful_submissions(),(unsigned long long)perf_now()));
 require_record(s,fprintf(s->file,"event=activity_callback_returned name=%s ok=%d exception=%u pending=%d exception_text=%s\n",name,ok,dvm_exception(s->vm),jni_bridge_pending(s->bridge),error));
#ifdef LUNARIA_TASK8A
 task8a_beat(2);atomic_store(&task8a_lifecycle_stage,"driver");
#endif
 return ok;
}
static bool run_foreground_lifecycle(state *s){
 int showing=0,focused=0;
 if(!foreground_observation(s->vm,&showing,&focused)||!showing){require_record(s,fprintf(s->file,"event=foreground_unavailable no_synthetic_lifecycle=1\n"));return false;}
 require_record(s,fprintf(s->file,"event=host_foreground_observed showing=%d focused=%d diagnostic_snapshot_only=1\n",showing,focused));
 s->start_ok=lifecycle_call(s,"onStart","()V",NULL,0);if(!s->start_ok)return false;
 if(!foreground_observation(s->vm,&showing,&focused)||!showing)return false;
 s->resume_ok=lifecycle_call(s,"onResume","()V",NULL,0);if(!s->resume_ok)return false;
 if(!foreground_observation(s->vm,&showing,&focused)||!showing)return false;
 union dvm_value focus={.i=focused};s->focus_ok=lifecycle_call(s,"onWindowFocusChanged","(Z)V",&focus,1);if(!s->focus_ok)return false;
 // Allow the actual worker to run; never call nativeRender from this driver.
 // A successful EGL swap is recorded only as submission evidence, not a frame.
 log_threads(s,"frame_wait_start");
 /* 60 秒观察窗口：BanG Dream! 首次启动要加载大量资源，30 秒不足以让
  * 加载线程进入首帧提交（72pl 实测 30 秒内 submissions=0 且 AsyncRead/
  * Job.Worker 线程仍在等待）。capture 超时 240 秒，留足余量。 */
 bool submission_reported=false;
 frame_touch_state=s;
 if(!touch_worker_start(s))atomic_store(&touch_worker_enabled,0);
 uint64_t owner_deadline=perf_now()+120000000000ull,owner_controls=0,owner_focus=0,owner_threads=0;
 for(int i=0;perf_now()<owner_deadline;i++){
  uint64_t owner_now=perf_now();
  bool owner_poll_tick=false;
  if(owner_now-owner_controls>=100000000ull){
   owner_controls=owner_now;
   owner_poll_tick=true;
   perf_config_poll(); /* Controls stay <=10Hz even when input wakes owner. */
   play_touch_controls();
  }
  drain_touches(s);
  unsigned long long submitted=egl_window_runtime_successful_submissions();
  if(submitted>s->submission_baseline&&!submission_reported){require_record(s,fprintf(s->file,"event=engine_submission_observed delta=%llu visible_first_frame=unverified\n",submitted-s->submission_baseline));submission_reported=true;}
  unsigned depth=dvm_gil_unlock_all(s->vm);ed_wait_owner();dvm_main_looper_tick(s->vm);dvm_gil_relock(s->vm,depth);
  if(!owner_threads)owner_threads=owner_now;
  if(owner_now-owner_threads>=5000000000ull){owner_threads=owner_now;log_threads(s,"frame_wait_periodic");}
  if(!owner_focus)owner_focus=owner_now;
  if(owner_now-owner_focus>=1000000000ull){
   owner_focus=owner_now;
   int next_focus=0;
   if(!foreground_observation(s->vm,&showing,&next_focus))return false;
   if(!showing){
    require_record(s,fprintf(s->file,"event=host_not_showing stopping_frame_wait=1\n"));
    if(lifecycle_call(s,"onPause","()V",NULL,0))lifecycle_call(s,"onStop","()V",NULL,0);
    return false;
   }
   if(next_focus!=focused){focus.i=next_focus;if(!lifecycle_call(s,"onWindowFocusChanged","(Z)V",&focus,1))return false;focused=next_focus;}
  }
 }
 unity_attach_debug_snapshot("frame_wait_end");
 directory_global_snapshot(s->bridge,"frame_wait_end");
 log_threads(s,"frame_wait_end");
 require_record(s,fprintf(s->file,"event=frame_wait_finished submissions=%llu visible_first_frame=unverified\n",egl_window_runtime_successful_submissions()));
 return true;
}
static int run_activity(void*u){state*s=u;
 ed_start(); /* Dedicated native controls/epoch writer, before Unity startup. */
 /* Startup HAP assertions run before Unity has created a render thread. */
 extern bool mem_long_cache_selftest(struct dvm *),mem_reflection_cache_selftest(struct dvm *);
 if(!mem_long_cache_selftest(s->vm)||!mem_reflection_cache_selftest(s->vm)){
  require_record(s,fprintf(s->file,"event=mem_long_selftest ok=0 startup_fail_closed=1\n"));return -1;
 }

 extern bool file_freeze180_selftest(struct dvm *);
 if(!file_freeze180_selftest(s->vm))return -1;
 extern bool activity_runtime_resume_selftest(struct dvm *);
 if(!activity_runtime_resume_selftest(s->vm))return -1;
 if(!multitouch_selftest(s->vm)||!play_touch_selftest(s->vm)){require_record(s,fprintf(s->file,"event=play_stall_selftest ok=0 startup_fail_closed=1\n"));return -1;}
 extern bool dvm_gil_deep_selftest(struct dvm *);
 if(!dvm_gil_deep_selftest(s->vm)){require_record(s,fprintf(s->file,"event=gil_deep_selftest ok=0 startup_fail_closed=1\n"));return -1;}
 extern bool package_metadata_emu_selftest(struct dvm *);
 if(!package_metadata_emu_selftest(s->vm)){require_record(s,fprintf(s->file,"event=emu_detect_selftest ok=0 startup_fail_closed=1\n"));return -1;}
 extern bool dvm_gil_scheduler_selftest(struct dvm *);
 if(!dvm_gil_scheduler_selftest(s->vm)){require_record(s,fprintf(s->file,"event=gil_scheduler_selftest ok=0 startup_fail_closed=1\n"));return -1;}
 extern bool dvm_gc_foundation_selftest(struct dvm *,const char *);
 if(!dvm_gc_foundation_selftest(s->vm,"/data/storage/el2/base/haps/entry/files/sdkdex/gc1-selftest.txt")){
  require_record(s,fprintf(s->file,"event=gc1_foundation_selftest ok=0 startup_fail_closed=1\n"));return -1;
 }
 extern bool dvm_gc_handles_selftest(struct dvm *,const char *);
 extern bool jni_gc_selftest(struct dvm *,const char *);
 if(!dvm_gc_handles_selftest(s->vm,"/data/storage/el2/base/haps/entry/files/sdkdex/gc2-selftest.txt")||
    !jni_gc_selftest(s->vm,"/data/storage/el2/base/haps/entry/files/sdkdex/gc2-jni-selftest.txt")){
  require_record(s,fprintf(s->file,"event=gc2_handles_selftest ok=0 startup_fail_closed=1\n"));return -1;
 }
 extern bool dvm_gc_microbenchmark(struct dvm *,const char *,const char *);
 if(!dvm_gc_microbenchmark(s->vm,"/data/storage/el2/base/haps/entry/files/sdkdex/gc-perf.csv","gc2")){
  require_record(s,fprintf(s->file,"event=gc2_microbenchmark ok=0 startup_fail_closed=1\n"));return -1;
 }
 require_record(s,fprintf(s->file,"event=gc2_microbenchmark ok=1 workload=dex_int_loop_v1_8x2000 unity_frames_measured=0\n"));
#ifdef LUNARIA_TASK8A
 task8a_beat(2);task8a_watch_start();
 extern bool jni_statfs_reflection_selftest(struct dvm *,const char *);
 if(!jni_statfs_reflection_selftest(s->vm,"/data/storage/el2/base/haps/entry/files/guest-cache")){
  require_record(s,fprintf(s->file,"event=task8a_statfs_selftest ok=0 startup_fail_closed=1\n"));return -1;
 }
#endif
 /* The direct Unity diagnostic bypasses the generic runner's Application
  * branch. Run the original lifecycle before creating its launch Activity. */
 dvm_set_step_limit(s->vm,2000000);
 dvm_ref application=sdk_application_create(s->vm);
 bool app_ok=application&&sdk_application_construct(s->vm,application)&&
  sdk_application_attach(s->vm,application)&&sdk_application_on_create(s->vm,application);
 char app_error[768]="-";if(dvm_exception(s->vm))dvm_describe_exception(s->vm,dvm_exception(s->vm),app_error,sizeof app_error);
 require_record(s,fprintf(s->file,"event=application_lifecycle original=1 receiver=%u ok=%d exception=%u detail=%s\n",application,app_ok,dvm_exception(s->vm),app_error));
 if(!app_ok||dvm_exception(s->vm)||jni_bridge_pending(s->bridge))return -1;
 extern bool activity_runtime_ui_policy_selftest(struct dvm *);
 if(!activity_runtime_ui_policy_selftest(s->vm)){require_record(s,fprintf(s->file,"event=ui_policy_selftest ok=0 startup_fail_closed=1\n"));return -1;}
 /* APK 的启动 Activity 是渠道子类 com.bilibili.star.bili.BaseMainActivity
  * （extends UnityPlayerActivity）。它的 onCreate 构造游戏 Java 桥的
  * UnityAgent 单例，托管层随后通过 UnityAgent.getInstance() 取用；实例化
  * 基类会让 getInstance() 返回 null 并触发托管 NullReferenceException。
  * DEX 未定义该子类时回退基类。 */
 struct dvm_class*cls=dvm_find_class(s->vm,"com/bilibili/star/bili/BaseMainActivity");
 if(!cls)cls=dvm_find_class(s->vm,"com/unity3d/player/UnityPlayerActivity");
 struct dvm_method*ctor=cls?dvm_find_method(s->vm,cls,"<init>","()V"):NULL;struct dvm_method*oncreate=cls?dvm_find_method(s->vm,cls,"onCreate","(Landroid/os/Bundle;)V"):NULL;
 if(!cls||!ctor||!oncreate){require_record(s,fprintf(s->file,"event=failed stage=resolve_activity\n"));return -1;}
 s->activity=dvm_new_object(s->vm,cls);if(!s->activity){require_record(s,fprintf(s->file,"event=failed stage=allocate_activity exception=%u\n",dvm_exception(s->vm)));return -1;}
 require_record(s,fprintf(s->file,"event=activity_ctor_enter receiver=%u\n",s->activity));union dvm_value out={0};s->ctor_ok=dvm_call(s->vm,ctor,s->activity,NULL,0,&out);
 require_record(s,fprintf(s->file,"event=activity_ctor_returned ok=%d exception=%u pending=%d\n",s->ctor_ok,dvm_exception(s->vm),jni_bridge_pending(s->bridge)));
 if(!s->ctor_ok||dvm_exception(s->vm)||jni_bridge_pending(s->bridge))return -1;
 /* This direct Activity diagnostic supplies its own empty launch Intent.
  * It does not reconstruct the original launcher/SDK authentication route. */
 struct dvm_class*intent_class=dvm_find_class_or_external(s->vm,"android/content/Intent");
 dvm_ref intent=intent_class?dvm_new_object(s->vm,intent_class):0;
 if(!intent||!intent_runtime_call(s->vm,"android/content/Intent","<init>","()V",intent,NULL,0,&out)||dvm_exception(s->vm)||!activity_runtime_bind_intent(s->vm,intent)){
  require_record(s,fprintf(s->file,"event=failed stage=bind_diagnostic_launch_intent exception=%u\n",dvm_exception(s->vm)));return -1;
 }
 require_record(s,fprintf(s->file,"event=diagnostic_launch_intent_bound intent=%u empty_extras=1 original_launcher_route=0\n",intent));
 if(!jni_activity_get_intent_selftest(s->vm,s->activity,intent)){
  require_record(s,fprintf(s->file,"event=failed stage=JNI_activity_getIntent_selftest\n"));return -1;
 }
 OH_LOG_Print(LOG_APP,LOG_INFO,0xFF00,"NativeProbeLife","UNITY_ACTIVITY_CREATE pid=%{public}d receiver=%{public}u mono_ns=%{public}llu",getpid(),s->activity,(unsigned long long)perf_now());
 union dvm_value arg={.l=0};memset(&out,0,sizeof(out));require_record(s,fprintf(s->file,"event=activity_onCreate_enter receiver=%u bundle=0\n",s->activity));s->create_ok=dvm_call(s->vm,oncreate,s->activity,&arg,1,&out);
 require_record(s,fprintf(s->file,"event=activity_onCreate_returned ok=%d exception=%u pending=%d\n",s->create_ok,dvm_exception(s->vm),jni_bridge_pending(s->bridge)));if(!s->create_ok||dvm_exception(s->vm)||jni_bridge_pending(s->bridge))return -1;
 /* PermissionActivity normally invokes this init before launching Unity. The
  * direct probe supplies the same original resource parameters and the Sdk's
  * own callbacks on its actual Activity; no readiness flag is substituted. */
 {
  union dvm_value sdk={0},init_cb={0},exit_cb={0},instance={0},params[7]={{.l=s->activity}};
  bool ready=dvm__get_field_from(s->vm,s->activity,cls,"mSdk","Lcom/bilibili/star/bili/Sdk;",&sdk);
  struct dvm_class *sc=sdk.l?dvm_object_class(s->vm,sdk.l):NULL;
  ready=ready&&sc&&dvm__get_field_from(s->vm,sdk.l,sc,"mInitHandler","Lcom/bsgamesdk/android/callbacklistener/InitCallbackListener;",&init_cb)&&
   dvm__get_field_from(s->vm,sdk.l,sc,"mExitCallbackListener","Lcom/bsgamesdk/android/callbacklistener/ExitCallbackListener;",&exit_cb)&&init_cb.l&&exit_cb.l;
  const int ids[]={0x7f0d01ad,0x7f0d01aa,0x7f0d01ae,0x7f0d01ab};
  socket_runtime_set_external(true);
  require_record(s,fprintf(s->file,"event=java_socket_network_policy external_enabled=%d reason=authorized_original_sdk_business_validation\n",socket_runtime_external_allowed()));
  int resource_count=0;
  for(int i=0;ready&&i<4;i++){union dvm_value id={.i=ids[i]};ready=platform_runtime_call(s->vm,"android/content/Context","getString","(I)Ljava/lang/String;",s->activity,&id,1,&params[i+1])&&params[i+1].l&&!dvm_exception(s->vm);if(ready)resource_count++;}
  params[5]=init_cb;params[6]=exit_cb;
  struct dvm_method *get=dvm_lookup(s->vm,"com/gsc/pub/GSCPubCommon","getInstance","()Lcom/gsc/pub/GSCPubCommon;");
  struct dvm_method *init=dvm_lookup(s->vm,"com/gsc/pub/GSCPubCommon","init","(Landroid/app/Activity;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Lcom/bsgamesdk/android/callbacklistener/InitCallbackListener;Lcom/bsgamesdk/android/callbacklistener/ExitCallbackListener;)V");
  ready=ready&&get&&init&&dvm_call(s->vm,get,0,NULL,0,&instance)&&instance.l&&dvm_call(s->vm,init,instance.l,params,7,&out)&&!dvm_exception(s->vm);
  char why[768]="-";if(dvm_exception(s->vm))dvm_describe_exception(s->vm,dvm_exception(s->vm),why,sizeof why);
  require_record(s,fprintf(s->file,"event=original_sdk_init dispatched=%d sdk=%u init_callback=%u exit_callback=%u resources=%d get_resolved=%d init_resolved=%d exception=%u detail=%s\n",ready,sdk.l,init_cb.l,exit_cb.l,resource_count,get!=NULL,init!=NULL,dvm_exception(s->vm),why));
  if(!ready||jni_bridge_pending(s->bridge))return -1;
 }
 {union dvm_value manager={0},routes={0},count={0};struct dvm_method *get=dvm_lookup(s->vm,"com/gsc/base/d","c","()Lcom/gsc/base/d;"),*parse=dvm_lookup(s->vm,"com/gsc/base/d","h","()Ljava/util/List;");bool ok=get&&parse&&dvm_call(s->vm,get,0,NULL,0,&manager)&&manager.l&&dvm_call(s->vm,parse,manager.l,NULL,0,&routes)&&routes.l&&!dvm_exception(s->vm);struct dvm_class *route_class=ok?dvm_object_class(s->vm,routes.l):NULL;struct dvm_method *size=route_class?dvm_find_method(s->vm,route_class,"size","()I"):NULL;ok=ok&&(size?dvm_call(s->vm,size,routes.l,NULL,0,&count):collections_call(s->vm,"java/util/List","size","()I",routes.l,NULL,0,&count))&&!dvm_exception(s->vm);char why[768]="-";if(dvm_exception(s->vm))dvm_describe_exception(s->vm,dvm_exception(s->vm),why,sizeof why);require_record(s,fprintf(s->file,"event=login_route_parse_diagnostic ok=%d manager=%u list=%u class=%s size=%d exception=%u detail=%s diagnostic_only=1\n",ok,manager.l,routes.l,route_class?route_class->name:"-",ok?count.i:-1,dvm_exception(s->vm),why));if(!ok&&dvm_exception(s->vm))return -1;}
 {uintptr_t ub=0;FILE*mf=fopen("/proc/self/maps","r");if(mf){char line[512];while(fgets(line,sizeof line,mf)){if(strstr(line,"libunity.so")){unsigned long long a=0;if(sscanf(line,"%llx",&a)==1){ub=(uintptr_t)a;break;}}}fclose(mf);}
  int inited=-1;if(ub){inited=(int)*(volatile unsigned char*)(ub+0x1885918);}
  require_record(s,fprintf(s->file,"event=unity_global_state base=0x%llx init_flag_918=%d\n",(unsigned long long)ub,inited));}
 directory_global_snapshot(s->bridge,"after_oncreate");
 struct dvm_class *w0=dvm_find_class(s->vm,"com/unity3d/player/W0");struct dvm_method *wd=w0?dvm_find_method(s->vm,w0,"d","()Z"):NULL;union dvm_value wv={0};bool wcall=wd&&dvm_call(s->vm,wd,0,NULL,0,&wv)&&!dvm_exception(s->vm);
 require_record(s,fprintf(s->file,"event=w0_state d_resolved=%d called=%d native_loaded=%d exception=%u\n",wd!=NULL,wcall,wcall?(wv.i!=0):-1,dvm_exception(s->vm)));
 struct dvm_class *ac=dvm_find_class(s->vm,"com/unity3d/player/UnityPlayerActivity");struct dvm_class *up=dvm_find_class(s->vm,"com/unity3d/player/UnityPlayer");
 union dvm_value player={0},mmo={0},ms={0};
 bool pg=ac&&up&&dvm__get_field_from(s->vm,s->activity,ac,"mUnityPlayer","Lcom/unity3d/player/UnityPlayer;",&player)&&!dvm_exception(s->vm);
 bool mg=pg&&player.l&&dvm__get_field_from(s->vm,player.l,up,"mMainDisplayOverride","Z",&mmo)&&!dvm_exception(s->vm);
 bool sg=pg&&player.l&&dvm__get_field_from(s->vm,player.l,up,"mState","Lcom/unity3d/player/W0;",&ms)&&!dvm_exception(s->vm);
 struct dvm_method *wa=sg&&ms.l?dvm_find_method(s->vm,dvm_object_class(s->vm,ms.l),"a","()Z"):NULL;union dvm_value wav={0};bool waok=wa&&dvm_call(s->vm,wa,ms.l,NULL,0,&wav)&&!dvm_exception(s->vm);
 require_record(s,fprintf(s->file,"event=player_state main_display_override=%d mmo_read=%d state_ref=%d state_a_resolved=%d state_a=%d exception=%u\n",mg?mmo.i:-1,mg,sg&&ms.l!=0,wa!=NULL,waok?(wav.i!=0):-1,dvm_exception(s->vm)));
 {union dvm_value mtref={0},hcur={0};
  bool mtread=pg&&player.l&&up&&dvm__get_field_from(s->vm,player.l,up,"m_MainThread","Lcom/unity3d/player/U0;",&mtref)&&!dvm_exception(s->vm)&&mtref.l;
  struct dvm_class *u0c=mtread?dvm_object_class(s->vm,mtref.l):NULL;
  bool hread=u0c&&dvm__get_field_from(s->vm,mtref.l,u0c,"a","Landroid/os/Handler;",&hcur)&&!dvm_exception(s->vm);
  require_record(s,fprintf(s->file,"event=main_thread_handler main_thread_read=%d main_thread=%u field_read=%d handler=%u class=%s exception=%u\n",mtread,(unsigned)mtref.l,hread,(unsigned)hcur.l,u0c&&u0c->name?u0c->name:"?",dvm_exception(s->vm)));}
 return run_foreground_lifecycle(s)?0:-1;}
static int with_native(void*u){state*s=u;return jni_bridge_with_native_observer(native_event,s,run_activity,s);}
static int with_dlopen(void*u){state*s=u;return elf_android_dlfcn_observe(dl_event,s,with_native,s);}
int unity_player_probe_run(struct dvm*vm,const char*dir,const char*reportdir){
 if(reportdir){char ep[4096];int en=snprintf(ep,sizeof ep,"%s/engine-probe.txt",reportdir);if(en>0&&(size_t)en<sizeof ep){FILE *ef=fopen(ep,"a");if(ef){fprintf(ef,"ENGINE_PROBE_BEGIN candidate=%s pid=%ld\n",LUNARIA_CONSTRUCTOR_CANDIDATE,(long)getpid());fclose(ef);}}}
 if(!vm||!dir||!*dir||!reportdir||!*reportdir||atomic_flag_test_and_set(&attempted))return -1;char path[4096],why[1024];int n=snprintf(path,sizeof path,"%s/unity-player-constructor.txt",reportdir);if(n<0||(size_t)n>=sizeof path)return -1;
 /* A contended diagnostic cookie is intentionally lossy. Its absence must
  * never veto engine startup. The fallback is opened once, outside rendering. */
 FILE*f=fopen(path,"w");int diagnostic_sink=f==NULL;if(!f)f=fopen("/dev/null","w");if(!f)return -1;state s={.file=f,.vm=vm,.bridge=jni_bridge_for_vm(vm)};int ok=0;
 n=snprintf(s.fault_path,sizeof s.fault_path,"%s/unity-native-fault.txt",reportdir);if(n<0||(size_t)n>=sizeof s.fault_path){fclose(f);return -1;}
 require_record(&s,fprintf(f,"event=begin candidate=%s pid=%ld thread=%llu library_dir=%s host_ui_queue_bound=0 engine_may_start=1\n",LUNARIA_CONSTRUCTOR_CANDIDATE,(long)getpid(),(unsigned long long)(uintptr_t)pthread_self(),dir));
 const char *failure_stage="bridge_or_main_registration";
 if(!s.bridge||!jni_bridge_thread_is_attached(s.bridge)||jni_bridge_pending(s.bridge)||dvm_exception(vm)||!jni_bridge_native_registered(vm,"com/unity3d/player/NativeLoader","load","(Ljava/lang/String;)Z")||!jni_bridge_native_registered(vm,"com/unity3d/player/NativeLoader","unload","()Z")){require_record(&s,fprintf(f,"event=failed stage=bridge_or_main_registration\n"));goto done;}
 extern bool click_up_class_selftest(struct dvm *);
 failure_stage="click_up_class_selftest";
 if(!click_up_class_selftest(s.vm)){require_record(&s,fprintf(f,"event=failed stage=click_up_class_selftest\n"));goto done;}
 extern bool jni_gaps_selftest(struct dvm *);
 failure_stage="jni_gaps_selftest";
 if(!jni_gaps_selftest(s.vm)){require_record(&s,fprintf(f,"event=failed stage=jni_gaps_selftest\n"));goto done;}
 extern bool jni_exception_copy_selftest(void);
 failure_stage="jni_exception_copy_selftest";
 if(!jni_exception_copy_selftest()){require_record(&s,fprintf(f,"event=failed stage=jni_exception_copy_selftest\n"));goto done;}
 failure_stage="JNI_forName_selftest";
 if(!jni_forname_selftest(jni_bridge_env(s.bridge))){require_record(&s,fprintf(f,"event=failed stage=JNI_forName_selftest\n"));goto done;}
 extern void jni_sweep_start(struct dvm *);
 jni_sweep_start(s.vm);
 failure_stage="library_already_initialized";
 for(int i=0;i<2;i++){n=snprintf(path,sizeof path,"%s/%s",dir,i?"libil2cpp.so":"libunity.so");if(n<0||(size_t)n>=sizeof path)goto done;elf_loader_t*l=elf_loader_acquire_initialized_cached(path,why,sizeof why);if(l){elf_loader_close(l);require_record(&s,fprintf(f,"event=failed stage=library_already_initialized path=%s\n",path));goto done;}}
 failure_stage="guest_main_looper";
 union dvm_value looper={0},main_looper={0};if(!android_basics_call(vm,"android/os/Looper","myLooper","()Landroid/os/Looper;",0,NULL,0,&looper)||!android_basics_call(vm,"android/os/Looper","getMainLooper","()Landroid/os/Looper;",0,NULL,0,&main_looper)||!looper.l||looper.l!=main_looper.l||dvm_exception(vm)){require_record(&s,fprintf(f,"event=failed stage=guest_main_looper\n"));goto done;}
 s.submission_baseline=egl_window_runtime_successful_submissions();
 s.limited_before=jni_bridge_limited_count(s.bridge);require_record(&s,fprintf(f,"event=guest_looper_ready looper=%u host_ui_queue_bound=0 registrations=%u limited=%u\n",looper.l,jni_bridge_native_registration_count(vm),s.limited_before));
 failure_stage="constructor_lifecycle";
 (void)elf_loader_with_init_observer(init_event,&s,with_dlopen,&s);
 ok=s.ctor_ok&&s.create_ok&&s.start_ok&&s.resume_ok&&s.focus_ok&&s.init_enter==1&&s.init_return==1&&s.init_receiver&&s.init_context==s.activity&&!dvm_exception(vm)&&!jni_bridge_pending(s.bridge)&&jni_bridge_limited_count(s.bridge)==s.limited_before;
 done: touch_worker_finish(&s);frame_touch_state=NULL;{char description[1024]="-";dvm_ref exc=dvm_exception(vm);if(exc)dvm_describe_exception(vm,exc,description,sizeof description);require_record(&s,fprintf(f,"event=probe_result result=%s activity=%u init_enter=%u init_return=%u exception=%u exception_text=%s pending=%d limited_before=%u limited_after=%u host_ui_queue_bound=0 engine_status=unknown\n",ok?"pass":"fail",s.activity,s.init_enter,s.init_return,exc,description,s.bridge?jni_bridge_pending(s.bridge):-1,s.limited_before,s.bridge?jni_bridge_limited_count(s.bridge):0));}
 /* Cookie close may report EBADF on this libc even when queue writes worked.
  * Diagnostic persistence is not an engine acceptance condition. */
 (void)fclose(f);
 extern void perf_diag_connectivity_detail(const char *,const char *);
 char startup_detail[256];snprintf(startup_detail,sizeof startup_detail,"result=%s failure_stage=%s diagnostic_sink=%d failed_writes=%u ctor=%d create=%d start=%d resume=%d focus=%d pending=%d exception=%u",ok?"pass":"fail",ok?"none":failure_stage,diagnostic_sink,atomic_load(&probe_diagnostic_failures),s.ctor_ok,s.create_ok,s.start_ok,s.resume_ok,s.focus_ok,s.bridge?jni_bridge_pending(s.bridge):-1,(unsigned)dvm_exception(vm));
 perf_diag_connectivity_detail("unity_startup_result",startup_detail);
 return ok?0:-1;
}
