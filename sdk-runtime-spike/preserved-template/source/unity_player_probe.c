#define _GNU_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#include "unity_player_probe.h"
#include "native_fault_probe.h"
#include "android_dlfcn.h"
#include "elf_loader.h"
#include "jni_bridge.h"
#include "android_basics.h"
#include "activity_runtime.h"
#include "intent_runtime.h"
#include "egl_window_runtime.h"
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
#include <hilog/log.h>
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
 return status==0&&(*showing==0||*showing==1)&&(*focused==0||*focused==1);
}

typedef struct {char fault_path[4096];FILE *file;struct dvm*vm;struct jni_bridge*bridge;dvm_ref activity;int ctor_ok,create_ok,start_ok,resume_ok,focus_ok;unsigned init_enter,init_return;dvm_ref init_receiver,init_context;uint32_t limited_before;unsigned long long submission_baseline;} state;

static void require_record(state*s,int n);
/* ---------------- 宿主触摸注入（XComponent → Unity nativeInjectEvent） ----------------
 * 队列由宿主 XComponent 的 Touch 回调（ArkTS 主线程）写入，VM worker 在
 * frame_wait 的 100ms tick 中消费：构造 MotionEvent 快照并调用
 * UnityPlayer.nativeInjectEvent(InputEvent,int)。该方法由 libunity 通过
 * RegisterNatives 注册（sig (Landroid/view/InputEvent;I)Z），dvm 的
 * call_native hook 会进入真实实现。 */
#define TOUCH_QUEUE_CAP 64
typedef struct { int action,pointer_id; float x,y; int64_t event_ms; } queued_touch;
static pthread_mutex_t touch_lock=PTHREAD_MUTEX_INITIALIZER;
static queued_touch touch_queue[TOUCH_QUEUE_CAP];
static unsigned touch_head,touch_tail;
static uint64_t touch_queued,touch_dropped,touch_injected,touch_failed;
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
__attribute__((visibility("default"))) int sdk_input_queue_touch(int action,int pointer_id,float x,float y,int64_t event_time_ns){
 int64_t event_ms=event_time_ns>0?event_time_ns/1000000:0;
 pthread_mutex_lock(&touch_lock);
 unsigned next=(touch_tail+1)%TOUCH_QUEUE_CAP;
 if(next==touch_head){touch_dropped++;pthread_mutex_unlock(&touch_lock);
  OH_LOG_Print(LOG_APP,LOG_INFO,0xFF00,"NativeProbeTouch","TOUCHQ host=0 action=%{public}d dropped=%{public}llu full=1",action,(unsigned long long)touch_dropped);
  return -1;}
 queued_touch *q=&touch_queue[touch_tail];
 q->action=action;q->pointer_id=pointer_id;q->x=x;q->y=y;q->event_ms=event_ms;
 touch_tail=next;touch_queued++;
 pthread_mutex_unlock(&touch_lock);
 OH_LOG_Print(LOG_APP,LOG_INFO,0xFF00,"NativeProbeTouch","TOUCHQ host=0 action=%{public}d id=%{public}d queued=%{public}llu dropped=%{public}llu",action,pointer_id,(unsigned long long)touch_queued,(unsigned long long)touch_dropped);
 return 0;
}
static bool inject_touch(struct dvm *vm,dvm_ref player,const queued_touch *q,int64_t down_ms,int *consumed){
 struct dvm_class *up=dvm_find_class(vm,"com/unity3d/player/UnityPlayer");
 struct dvm_method *m=up?dvm_find_method(vm,up,"nativeInjectEvent","(Landroid/view/InputEvent;I)Z"):NULL;
 if(!m){if(consumed)*consumed=0;return false;}
 dvm_ref me=sdk_motion_event_create(vm,q->action,q->pointer_id,q->x,q->y,4098 /*InputDevice.SOURCE_TOUCHSCREEN*/,down_ms,q->event_ms);
 if(!me||dvm_exception(vm)){if(consumed)*consumed=0;return false;}
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
 union dvm_value args[2]={ {.l=me}, {.i=0} }, out={0};
 bool ok=dvm_call(vm,m,player,args,2,&out)&&!dvm_exception(vm);
 dvm_unpin(vm,me);
 if(consumed)*consumed=out.i?1:0;
 return ok;
}
static void drain_touches(state *s){
 static int64_t current_down_ms;
 int drained=0,last_player=0,consumed_ok=0;
 for(;;){
  queued_touch q;
  pthread_mutex_lock(&touch_lock);
  if(touch_head==touch_tail){pthread_mutex_unlock(&touch_lock);break;}
  q=touch_queue[touch_head];touch_head=(touch_head+1)%TOUCH_QUEUE_CAP;
  pthread_mutex_unlock(&touch_lock);
  int action=q.action&0xff;
  if(action==0)current_down_ms=q.event_ms; /* ACTION_DOWN */
  dvm_ref player=0;
  struct dvm_class *ac=dvm_find_class(s->vm,"com/unity3d/player/UnityPlayerActivity");
  union dvm_value pv={0};
  if(s->activity&&ac&&dvm__get_field_from(s->vm,s->activity,ac,"mUnityPlayer","Lcom/unity3d/player/UnityPlayer;",&pv)&&!dvm_exception(s->vm)&&pv.l)player=pv.l;
  int consumed=0;
  /* Inject TAP 的 DOWN/UP 共用同一 event_ns：UP 的 eventTime 至少比
   * downTime 晚 80ms，避免引擎把零时长点击判为无效。 */
  queued_touch qq=q;
  if((action==1||action==3)&&current_down_ms&&qq.event_ms<=current_down_ms)qq.event_ms=current_down_ms+80;
  bool ok=player&&inject_touch(s->vm,player,&qq,current_down_ms?current_down_ms:qq.event_ms,&consumed);
  last_player=player?1:0;drained++;consumed_ok+=consumed;
  if(ok)touch_injected++;else touch_failed++;
  if(action==1||action==3)current_down_ms=0; /* UP/CANCEL */
  /* Inject TAP 合成的 DOWN/UP 使用同一 event_ms；若同一 tick 连续注入，
   * Unity 的 uGUI 会把按下与抬起合并。DOWN 之后若队首是同一 event_ms 的
   * UP，本 tick 停止，让 UP 在下一个 100ms tick 注入。 */
  if(action==0){
   int split=0;
   pthread_mutex_lock(&touch_lock);
   if(touch_head!=touch_tail&&(touch_queue[touch_head].action&0xff)==1&&touch_queue[touch_head].event_ms==q.event_ms)split=1;
   pthread_mutex_unlock(&touch_lock);
   if(split)break;
  }
  if(touch_injected+touch_failed<=64){
   FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt","a");
   if(f){fprintf(f,"TOUCH_INJECT action=%d id=%d x=%.1f y=%.1f down=%lld event=%lld ok=%d consumed=%d player=%u queued=%llu injected=%llu failed=%llu dropped=%llu\n",q.action,q.pointer_id,q.x,q.y,(long long)(current_down_ms?current_down_ms:q.event_ms),(long long)q.event_ms,ok?1:0,consumed,(unsigned)player,(unsigned long long)touch_queued,(unsigned long long)touch_injected,(unsigned long long)touch_failed,(unsigned long long)touch_dropped);fclose(f);}
   if(s&&s->file)require_record(s,fprintf(s->file,"event=touch_injected action=%d id=%d x=%.1f y=%.1f ok=%d consumed=%d injected=%llu failed=%llu\n",q.action,q.pointer_id,q.x,q.y,ok?1:0,consumed,(unsigned long long)touch_injected,(unsigned long long)touch_failed));
  }
 }
 if(drained)OH_LOG_Print(LOG_APP,LOG_INFO,0xFF00,"NativeProbeTouch","TOUCHDRAIN host=0 drained=%{public}d player=%{public}d consumed=%{public}d injected=%{public}llu failed=%{public}llu queued=%{public}llu dropped=%{public}llu",drained,last_player,consumed_ok,(unsigned long long)touch_injected,(unsigned long long)touch_failed,(unsigned long long)touch_queued,(unsigned long long)touch_dropped);
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
void unity_player_probe_on_native_render(void){
    static _Atomic unsigned started;
    if(atomic_fetch_add(&started,1)!=0)return;
    unity_player_probe_build_class();
    pthread_t th;
    int rc=pthread_create(&th,NULL,native_render_watchdog,(void*)(intptr_t)syscall(SYS_gettid));
    { int e=errno; FILE*o=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt","a");
      if(o){fprintf(o,"NATIVE_RENDER_WATCHDOG_CREATE rc=%d errno=%d\n",rc,rc?e:0);fclose(o);} }
    if(rc==0)pthread_detach(th);
}
static int sync_report(state*s){int bad=fflush(s->file)!=0;if(fsync(fileno(s->file)))bad=1;return bad;}
static void require_record(state*s,int n){if(n<0||sync_report(s))abort();}
static int init_event(const char*path,size_t index,uintptr_t address,int entering,void*u){state*s=u;require_record(s,fprintf(s->file,"event=elf_constructor_%s path=%s index=%zu address=0x%llx thread=%llu\n",entering?"enter":"returned",path,index,(unsigned long long)address,(unsigned long long)(uintptr_t)pthread_self()));return 0;}
static int dl_event(const char*event,const char*path,uintptr_t address,int value,void*u){state*s=u;require_record(s,fprintf(s->file,"event=dl_%s path=%s address=0x%llx value=%d thread=%llu\n",event,path,(unsigned long long)address,value,(unsigned long long)(uintptr_t)pthread_self()));return 0;}
static void log_threads(state*s,const char*tag){
 DIR *d=opendir("/proc/self/task");if(!d)return;struct dirent *e;int seen=0;
 while((e=readdir(d))&&seen<64){if(e->d_name[0]=='.')continue;char path[128];if(snprintf(path,sizeof path,"/proc/self/task/%s/comm",e->d_name)>=(int)sizeof path)continue;FILE*f=fopen(path,"r");if(!f)continue;char comm[64]={0};if(fgets(comm,sizeof comm,f)){char*nl=strchr(comm,'\n');if(nl)*nl=0;char statp[128],state='?';long wchan=0;if(snprintf(statp,sizeof statp,"/proc/self/task/%s/stat",e->d_name)<(int)sizeof statp){FILE*sf=fopen(statp,"r");if(sf){char line[512]={0};if(fgets(line,sizeof line,sf)){char*rp=strrchr(line,')');if(rp&&rp[1]==' '&&rp[2])state=rp[2];}fclose(sf);}}if(snprintf(statp,sizeof statp,"/proc/self/task/%s/wchan",e->d_name)<(int)sizeof statp){FILE*wf=fopen(statp,"r");if(wf){char wl[128]={0};if(fgets(wl,sizeof wl,wf)){wl[strcspn(wl,"\n")]=0;}fclose(wf);if(!strcmp(comm,"UnityMain")){if(snprintf(statp,sizeof statp,"/proc/self/task/%s/syscall",e->d_name)<(int)sizeof statp){FILE*scf=fopen(statp,"r");if(scf){char sl[256]={0};if(fgets(sl,sizeof sl,scf)){sl[strcspn(sl,"\n")]=0;require_record(s,fprintf(s->file,"event=thread_syscall tag=%s tid=%s comm=%s syscall=%s\n",tag,e->d_name,comm,sl));}fclose(scf);}}}require_record(s,fprintf(s->file,"event=thread_list tag=%s tid=%s comm=%s state=%c wchan=%s\n",tag,e->d_name,comm,state,wl));seen++;continue;}}require_record(s,fprintf(s->file,"event=thread_list tag=%s tid=%s comm=%s state=%c\n",tag,e->d_name,comm,state));seen++;}fclose(f);}
 closedir(d);
}
static int native_event(const char*cls,const char*method,const char*sig,dvm_ref self,const union dvm_value*args,int n,int entering,uintptr_t function_address,void*u){state*s=u;
 if(cls&&method&&!strcmp(cls,"com/unity3d/player/UnityPlayer")&&
    (!strcmp(method,"nativeRender")||!strcmp(method,"nativeRenderOverlay")||!strcmp(method,"nativeResume")||!strcmp(method,"nativePause")||!strcmp(method,"nativeFocusChanged")||!strcmp(method,"nativeRecreateGfxState")||!strcmp(method,"nativeSetSurface"))){
  require_record(s,fprintf(s->file,"event=unity_native_call name=%s sig=%s entering=%d thread=%llu\n",method,sig?sig:"?",entering,(unsigned long long)(uintptr_t)pthread_self()));
  if(entering&&!strcmp(method,"nativeRender"))unity_attach_debug_snapshot("nativeRender_enter");
  return 0;
 }
 if(strcmp(cls,"com/unity3d/player/UnityPlayer")||strcmp(method,"initJni")||strcmp(sig,"(Landroid/content/Context;)V"))return 0;if(entering){int fault_ready=native_fault_probe_begin(s->fault_path,LUNARIA_CONSTRUCTOR_CANDIDATE);require_record(s,fprintf(s->file,"event=fault_capture_ready ready=%d\n",fault_ready));s->init_enter++;s->init_receiver=self;s->init_context=n>0&&args?args[0].l:0;}else {s->init_return++;directory_global_snapshot(s->bridge,"initjni_returned");/* Keep diagnostic fatal capture armed for the real engine threads. */}require_record(s,fprintf(s->file,"event=initJni_%s address=0x%llx receiver=%u context=%u thread=%llu pending=%d exception=%u limited=%u\n",entering?"enter":"returned",(unsigned long long)function_address,self,n>0&&args?args[0].l:0,(unsigned long long)(uintptr_t)pthread_self(),jni_bridge_pending(s->bridge),dvm_exception(s->vm),jni_bridge_limited_count(s->bridge)));return 0;}
static bool lifecycle_call(state *s,const char *name,const char *sig,const union dvm_value *args,int nargs){
 struct dvm_class *cls=dvm_object_class(s->vm,s->activity);
 struct dvm_method *method=cls?dvm_find_method(s->vm,cls,name,sig):NULL;
 require_record(s,fprintf(s->file,"event=activity_callback_enter name=%s receiver=%u resolved=%d\n",name,s->activity,method!=NULL));
 union dvm_value out={0};bool ok=method&&dvm_call(s->vm,method,s->activity,args,nargs,&out)&&!dvm_exception(s->vm)&&!jni_bridge_pending(s->bridge);
 require_record(s,fprintf(s->file,"event=activity_callback_returned name=%s ok=%d exception=%u pending=%d\n",name,ok,dvm_exception(s->vm),jni_bridge_pending(s->bridge)));
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
 for(int i=0;i<600;i++){
  drain_touches(s);
  unsigned long long submitted=egl_window_runtime_successful_submissions();
  if(submitted>s->submission_baseline&&!submission_reported){require_record(s,fprintf(s->file,"event=engine_submission_observed delta=%llu visible_first_frame=unverified\n",submitted-s->submission_baseline));submission_reported=true;}
  unsigned depth=dvm_gil_unlock_all(s->vm);struct timespec wait={.tv_sec=0,.tv_nsec=100000000};nanosleep(&wait,NULL);dvm_main_looper_tick(s->vm);dvm_gil_relock(s->vm,depth);
  if(i%50==49){char t50[32];snprintf(t50,sizeof t50,"frame_wait_%ds",(i+1)/10);log_threads(s,t50);}
  if(i%10==9){
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
 union dvm_value arg={.l=0};memset(&out,0,sizeof(out));require_record(s,fprintf(s->file,"event=activity_onCreate_enter receiver=%u bundle=0\n",s->activity));s->create_ok=dvm_call(s->vm,oncreate,s->activity,&arg,1,&out);
 require_record(s,fprintf(s->file,"event=activity_onCreate_returned ok=%d exception=%u pending=%d\n",s->create_ok,dvm_exception(s->vm),jni_bridge_pending(s->bridge)));if(!s->create_ok||dvm_exception(s->vm)||jni_bridge_pending(s->bridge))return -1;
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
 if(!vm||!dir||!*dir||!reportdir||!*reportdir||atomic_flag_test_and_set(&attempted))return -1;char path[4096],why[1024];int n=snprintf(path,sizeof path,"%s/unity-player-constructor.txt",reportdir);if(n<0||(size_t)n>=sizeof path)return -1;FILE*f=fopen(path,"w");if(!f)return -1;state s={.file=f,.vm=vm,.bridge=jni_bridge_for_vm(vm)};int ok=0;
 n=snprintf(s.fault_path,sizeof s.fault_path,"%s/unity-native-fault.txt",reportdir);if(n<0||(size_t)n>=sizeof s.fault_path){fclose(f);return -1;}
 require_record(&s,fprintf(f,"event=begin candidate=%s pid=%ld thread=%llu library_dir=%s host_ui_queue_bound=0 engine_may_start=1\n",LUNARIA_CONSTRUCTOR_CANDIDATE,(long)getpid(),(unsigned long long)(uintptr_t)pthread_self(),dir));
 if(!s.bridge||!jni_bridge_thread_is_attached(s.bridge)||jni_bridge_pending(s.bridge)||dvm_exception(vm)||!jni_bridge_native_registered(vm,"com/unity3d/player/NativeLoader","load","(Ljava/lang/String;)Z")||!jni_bridge_native_registered(vm,"com/unity3d/player/NativeLoader","unload","()Z")){require_record(&s,fprintf(f,"event=failed stage=bridge_or_main_registration\n"));goto done;}
 if(!jni_forname_selftest(jni_bridge_env(s.bridge))){require_record(&s,fprintf(f,"event=failed stage=JNI_forName_selftest\n"));goto done;}
 for(int i=0;i<2;i++){n=snprintf(path,sizeof path,"%s/%s",dir,i?"libil2cpp.so":"libunity.so");if(n<0||(size_t)n>=sizeof path)goto done;elf_loader_t*l=elf_loader_acquire_initialized_cached(path,why,sizeof why);if(l){elf_loader_close(l);require_record(&s,fprintf(f,"event=failed stage=library_already_initialized path=%s\n",path));goto done;}}
 union dvm_value looper={0},main_looper={0};if(!android_basics_call(vm,"android/os/Looper","myLooper","()Landroid/os/Looper;",0,NULL,0,&looper)||!android_basics_call(vm,"android/os/Looper","getMainLooper","()Landroid/os/Looper;",0,NULL,0,&main_looper)||!looper.l||looper.l!=main_looper.l||dvm_exception(vm)){require_record(&s,fprintf(f,"event=failed stage=guest_main_looper\n"));goto done;}
 s.submission_baseline=egl_window_runtime_successful_submissions();
 s.limited_before=jni_bridge_limited_count(s.bridge);require_record(&s,fprintf(f,"event=guest_looper_ready looper=%u host_ui_queue_bound=0 registrations=%u limited=%u\n",looper.l,jni_bridge_native_registration_count(vm),s.limited_before));
 (void)elf_loader_with_init_observer(init_event,&s,with_dlopen,&s);
 ok=s.ctor_ok&&s.create_ok&&s.start_ok&&s.resume_ok&&s.focus_ok&&s.init_enter==1&&s.init_return==1&&s.init_receiver&&s.init_context==s.activity&&!dvm_exception(vm)&&!jni_bridge_pending(s.bridge)&&jni_bridge_limited_count(s.bridge)==s.limited_before;
 done: {char description[1024]="-";dvm_ref exc=dvm_exception(vm);if(exc)dvm_describe_exception(vm,exc,description,sizeof description);require_record(&s,fprintf(f,"event=probe_result result=%s activity=%u init_enter=%u init_return=%u exception=%u exception_text=%s pending=%d limited_before=%u limited_after=%u host_ui_queue_bound=0 engine_status=unknown\n",ok?"pass":"fail",s.activity,s.init_enter,s.init_return,exc,description,s.bridge?jni_bridge_pending(s.bridge):-1,s.limited_before,s.bridge?jni_bridge_limited_count(s.bridge):0));}
 if(fclose(f))ok=0;return ok?0:-1;
}
