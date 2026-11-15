#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1 /* bionic declares gettid() only under _GNU_SOURCE */
#endif
#include "sqlite_runtime.h"
#include "thread_runtime.h"
/* VM-local Android value/identity contracts. This does not provide an Android
 * UI thread, queue, scheduler, or SDK business service. android/os/Process
 * identity queries map directly to the host kernel's pid/tid/uid. */
#include "android_basics.h"
#include "portable.h"
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <limits.h>
#include <pthread.h>
#include <unistd.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <sys/types.h>
#include "guest_identity.h"
/* base is the host-owned Context passed to the one original Application's
 * attachBaseContext. It is deliberately not the Application itself: the
 * ContextWrapper chain must terminate at a distinct object. */
static struct {
 struct dvm *vm;
 dvm_ref main_looper,main_thread,application,base;
 pthread_t owner;
 bool attached,created;
 dvm_ref callbacks[16];
 int callback_count;
 /* Local BroadcastReceiver registry. Registration is real bookkeeping, not a
  * fabricated success: sendBroadcast below dispatches onReceive to every
  * registered receiver. Cross-process broadcasts are outside this host. */
 dvm_ref receivers[16];
 int receiver_count;
 struct {dvm_ref listener,looper;} phone_listeners[128];
} state;
struct handler {dvm_ref looper,callback;bool async;};
struct context_wrapper{dvm_ref base;};
struct handler_thread{dvm_ref name;int priority;};
/* The original Android libraries reference LIBC-versioned Bionic symbols
 * (__errno, __sF, fortify wrappers).  Load the compatibility definitions into
 * the global namespace before any original .so so their versioned relocations
 * can resolve.  Failure is reported and the subsequent dlopen still produces
 * the real error; it is never replaced by a fabricated success. */
static void preload_android_compat(void){
 static int attempted=0;
 if(attempted)return;
 attempted=1;
 const char *path="/data/storage/el1/bundle/libs/arm64/libandroid_compat.so";
 void *handle=dlopen(path,RTLD_NOW|RTLD_GLOBAL);
 const char *error=handle?NULL:dlerror();
 sdk_trace_fprintf(stderr,"ANDROID_COMPAT_PRELOAD result=%s error=%s\n",handle?"ok":"fail",error?error:"none");
}
void android_basics_vm_start(struct dvm *v){
 state.vm=v;state.main_looper=0;state.main_thread=0;state.application=0;state.base=0;state.owner=pthread_self();
 state.attached=false;state.created=false;state.callback_count=0;state.receiver_count=0;memset(state.phone_listeners,0,sizeof state.phone_listeners);
 struct dvm_class *context=dvm__class_by_desc(v,"Landroid/content/Context;");
 struct dvm_class *wrapper=dvm__class_by_desc(v,"Landroid/content/ContextWrapper;");
 struct dvm_class *app=dvm__class_by_desc(v,"Landroid/app/Application;");
 if(!context||!wrapper||!app){dvm__throw(v,"java/lang/OutOfMemoryError","Context type metadata");return;}
 wrapper->super=context;app->super=wrapper;
 struct dvm_class *thread=dvm__class_by_desc(v,"Ljava/lang/Thread;"),*ht=dvm__class_by_desc(v,"Landroid/os/HandlerThread;");if(thread&&ht)ht->super=thread;
 /* android.os.Build：真机 param 实测值（brand/manufacturer=HUAWEI、
  * model=MLR-AL00）。引擎在 GfxDevice 初始化前用 Build.MANUFACTURER 做设备
  * 分支；外部类没有 DEX 字段表，这里按 Android 平台约定建立只读静态字段。 */
 {
  static const char *const bn[]={"MANUFACTURER","BRAND","MODEL","PRODUCT"};
  static const char *const bv[]={"HUAWEI","HUAWEI","MLR-AL00","MLR-AL00"};
  struct dvm_class *b=dvm__class_by_desc(v,"Landroid/os/Build;");
  if(b&&!b->sfields){
   b->sfields=calloc(4,sizeof *b->sfields);
   b->sslots=calloc(4,sizeof *b->sslots);
   if(b->sfields&&b->sslots){
    for(int i=0;i<4;i++){
     struct dvm_field *f=&b->sfields[i];
     f->cls=b;f->name=bn[i];f->type="Ljava/lang/String;";f->kind='L';
     f->width=1;f->access=0x19;f->slot=(uint16_t)i;
     b->sslots[i].l=dvm_new_string(v,bv[i]);
    }
    b->nsfields=b->nsslots=4;
   }
  }
 }
 /* android.os.Build$VERSION：VM 模拟的系统版本声明为 Android 12 / API 31
  * （游戏 manifest 的 minSdk=24、targetSdk=33 允许运行在 31 上）。选 31 而
  * 不是 33 的依据：Unity 在 SDK_INT>=33 时会调用 API 33 独有的
  * Activity.getOnBackInvokedDispatcher()（72op 真机实测 onCreate 因此抛
  * NoSuchMethodError 并中止），而本 VM 尚未实现该 API 面；31 是当前已实现
  * 的 API 表面能一致支持的最高版本。RELEASE 为 Android 官方 API 31 版本名
  * "12"，CODENAME 为正式版 "REL"。设备不提供 INCREMENTAL /
  * SECURITY_PATCH / BASE_OS 的真实值，因此保持空串而不是编造。 */
 {
  static const char *const vn[]={"SDK_INT","RELEASE","CODENAME","INCREMENTAL",
                                 "PREVIEW_SDK_INT","SECURITY_PATCH","BASE_OS"};
  struct dvm_class *ver=dvm__class_by_desc(v,"Landroid/os/Build$VERSION;");
  if(ver&&!ver->sfields){
   ver->sfields=calloc(7,sizeof *ver->sfields);
   ver->sslots=calloc(7,sizeof *ver->sslots);
   if(ver->sfields&&ver->sslots){
    for(int i=0;i<7;i++){
     struct dvm_field *f=&ver->sfields[i];
     f->cls=ver;f->name=vn[i];f->access=0x19;f->slot=(uint16_t)i;f->width=1;
     if(!strcmp(vn[i],"SDK_INT")){f->type="I";f->kind='I';ver->sslots[i].i=31;}
     else if(!strcmp(vn[i],"PREVIEW_SDK_INT")){f->type="I";f->kind='I';ver->sslots[i].i=0;}
     else {
      f->type="Ljava/lang/String;";f->kind='L';
      const char *value=!strcmp(vn[i],"RELEASE")?"12":!strcmp(vn[i],"CODENAME")?"REL":"";
      ver->sslots[i].l=dvm_new_string(v,value);
     }
    }
     ver->nsfields=ver->nsslots=7;
    }
   }
  }
  /* android.media.AudioManager 的 STREAM_* 公开常量（AOSP AudioSystem.h：
   * VOICE_CALL=0 SYSTEM=1 RING=2 MUSIC=3 ALARM=4 NOTIFICATION=5 DTMF=8
   * ACCESSIBILITY=10）。72pt 实测 nativeRender 内 GetStaticFieldID
   * STREAM_MUSIC 抛 NoSuchFieldError（pending=692，被引擎清除但每帧重复）；
   * 这里只声明常量值，不新增行为。 */
  {
   static const char *const an[]={"STREAM_VOICE_CALL","STREAM_SYSTEM",
    "STREAM_RING","STREAM_MUSIC","STREAM_ALARM","STREAM_NOTIFICATION",
    "STREAM_DTMF","STREAM_ACCESSIBILITY"};
   static const int av[]={0,1,2,3,4,5,8,10};
   struct dvm_class *am=dvm__class_by_desc(v,"Landroid/media/AudioManager;");
   if(am&&!am->sfields){
    am->sfields=calloc(8,sizeof *am->sfields);
    am->sslots=calloc(8,sizeof *am->sslots);
    if(am->sfields&&am->sslots){
     for(int i=0;i<8;i++){
      struct dvm_field *f=&am->sfields[i];
      f->cls=am;f->name=an[i];f->type="I";f->kind='I';f->access=0x19;
      f->slot=(uint16_t)i;f->width=1;
      am->sslots[i].i=av[i];
     }
     am->nsfields=am->nsslots=8;
    }
   }
  }
  /* android.media.MediaRouter 的 ROUTE_TYPE_* 公开常量（AOSP MediaRouter.java：
   * LIVE_AUDIO=0x1 LIVE_VIDEO=0x2 LIVE_VIDEO_CAPTURE=0x4 USER=0x00800000
   * ANY=0x00800007）。72pt 实测 GetStaticFieldID ROUTE_TYPE_LIVE_VIDEO 抛
   * NoSuchFieldError（pending=698）；只声明常量值，不新增行为。 */
  {
   static const char *const rn[]={"ROUTE_TYPE_LIVE_AUDIO","ROUTE_TYPE_LIVE_VIDEO",
    "ROUTE_TYPE_LIVE_VIDEO_CAPTURE","ROUTE_TYPE_USER","ROUTE_TYPE_ANY"};
   static const int rv[]={1,2,4,0x800000,0x800007};
   struct dvm_class *mr=dvm__class_by_desc(v,"Landroid/media/MediaRouter;");
   if(mr&&!mr->sfields){
    mr->sfields=calloc(5,sizeof *mr->sfields);
    mr->sslots=calloc(5,sizeof *mr->sslots);
    if(mr->sfields&&mr->sslots){
     for(int i=0;i<5;i++){
      struct dvm_field *f=&mr->sfields[i];
      f->cls=mr;f->name=rn[i];f->type="I";f->kind='I';f->access=0x19;
      f->slot=(uint16_t)i;f->width=1;
      mr->sslots[i].i=rv[i];
     }
     mr->nsfields=mr->nsslots=5;
    }
   }
  }
  /* android.os.Handler 的方法体由 android_basics_call（主 Looper 构造/
   * 投递/移除）与 thread_runtime_call（worker Looper 投递）分派；72pu 实测
   * native 侧 FindClass(Handler) 通过后 GetMethodID 仍需类方法表，没有
   * 声明会抛 NoSuchMethodError。这里只声明两个分派器已实现的公开方法，
   * 不新增行为。 */
  {
   static const struct{const char *name;const char *sig;int access;} h_methods[]={
    {"<init>","()V",0x10001},
    {"<init>","(Landroid/os/Handler$Callback;)V",0x10001},
    {"<init>","(Landroid/os/Looper;)V",0x10001},
    {"<init>","(Landroid/os/Looper;Landroid/os/Handler$Callback;)V",0x10001},
    {"<init>","(Landroid/os/Looper;Landroid/os/Handler$Callback;Z)V",0x10001},
    {"getLooper","()Landroid/os/Looper;",0x1},
    {"obtainMessage","()Landroid/os/Message;",0x1},
    {"obtainMessage","(I)Landroid/os/Message;",0x1},
    {"obtainMessage","(ILjava/lang/Object;)Landroid/os/Message;",0x1},
    {"obtainMessage","(III)Landroid/os/Message;",0x1},
    {"obtainMessage","(IIILjava/lang/Object;)Landroid/os/Message;",0x1},
    {"post","(Ljava/lang/Runnable;)Z",0x1},
    {"postDelayed","(Ljava/lang/Runnable;J)Z",0x1},
    {"sendMessage","(Landroid/os/Message;)Z",0x1},
    {"sendMessageAtTime","(Landroid/os/Message;J)Z",0x1},
    {"sendEmptyMessage","(I)Z",0x1},
    {"sendEmptyMessageDelayed","(IJ)Z",0x1},
    {"hasMessages","(I)Z",0x1},
    {"removeMessages","(I)V",0x1},
    {"removeCallbacks","(Ljava/lang/Runnable;)V",0x1},
    {"removeCallbacks","(Ljava/lang/Runnable;Ljava/lang/Object;)V",0x1},
    {"removeCallbacksAndMessages","(Ljava/lang/Object;)V",0x1},
   };
   enum {H_N=(int)(sizeof h_methods/sizeof h_methods[0])};
   struct dvm_class *hc=dvm__class_by_desc(v,"Landroid/os/Handler;");
   if(hc&&hc->nmethods==0){
    struct dvm_method *ms=calloc(H_N,sizeof *ms);
    if(ms){
     int ok=1;
     for(int i=0;i<H_N;i++){
      struct dvm_method *m=&ms[i];
      m->cls=hc;m->name=h_methods[i].name;m->sig=strdup(h_methods[i].sig);
      m->access=(uint32_t)h_methods[i].access;
      m->arg_slots=dvm_sig_arg_slots(m->sig);
      m->arg_count=dvm_sig_arg_count(m->sig);
      m->ret_kind=dvm_sig_return_kind(m->sig);
      if(!m->sig)ok=0;
     }
     if(ok){hc->methods=ms;hc->nmethods=H_N;}
     else{for(int i=0;i<H_N;i++)free(ms[i].sig);free(ms);}
    }
   }
  }
}
dvm_ref android_basics_application(struct dvm *v){
 if(state.vm!=v){dvm__throw(v,"java/lang/IllegalStateException","Application belongs to another VM");return 0;}
 if(!state.application)state.application=dvm_new_object(v,dvm_find_class_or_external(v,"android/app/Application"));
 return state.application;
}
/* Only the runner's original Application lifecycle calls this. It replaces the
 * lazily created generic android/app/Application with the real guest object and
 * creates the distinct host base Context. Existing generic diagnostics keep
 * using android_basics_application() unchanged. */
dvm_ref android_basics_bind_application(struct dvm *v,dvm_ref application){
 if(state.vm!=v){dvm__throw(v,"java/lang/IllegalStateException","Application belongs to another VM");return 0;}
 /* The runner binds the original Application on the VM main thread before
  * any SDK code runs. Publish the main Thread identity here so a worker that
  * later compares Looper.getMainLooper().getThread() against its own
  * Thread.currentThread() does not need to have reached the main Looper
  * first; thread_runtime exposes no way to ask for it from a worker. */
 if(!state.main_thread&&thread_runtime_is_main(v))state.main_thread=thread_runtime_current(v);
 struct dvm_object *o=dvm__obj(v,application);bool subtype=false;
 for(struct dvm_class *c=o?o->cls:NULL;c;c=c->super)if(!strcmp(c->name,"android/app/Application")){subtype=true;break;}
 if(!subtype){dvm__throw(v,"java/lang/IllegalArgumentException","not an Application subtype");return 0;}
 if(state.application&&state.application!=application){dvm__throw(v,"java/lang/IllegalStateException","Application identity already bound");return 0;}
 if(!state.base){
  struct dvm_class *cls=dvm_find_class_or_external(v,"android/content/Context");
  state.base=cls?dvm_new_object(v,cls):0;
  if(!state.base){dvm__throw(v,"java/lang/OutOfMemoryError","host base Context");return 0;}
 }
 state.application=application;
 return state.base;
}
dvm_ref android_basics_base_context(struct dvm *v){return state.vm==v?state.base:0;}
static bool context_name(const char *c){return !strcmp(c,"android/content/Context")||!strcmp(c,"android/content/ContextWrapper")||!strcmp(c,"android/app/Application");}
static dvm_ref application_for(struct dvm *v,dvm_ref receiver,unsigned depth){
 if(!receiver){dvm__throw(v,"java/lang/NullPointerException","Context base");return 0;}
 if(state.vm==v&&(receiver==state.application||receiver==state.base))return state.application;
 struct dvm_object *o=dvm__obj(v,receiver);bool wrapper=false,activity=false;
 for(struct dvm_class *c=o?o->cls:NULL;c;c=c->super){
  if(!strcmp(c->name,"android/content/ContextWrapper"))wrapper=true;
  if(!strcmp(c->name,"android/app/Activity"))activity=true;
 }
 /* Activity is a ContextWrapper on Android.  The diagnostic Activity state
  * below is only populated by Activity.<init>; a guest PermissionActivity
  * constructed by the original DEX never goes through that builtin, so its
  * getApplicationContext must still resolve to the process Application. */
 if(state.vm==v&&state.application&&activity)return state.application;
 if(depth<64&&wrapper&&o->data){struct context_wrapper *w=o->data;return application_for(v,w->base,depth+1);}
 sdk_trace_missing_api(o&&o->cls?o->cls->name:"?","getApplicationContext","()Landroid/content/Context;");
 dvm__throw(v,"java/lang/UnsupportedOperationException","Context has no bound diagnostic Application");return 0;
}
static dvm_ref main_looper(struct dvm *v){
 if(state.vm!=v){dvm__throw(v,"java/lang/IllegalStateException","VM platform identity not initialized");return 0;}
 if(!state.main_looper)state.main_looper=dvm_new_object(v,dvm_find_class_or_external(v,"android/os/Looper"));
 if(state.main_looper&&!state.main_thread&&thread_runtime_is_main(v))state.main_thread=thread_runtime_current(v);
 return state.main_looper;
}
static bool owner(struct dvm *v){return state.vm==v&&pthread_equal(state.owner,pthread_self());}
/* Main-Looper messages live in the VM's own pending queue (vm->pending_*).
 * dvm.c drains that queue on the main thread at a top-level dvm_call return
 * and from the frame pump's dvm_main_looper_tick(). The strict diagnostic
 * runtime leaves dvm__queue_runnable_at() as an explicit
 * UnsupportedOperationException stub, so main-Looper posts are appended here
 * with the same array layout, pinning and removal rules. Worker Looper queues
 * stay owned by thread_runtime.c. */
static bool main_pending_reserve(struct dvm *v){
 if(v->npending<v->pending_cap)return true;
 if(v->pending_cap>INT_MAX/2)return false;
 int cap=v->pending_cap?v->pending_cap*2:64;
 void *p=realloc(v->pending_threads,(size_t)cap*sizeof *v->pending_threads);if(!p)return false;v->pending_threads=p;
 p=realloc(v->pending_is_thread,(size_t)cap*sizeof *v->pending_is_thread);if(!p)return false;v->pending_is_thread=p;
 p=realloc(v->pending_due_ms,(size_t)cap*sizeof *v->pending_due_ms);if(!p)return false;v->pending_due_ms=p;
 p=realloc(v->pending_looper,(size_t)cap*sizeof *v->pending_looper);if(!p)return false;v->pending_looper=p;
 p=realloc(v->pending_owner,(size_t)cap*sizeof *v->pending_owner);if(!p)return false;v->pending_owner=p;
 p=realloc(v->pending_token,(size_t)cap*sizeof *v->pending_token);if(!p)return false;v->pending_token=p;
 v->pending_cap=cap;return true;
}
static bool main_post(struct dvm *v,dvm_ref handler,dvm_ref runnable,int64_t delay,dvm_ref token,union dvm_value *out){
 if(!runnable){dvm__throw(v,"java/lang/NullPointerException","posted Runnable");return true;}
 if(delay<0){dvm__throw(v,"java/lang/IllegalArgumentException","postDelayed negative delay");return true;}
 if(dvm_is_stopping(v)){dvm__throw(v,"java/lang/IllegalStateException","main Looper is stopping");return true;}
 struct dvm_class *c=dvm_object_class(v,runnable);
 struct dvm_method *run=c?dvm_find_method(v,c,"run","()V"):NULL;
 /* Mirror drain_pending(): a dynamic Proxy subclass has no declared run(),
  * but dvm_proxy_try_invoke() knows how to dispatch its invocation handler. */
 bool proxy=!run&&c&&c->super&&c->super->name&&!strcmp(c->super->name,"java/lang/reflect/Proxy");
 if((!run||(!run->has_code&&!run->builtin))&&!proxy){dvm__throw(v,"java/lang/UnsupportedOperationException","Runnable.run has no executable body");return true;}
 if(!main_pending_reserve(v)){dvm__throw(v,"java/lang/OutOfMemoryError","main Looper queue");return true;}
 dvm_pin(v,runnable);
 int i=v->npending++;
 uint64_t due=0;
 if(delay>0){uint64_t now=dvm__now_ms(),wait=(uint64_t)delay;due=wait>UINT64_MAX-now?UINT64_MAX:now+wait;}
 v->pending_threads[i]=runnable;
 v->pending_is_thread[i]=false;
 v->pending_due_ms[i]=due;
 /* Main-Looper entries are tagged 0 so dvm.c's main drain picks them up. */
 v->pending_looper[i]=0;
 v->pending_owner[i]=handler;
 v->pending_token[i]=token;
 out->i=1;
 return true;
}
static void main_remove_callbacks(struct dvm *v,dvm_ref handler,dvm_ref runnable,dvm_ref token,bool has_token){
 if(!runnable)return;
 int keep=0;
 for(int i=0;i<v->npending;++i){
  bool drop=!v->pending_is_thread[i]&&!v->pending_looper[i]&&v->pending_owner[i]==handler&&v->pending_threads[i]==runnable&&(!has_token||v->pending_token[i]==token);
  if(drop){dvm_unpin(v,v->pending_threads[i]);continue;}
  if(keep!=i){
   v->pending_threads[keep]=v->pending_threads[i];v->pending_is_thread[keep]=v->pending_is_thread[i];
   v->pending_due_ms[keep]=v->pending_due_ms[i];v->pending_looper[keep]=v->pending_looper[i];
   v->pending_owner[keep]=v->pending_owner[i];v->pending_token[keep]=v->pending_token[i];
  }
  ++keep;
 }
 v->npending=keep;
}
static void main_remove_messages(struct dvm *v,dvm_ref handler,dvm_ref token){
 int keep=0;
 for(int i=0;i<v->npending;++i){
  bool drop=!v->pending_is_thread[i]&&!v->pending_looper[i]&&v->pending_owner[i]==handler&&(!token||v->pending_token[i]==token);
  if(drop){dvm_unpin(v,v->pending_threads[i]);continue;}
  if(keep!=i){
   v->pending_threads[keep]=v->pending_threads[i];v->pending_is_thread[keep]=v->pending_is_thread[i];
   v->pending_due_ms[keep]=v->pending_due_ms[i];v->pending_looper[keep]=v->pending_looper[i];
   v->pending_owner[keep]=v->pending_owner[i];v->pending_token[keep]=v->pending_token[i];
  }
  ++keep;
 }
 v->npending=keep;
}
static dvm_ref from_units(struct dvm *v,const uint16_t *u,int n){
 char *s=malloc((size_t)n*3+1);if(!s){dvm__throw(v,"java/lang/OutOfMemoryError","String units");return 0;}size_t at=0;
 for(int i=0;i<n;++i){uint32_t cp=u[i];if(cp>=0xd800&&cp<=0xdbff&&i+1<n&&u[i+1]>=0xdc00&&u[i+1]<=0xdfff){cp=0x10000+((cp-0xd800)<<10)+(u[++i]-0xdc00);s[at++]=(char)(0xf0|(cp>>18));s[at++]=(char)(0x80|((cp>>12)&63));s[at++]=(char)(0x80|((cp>>6)&63));s[at++]=(char)(0x80|(cp&63));}else if(cp&&cp<128)s[at++]=(char)cp;else if(cp<2048){s[at++]=(char)(0xc0|(cp>>6));s[at++]=(char)(0x80|(cp&63));}else{s[at++]=(char)(0xe0|(cp>>12));s[at++]=(char)(0x80|((cp>>6)&63));s[at++]=(char)(0x80|(cp&63));}}
 dvm_ref r=dvm_new_string_n(v,s,at);free(s);return r;
}
/* Return the next UTF-16 code unit; MUTF-8 encoded surrogate units are retained.
 * Four-byte UTF-8 scalar values are expanded into a surrogate pair. */
static uint16_t *units(struct dvm *v,dvm_ref ref,int *length){
 struct dvm_object *o=dvm__obj(v,ref);if(!o||o->kind!=DVM_OBJ_STRING)return NULL;
 uint16_t *u=malloc((o->utf8_len+1)*sizeof *u);if(!u){dvm__throw(v,"java/lang/OutOfMemoryError","string UTF16");return NULL;}
 size_t n=0;
 for(size_t i=0;i<o->utf8_len;){
  uint32_t cp=(unsigned char)o->utf8[i++];unsigned tail=0;
  if(cp>=0xc0&&cp<0xe0){cp&=31;tail=1;}else if(cp>=0xe0&&cp<0xf0){cp&=15;tail=2;}else if(cp>=0xf0&&cp<0xf8){cp&=7;tail=3;}
  else if(cp>=0x80){free(u);dvm__throw(v,"java/lang/IllegalArgumentException","invalid UTF8 string");return NULL;}
  if(i+tail>o->utf8_len){free(u);dvm__throw(v,"java/lang/IllegalArgumentException","truncated UTF8 string");return NULL;}
  for(unsigned j=0;j<tail;++j){unsigned char b=o->utf8[i++];if((b&0xc0)!=0x80){free(u);dvm__throw(v,"java/lang/IllegalArgumentException","invalid UTF8 continuation");return NULL;}cp=cp<<6|(b&63);}
  if(cp>0x10ffff){free(u);dvm__throw(v,"java/lang/IllegalArgumentException","invalid Unicode scalar");return NULL;}
  if(cp>0xffff){cp-=0x10000;u[n++]=(uint16_t)(0xd800+(cp>>10));u[n++]=(uint16_t)(0xdc00+(cp&1023));}else u[n++]=(uint16_t)cp;
 }
 *length=(int)n;return u;
}
static bool length(struct dvm *v,dvm_ref ref,int *out){
 struct dvm_object *o=dvm__obj(v,ref);if(!o){dvm__throw(v,"java/lang/NullPointerException","CharSequence");return false;}
 if(o->kind==DVM_OBJ_STRING){uint16_t *u=units(v,ref,out);if(!u)return false;free(u);return true;}
 struct dvm_method *method=dvm_find_method(v,o->cls,"length","()I");
 if(!method){sdk_trace_missing_api(o->cls->name,"length","()I");dvm__throw(v,"java/lang/NoSuchMethodError","%s.length()I",o->cls->name);return false;}
 union dvm_value result={0};if(!dvm_call(v,method,ref,NULL,0,&result))return false;
 if(result.i<0){dvm__throw(v,"java/lang/IllegalStateException","CharSequence.length returned negative");return false;}
 *out=result.i;return true;
}
/* Read one UTF-16 code unit from a String or from any CharSequence subclass.
 * TextUtils.equals must compare contents rather than object identity; non-String
 * receivers go through their own charAt(I)C implementation, exactly like Java. */
static bool char_at(struct dvm *v,dvm_ref ref,int index,uint16_t *out){
 struct dvm_object *o=dvm__obj(v,ref);if(!o){dvm__throw(v,"java/lang/NullPointerException","CharSequence");return false;}
 if(o->kind==DVM_OBJ_STRING){int n=0;uint16_t *u=units(v,ref,&n);if(!u)return false;if(index<0||index>=n){free(u);dvm__throw(v,"java/lang/StringIndexOutOfBoundsException","charAt index");return false;}*out=u[index];free(u);return true;}
 struct dvm_method *method=dvm_find_method(v,o->cls,"charAt","(I)C");
 if(!method){sdk_trace_missing_api(o->cls->name,"charAt","(I)C");dvm__throw(v,"java/lang/NoSuchMethodError","%s.charAt(I)C",o->cls->name);return false;}
 union dvm_value arg={.i=index},result={0};
 if(!dvm_call(v,method,ref,&arg,1,&result))return false;
 *out=(uint16_t)result.i;return true;
}
static int color_digit(char c){if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;if(c>='A'&&c<='F')return c-'A'+10;return -1;}
/* android.graphics.Color.parseColor: #RGB/#ARGB/#RRGGBB/#AARRGGBB shorthand and
 * the fixed AOSP sColorNameMap. Hex digits are case-insensitive; the returned
 * int is always ARGB with alpha in the high byte. */
static bool color_hex(const char *s,uint32_t *out){
 size_t n=strlen(s);if(s[0]!='#'||(n!=4&&n!=5&&n!=7&&n!=9))return false;
 int d[8];for(size_t i=1;i<n;++i){int x=color_digit(s[i]);if(x<0)return false;d[i-1]=x;}
 if(n==4){uint32_t r=(uint32_t)(d[0]*17),g=(uint32_t)(d[1]*17),b=(uint32_t)(d[2]*17);*out=0xff000000u|r<<16|g<<8|b;}
 else if(n==5){uint32_t al=(uint32_t)(d[0]*17),r=(uint32_t)(d[1]*17),g=(uint32_t)(d[2]*17),b=(uint32_t)(d[3]*17);*out=al<<24|r<<16|g<<8|b;}
 else if(n==7){uint32_t r=(uint32_t)(d[0]<<4|d[1]),g=(uint32_t)(d[2]<<4|d[3]),b=(uint32_t)(d[4]<<4|d[5]);*out=0xff000000u|r<<16|g<<8|b;}
 else{uint32_t al=(uint32_t)(d[0]<<4|d[1]),r=(uint32_t)(d[2]<<4|d[3]),g=(uint32_t)(d[4]<<4|d[5]),b=(uint32_t)(d[6]<<4|d[7]);*out=al<<24|r<<16|g<<8|b;}
 return true;
}
static bool color_name(const char *s,uint32_t *out){
 static const struct{const char *name;uint32_t argb;} names[]={
  {"black",0xff000000u},{"darkgray",0xff444444u},{"gray",0xff888888u},{"lightgray",0xffccccccu},{"white",0xffffffffu},
  {"red",0xffff0000u},{"green",0xff00ff00u},{"blue",0xff0000ffu},{"yellow",0xffffff00u},{"cyan",0xff00ffffu},
  {"magenta",0xffff00ffu},{"aqua",0xff00ffffu},{"fuchsia",0xffff00ffu},{"darkgrey",0xff444444u},{"grey",0xff888888u},
  {"lightgrey",0xffccccccu},{"lime",0xff00ff00u},{"maroon",0xff800000u},{"navy",0xff000080u},{"olive",0xff808000u},
  {"purple",0xff800080u},{"silver",0xffc0c0c0u},{"teal",0xff008080u}};
 size_t n=strlen(s);
 for(size_t i=0;i<sizeof names/sizeof names[0];++i){const char *p=names[i].name;if(strlen(p)!=n)continue;size_t j=0;for(;j<n;++j){unsigned char c=(unsigned char)s[j];if(c>='A'&&c<='Z')c=(unsigned char)(c+32);if(c!=(unsigned char)p[j])break;}if(j==n){*out=names[i].argb;return true;}}
 return false;
}
#define IS(name,signature) (!strcmp(m,name)&&!strcmp(sig,signature))
/* Environment/Context 目录 API 的真实文件对象。HarmonyOS 沙箱中应用可写的
 * 数据根就是 el2/base/haps/entry/files（由宿主真实创建），这里把它当 Android
 * 的外部/内部数据根使用，不编造路径。 */
#define GUEST_PUBLIC_DATA_ROOT "/data/storage/el2/base/haps/entry/files"
static dvm_ref guest_file_object(struct dvm *v,const char *path){
 struct dvm_class *fc=dvm_find_class_or_external(v,"java/io/File");
 dvm_ref r=fc?dvm_new_object(v,fc):0;
 struct dvm_object *o=r?dvm__obj(v,r):NULL;
 if(!o)return 0;
 char *copy=strdup(path);
 if(!copy)return 0;
 o->data=copy;o->data_destroy=free;
 return r;
}
/* ---- AlertDialog / Dialog 最小真实语义 ----
 * 引擎 native 的错误框通过 Java Builder 创建。这里只实现对象与回调语义：
 * show() 记录显示状态并触发 positive 监听器（等价用户点 Ok），不伪造标题/消息
 * 内容，也不返回虚构的 UI 层级。 */
struct dialog_state { dvm_ref positive; int shown; };
struct builder_state { dvm_ref positive; dvm_ref negative; dvm_ref dialog; int cancelable; };
static void dialog_state_free(void *p){free(p);}
static void builder_state_free(void *p){free(p);}
static struct builder_state *builder_of(struct dvm *v,dvm_ref self){
 struct dvm_object *o=dvm__obj(v,self);
 return o&&o->data?o->data:NULL;
}
static dvm_ref make_dialog(struct dvm *v,dvm_ref builder){
 struct builder_state *b=builder_of(v,builder);
 struct dvm_class *dc=dvm_find_class_or_external(v,"android/app/AlertDialog");
 dvm_ref d=dc?dvm_new_object(v,dc):0;
 struct dvm_object *o=d?dvm__obj(v,d):NULL;
 if(!o)return 0;
 struct dialog_state *st=calloc(1,sizeof *st);
 if(!st)return 0;
 st->positive=b?b->positive:0;
 o->data=st;o->data_destroy=dialog_state_free;
 if(b)b->dialog=d;
 return d;
}
static void dialog_show(struct dvm *v,dvm_ref dialog){
 struct dvm_object *o=dvm__obj(v,dialog);
 struct dialog_state *st=o&&o->data?o->data:NULL;
 if(!st)return;
 st->shown=1;
 dvm_ref listener=st->positive;
 int called=-1;
 if(listener){
  struct dvm_class *lc=dvm_object_class(v,listener);
  struct dvm_method *m=lc?dvm_find_method(v,lc,"onClick","(Landroid/content/DialogInterface;I)V"):NULL;
  if(m){
   union dvm_value args[2]={{0}},r={0};
   args[0].l=dialog;args[1].i=-1;
   called=dvm_call(v,m,listener,args,2,&r)?1:0;
  }
  st->positive=0;
 }
 FILE*f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt","a");
 if(f){fprintf(f,"DIALOG_SHOW dialog=%u listener=%u onclick_called=%d\n",dialog,listener,called);fclose(f);}
}
bool android_basics_call(struct dvm *v,const char *c,const char *m,const char *sig,dvm_ref self,const union dvm_value *a,int nargs,union dvm_value *out){
 if(sqlite_runtime_call(v,c,m,sig,self,a,out))return true;
 (void)nargs;memset(out,0,sizeof *out);
 if(thread_runtime_call(v,c,m,sig,self,a,nargs,out))return true;
 /* AOSP android-14.0.0_r1 PhoneStateListener(): this(null, Looper.myLooper()).
  * This records only listener/Looper identity; it registers no telephony callbacks. */
 if(!strcmp(c,"android/telephony/PhoneStateListener")&&IS("<init>","()V")){
  struct dvm_object *o=self?dvm__obj(v,self):NULL;
  struct dvm_class *base=dvm__class_by_desc(v,"Landroid/telephony/PhoneStateListener;");
  if(!o||!o->cls||!base||!dvm__class_assignable(v,o->cls,base)){dvm__throw(v,"java/lang/IllegalArgumentException","invalid PhoneStateListener receiver");return true;}
  if(state.vm!=v){dvm__throw(v,"java/lang/IllegalStateException","PhoneStateListener belongs to another VM");return true;}
  for(unsigned i=0;i<128;i++)if(state.phone_listeners[i].listener==self){dvm__throw(v,"java/lang/IllegalStateException","PhoneStateListener already constructed");return true;}
  dvm_ref looper=owner(v)?main_looper(v):0;
  if(!looper){if(!dvm_exception(v))dvm__throw(v,"java/lang/RuntimeException","PhoneStateListener requires a prepared Looper");return true;}
  for(unsigned i=0;i<128;i++)if(!state.phone_listeners[i].listener){state.phone_listeners[i].listener=self;state.phone_listeners[i].looper=looper;return true;}
  dvm__throw(v,"java/lang/OutOfMemoryError","PhoneStateListener state capacity exhausted");return true;
 }

 if(!strcmp(c,"java/lang/String")&&(IS("substring","(II)Ljava/lang/String;")||IS("substring","(I)Ljava/lang/String;"))){int n=0;uint16_t *u=units(v,self,&n);if(!u)return true;int begin=a[0].i,end=IS("substring","(I)Ljava/lang/String;")?n:a[1].i;if(begin<0||end<begin||end>n)dvm__throw(v,"java/lang/StringIndexOutOfBoundsException","substring bounds");else out->l=from_units(v,u+begin,end-begin);free(u);return true;}
 if(!strcmp(c,"java/lang/String")&&IS("toLowerCase","()Ljava/lang/String;")){const char *s=dvm_string_utf8(v,self);if(!s)return false;size_t n=dvm_string_utf8_length(v,self);for(size_t i=0;i<n;++i)if(!strchr("0123456789abcdefABCDEF-",s[i]))return false;char *lower=malloc(n+1);if(!lower){dvm__throw(v,"java/lang/OutOfMemoryError","hex lowercase");return true;}for(size_t i=0;i<n;++i)lower[i]=(s[i]>='A'&&s[i]<='F')?s[i]+32:s[i];out->l=dvm_new_string_n(v,lower,n);free(lower);return true;}
 if(!strcmp(c,"java/lang/String")&&IS("replaceAll","(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;")){
  const char *source=dvm_string_utf8(v,self),*pattern=dvm_string_utf8(v,a[0].l),*replacement=dvm_string_utf8(v,a[1].l);if(!source||!pattern||!replacement){dvm__throw(v,"java/lang/NullPointerException","replaceAll args");return true;}
  if(!*pattern||strpbrk(pattern,"\\.^$|?*+(){}[]")||strpbrk(replacement,"\\$"))return false;
  size_t n=strlen(source),p=strlen(pattern),r=strlen(replacement),count=0;if(n>1048576){dvm__throw(v,"java/lang/OutOfMemoryError","diagnostic replacement limit");return true;}for(const char *s=source;(s=strstr(s,pattern));s+=p)++count;
  if(r>p&&count>(1048576-n)/(r-p)){dvm__throw(v,"java/lang/OutOfMemoryError","replaceAll output");return true;}size_t size=n+count*r-count*p;char *result=malloc(size+1);if(!result){dvm__throw(v,"java/lang/OutOfMemoryError","replaceAll");return true;}size_t at=0;const char *s=source,*hit;while((hit=strstr(s,pattern))){size_t part=(size_t)(hit-s);memcpy(result+at,s,part);at+=part;memcpy(result+at,replacement,r);at+=r;s=hit+p;}strcpy(result+at,s);out->l=dvm_new_string_n(v,result,size);free(result);return true;
 }
 if(context_name(c)){
  if(!strcmp(c,"android/content/ContextWrapper")&&IS("<init>","(Landroid/content/Context;)V")){
   struct dvm_object *o=dvm__obj(v,self);if(!o){dvm__throw(v,"java/lang/NullPointerException","ContextWrapper receiver");return true;}
   struct context_wrapper *w=calloc(1,sizeof *w);if(!w){dvm__throw(v,"java/lang/OutOfMemoryError","ContextWrapper base");return true;}w->base=a[0].l;free(o->data);o->data=w;return true;
  }
  if(!strcmp(c,"android/app/Application")&&IS("<init>","()V")){
   if(self!=state.application){dvm__throw(v,"java/lang/IllegalStateException","Application constructor requires the bound original instance");return true;}
   return true;
  }
  if(!strcmp(c,"android/app/Application")&&IS("attachBaseContext","(Landroid/content/Context;)V")){
   if(!a||!a[0].l||self!=state.application||a[0].l!=state.base){dvm__throw(v,"java/lang/IllegalStateException","Application.attachBaseContext requires the host base Context");return true;}
   struct dvm_object *o=dvm__obj(v,self);if(!o){dvm__throw(v,"java/lang/NullPointerException","Application receiver");return true;}
   struct context_wrapper *w=calloc(1,sizeof *w);if(!w){dvm__throw(v,"java/lang/OutOfMemoryError","Application base Context");return true;}
   w->base=a[0].l;free(o->data);o->data=w;state.attached=true;return true;
  }
  if(!strcmp(c,"android/app/Application")&&IS("onCreate","()V")){
   if(self!=state.application||!state.attached){dvm__throw(v,"java/lang/IllegalStateException","Application.onCreate requires attachBaseContext");return true;}
   if(state.created){dvm__throw(v,"java/lang/IllegalStateException","Application.onCreate called twice");return true;}
   state.created=true;return true;
  }
  if(!strcmp(c,"android/app/Application")&&IS("registerActivityLifecycleCallbacks","(Landroid/app/Application$ActivityLifecycleCallbacks;)V")){
   if(self!=state.application||!a||!a[0].l){dvm__throw(v,"java/lang/NullPointerException","ActivityLifecycleCallbacks");return true;}
   if(state.callback_count>=(int)(sizeof state.callbacks/sizeof state.callbacks[0])){dvm__throw(v,"java/lang/UnsupportedOperationException","diagnostic callback table full");return true;}
   state.callbacks[state.callback_count++]=a[0].l;return true;
  }
  if(IS("getApplicationContext","()Landroid/content/Context;")){out->l=application_for(v,self,0);return true;}
  if(IS("getPackageName","()Ljava/lang/String;")){if(application_for(v,self,0))out->l=dvm_new_string(v,GUEST_EXPECTED_PACKAGE);return true;}
  /* Same real path as activity_runtime: sdkdex/base.apk is a real zip built
   * from the original APK's startup Data subset; no full APK is deployed. */
  if(IS("getPackageCodePath","()Ljava/lang/String;")){FILE*gf=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt","a");if(gf){fprintf(gf,"GET_PACKAGE_CODE_PATH site=context thread=%llu\n",(unsigned long long)(uintptr_t)pthread_self());fclose(gf);}out->l=dvm_new_string(v,"/data/storage/el2/base/haps/entry/files/sdkdex/base.apk");return true;}
  /* Context.registerReceiver: the receiver and IntentFilter are real guest
   * objects. The host keeps a bounded registry and sendBroadcast below really
   * invokes onReceive; there is no fabricated sticky Intent (null is the
   * correct Android result when no sticky broadcast exists). */
  if(IS("registerReceiver","(Landroid/content/BroadcastReceiver;Landroid/content/IntentFilter;Ljava/lang/String;Landroid/os/Handler;)Landroid/content/Intent;")){
   if(!a||!a[0].l||!a[1].l){dvm__throw(v,"java/lang/NullPointerException","registerReceiver receiver/filter");return true;}
   if(state.receiver_count>=(int)(sizeof state.receivers/sizeof state.receivers[0])){dvm__throw(v,"java/lang/UnsupportedOperationException","local receiver table full");return true;}
   state.receivers[state.receiver_count++]=a[0].l;
   out->l=0;return true;
  }
  if(IS("registerReceiver","(Landroid/content/BroadcastReceiver;Landroid/content/IntentFilter;)Landroid/content/Intent;")){
   if(!a||!a[0].l||!a[1].l){dvm__throw(v,"java/lang/NullPointerException","registerReceiver receiver/filter");return true;}
   if(state.receiver_count>=(int)(sizeof state.receivers/sizeof state.receivers[0])){dvm__throw(v,"java/lang/UnsupportedOperationException","local receiver table full");return true;}
   state.receivers[state.receiver_count++]=a[0].l;
   out->l=0;return true;
  }
  if(IS("unregisterReceiver","(Landroid/content/BroadcastReceiver;)V")){
   if(!a||!a[0].l){dvm__throw(v,"java/lang/NullPointerException","unregisterReceiver receiver");return true;}
   for(int i=0;i<state.receiver_count;++i)if(state.receivers[i]==a[0].l){state.receivers[i]=state.receivers[--state.receiver_count];return true;}
   dvm__throw(v,"java/lang/IllegalArgumentException","Receiver not registered");return true;
  }
  if(IS("sendBroadcast","(Landroid/content/Intent;)V")||IS("sendBroadcast","(Landroid/content/Intent;Ljava/lang/String;)V")){
   if(!a||!a[0].l){dvm__throw(v,"java/lang/NullPointerException","sendBroadcast intent");return true;}
   /* AOSP's receiverPermission gates delivery on the receiver holding that
    * permission.  This local registry has no per-receiver permission model;
    * record the name instead of claiming the check happened. */
   if(nargs==2){const char *permission=a[1].l?dvm_string_utf8(v,a[1].l):NULL;if(a[1].l&&!permission){dvm__throw(v,"java/lang/ClassCastException","sendBroadcast permission");return true;}if(permission&&*permission)sdk_trace_fprintf(stderr,"BROADCAST_PERMISSION_UNMODELLED name=%s\n",permission);}
   dvm_ref context=application_for(v,self,0);
   if(!context)return true;
   for(int i=0;i<state.receiver_count;++i){
    dvm_ref receiver=state.receivers[i];
    struct dvm_object *ro=dvm__obj(v,receiver);if(!ro)continue;
    struct dvm_method *m=dvm_find_method(v,ro->cls,"onReceive","(Landroid/content/Context;Landroid/content/Intent;)V");
    if(!m||(!m->has_code&&!m->builtin)){dvm__throw(v,"java/lang/AbstractMethodError","BroadcastReceiver.onReceive");return true;}
    union dvm_value ra[2]={{.l=context},{.l=a[0].l}},rr={0};
    if(!dvm_call(v,m,receiver,ra,2,&rr)||dvm_exception(v))return true;
   }
   return true;
  }
  if((!strcmp(c,"android/content/ContextWrapper")||!strcmp(c,"android/app/Application"))&&IS("getBaseContext","()Landroid/content/Context;")){
   struct dvm_object *o=dvm__obj(v,self);if(!o){dvm__throw(v,"java/lang/NullPointerException","ContextWrapper receiver");return true;}struct context_wrapper *w=o->data;out->l=w?w->base:0;return true;
  }
  /* Android normal (install-granted) permissions that this HAP really
   * declares: INTERNET -> ohos.permission.INTERNET and ACCESS_NETWORK_STATE
   * -> ohos.permission.GET_NETWORK_INFO.  Everything else is DENIED rather
   * than fabricated as granted; a future host access-token bridge can replace
   * this manifest-backed normal-permission mapping with a live query.  The
   * pid/uid arguments must identify this process, matching the only call
   * shape ContextCompat.checkSelfPermission uses. */
  if(IS("checkPermission","(Ljava/lang/String;II)I")&&nargs==3){
   if(!a[0].l){dvm__throw(v,"java/lang/NullPointerException","checkPermission permission");return true;}
   const char *permission=dvm_string_utf8(v,a[0].l);size_t length=dvm_string_utf8_length(v,a[0].l);
   if(!permission||strlen(permission)!=length||memchr(permission,0,length)){
    dvm__throw(v,"java/lang/IllegalArgumentException","checkPermission permission string");return true;
   }
   bool identity=a[1].i==(int32_t)getpid()&&a[2].i==(int32_t)getuid();
   bool declared=identity&&(!strcmp(permission,"android.permission.INTERNET")||!strcmp(permission,"android.permission.ACCESS_NETWORK_STATE"));
   const char *reason=!identity?"identity_mismatch":declared?"hap_manifest_normal":"unmapped_denied";
   sdk_trace_fprintf(stderr,"PERMISSION_CHECK permission=%s pid=%d uid=%d result=%s reason=%s\n",permission,a[1].i,a[2].i,declared?"GRANTED":"DENIED",reason);
   out->i=declared?0:-1; /* PERMISSION_GRANTED=0, PERMISSION_DENIED=-1 */
   return true;
  }
 }
 if(!strcmp(c,"android/os/SystemClock")){
  if(IS("elapsedRealtime","()J")||IS("uptimeMillis","()J")){
   struct timespec now;if(clock_gettime(CLOCK_MONOTONIC,&now))dvm__throw(v,"java/lang/IllegalStateException","monotonic clock unavailable");
   else out->j=(int64_t)now.tv_sec*1000+now.tv_nsec/1000000;return true;
  }
  if(IS("currentThreadTimeMillis","()J")){
   struct timespec now;if(clock_gettime(CLOCK_THREAD_CPUTIME_ID,&now))dvm__throw(v,"java/lang/IllegalStateException","thread clock unavailable");
   else out->j=(int64_t)now.tv_sec*1000+now.tv_nsec/1000000;return true;
  }
  return false;
 }
 /* java.util.Properties：System.getProperties() 返回空表，因此 getProperty
  * 对未知键返回 null（两参重载返回调用方默认值）；setProperty 存储尚未建模，
  * 按契约返回旧值 null。不编造平台属性。 */
 if(!strcmp(c,"java/util/Properties")){
  if(IS("getProperty","(Ljava/lang/String;)Ljava/lang/String;")&&nargs==1){
   out->l=0;return true;
  }
  if(IS("getProperty","(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;")&&nargs==2){
   out->l=a&&a[1].l?a[1].l:0;return true;
  }
  if(IS("setProperty","(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/Object;")&&nargs==2){
   out->l=0;return true;
  }
  return false;
 }
 if(!strcmp(c,"java/lang/System")){
  if(IS("currentTimeMillis","()J")){
   struct timespec now;if(clock_gettime(CLOCK_REALTIME,&now))dvm__throw(v,"java/lang/IllegalStateException","host wall clock unavailable");else out->j=(int64_t)now.tv_sec*1000+now.tv_nsec/1000000;return true;
  }
  if(IS("nanoTime","()J")){
   struct timespec now;if(clock_gettime(CLOCK_MONOTONIC,&now))dvm__throw(v,"java/lang/IllegalStateException","host monotonic clock unavailable");else out->j=(int64_t)now.tv_sec*1000000000LL+now.tv_nsec;return true;
  }
  /* VM 没有发布任何系统属性：getProperties 返回空表，getProperty 按 Java
   * 契约返回 null（无该属性且无默认值），不编造 os.name 等平台值。 */
  if(IS("getProperties","()Ljava/util/Properties;")){
   struct dvm_class *pc=dvm_find_class_or_external(v,"java/util/Properties");
   out->l=pc?dvm_new_object(v,pc):0;
   if(!out->l)dvm__throw(v,"java/lang/OutOfMemoryError","System.getProperties");
   return true;
  }
  if(IS("getProperty","(Ljava/lang/String;)Ljava/lang/String;")&&nargs==1){
   out->l=0;return true;
  }
  if(IS("getProperty","(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;")&&nargs==2){
   out->l=a&&a[1].l?a[1].l:0;return true;
  }
  /* Java's library loader is backed by the host dynamic loader.  A missing
   * Android .so is reported as UnsatisfiedLinkError, never as a fabricated
   * success.  The HAP's native directory is tried after the normal linker
   * search path so an equivalent OHOS build can be bundled under the same
   * soname. */
  if(IS("loadLibrary","(Ljava/lang/String;)V")&&nargs==1){
   preload_android_compat();
   if(!a||!a[0].l){dvm__throw(v,"java/lang/NullPointerException","library name");return true;}
   const char *name=dvm_string_utf8(v,a[0].l);size_t length=dvm_string_utf8_length(v,a[0].l);
   if(!name||strlen(name)!=length||!length||length>=200||strchr(name,'/')||memchr(name,0,length)){
    dvm__throw(v,"java/lang/UnsatisfiedLinkError","Invalid library name: %s",name?name:"?");return true;
   }
   /* Prefer the JNI bridge's loader when installed: it owns Java_ symbol
    * lookup and now falls back to the custom ELF64 loader for original
    * Android libraries whose LIBC-versioned relocations the system loader
    * cannot resolve.  If it fails, the existing dlopen path below still
    * produces the precise system dlerror. */
   if(v->hooks.load_library&&v->hooks.load_library(v->hooks.user,v,name)){
    sdk_trace_fprintf(stderr,"SYSTEM_LOAD_LIBRARY name=%s result=ok source=jni-bridge\n",name);
    return true;
   }
   char soname[256];int sn=snprintf(soname,sizeof soname,"lib%s.so",name);
   if(sn<0||(size_t)sn>=sizeof soname){dvm__throw(v,"java/lang/UnsatisfiedLinkError","Library name too long");return true;}
   void *handle=dlopen(soname,RTLD_NOW|RTLD_GLOBAL);
   const char *source="linker";
   if(!handle){
    char bundled[512];int bn=snprintf(bundled,sizeof bundled,"/data/storage/el1/bundle/libs/arm64/%s",soname);
    if(bn>0&&(size_t)bn<sizeof bundled){handle=dlopen(bundled,RTLD_NOW|RTLD_GLOBAL);if(handle)source="hap-libs";}
   }
   if(!handle){
    const char *error=dlerror();sdk_trace_fprintf(stderr,"SYSTEM_LOAD_LIBRARY name=%s result=fail error=%s\n",soname,error?error:"unknown");
    dvm__throw(v,"java/lang/UnsatisfiedLinkError","%s",error?error:"library not found");return true;
   }
   sdk_trace_fprintf(stderr,"SYSTEM_LOAD_LIBRARY name=%s result=ok source=%s\n",soname,source);
   return true;
  }
  if(IS("load","(Ljava/lang/String;)V")&&nargs==1){
   preload_android_compat();
   if(!a||!a[0].l){dvm__throw(v,"java/lang/NullPointerException","library path");return true;}
   const char *path=dvm_string_utf8(v,a[0].l);size_t length=dvm_string_utf8_length(v,a[0].l);
   if(!path||strlen(path)!=length||!length||path[0]!='/'||memchr(path,0,length)){
    dvm__throw(v,"java/lang/UnsatisfiedLinkError","Invalid library path");return true;
   }
   void *handle=dlopen(path,RTLD_NOW|RTLD_GLOBAL);
   if(!handle){const char *error=dlerror();sdk_trace_fprintf(stderr,"SYSTEM_LOAD_LIBRARY path=%s result=fail error=%s\n",path,error?error:"unknown");dvm__throw(v,"java/lang/UnsatisfiedLinkError","%s",error?error:"library not found");return true;}
   sdk_trace_fprintf(stderr,"SYSTEM_LOAD_LIBRARY path=%s result=ok source=absolute\n",path);
   return true;
  }
 }
 /* Android Process exposes the host process identity through direct kernel
  * queries; no Android runtime, guest Process object or JNI bridge is
  * involved. Static callers may pass self=0, so the receiver is not inspected.
  * Unknown Process methods still fall through and fail closed. */
 if(!strcmp(c,"android/os/Process")){
  if(IS("myPid","()I")){out->i=(int32_t)getpid();return true;}
  if(IS("myTid","()I")){out->i=(int32_t)gettid();return true;}
  if(IS("myUid","()I")){out->i=(int32_t)getuid();return true;}
  return false;
 }
 /* android.os.Environment：宿主沙箱中的真实目录语义。外部存储状态为 mounted，
 * 因为 GUEST_PUBLIC_DATA_ROOT 真实存在且可写；可移动/模拟标志按 AOSP 模拟卷
 * 语义返回。未知方法继续 fail-closed。 */
 if(!strcmp(c,"android/os/Environment")){
  if(IS("getExternalStorageDirectory","()Ljava/io/File;")||IS("getDataDirectory","()Ljava/io/File;")){
   out->l=guest_file_object(v,GUEST_PUBLIC_DATA_ROOT);return true;
  }
  if(IS("getRootDirectory","()Ljava/io/File;")){out->l=guest_file_object(v,"/system");return true;}
  if(IS("getExternalStorageState","()Ljava/lang/String;")){out->l=dvm_new_string(v,"mounted");return true;}
  if(IS("getExternalStoragePublicDirectory","(Ljava/lang/String;)Ljava/io/File;")){
   if(!a||!a[0].l){dvm__throw(v,"java/lang/NullPointerException","type");return true;}
   const char *type=dvm_string_utf8(v,a[0].l);size_t n=dvm_string_utf8_length(v,a[0].l);
   if(!type||strlen(type)!=n||!n||type[0]=='/'||strstr(type,"..")){dvm__throw(v,"java/lang/IllegalArgumentException","type");return true;}
   char path[512];
   if(snprintf(path,sizeof path,GUEST_PUBLIC_DATA_ROOT "/%s",type)>=(int)sizeof path){dvm__throw(v,"java/lang/IllegalArgumentException","type");return true;}
   (void)mkdir(path,0700);
   out->l=guest_file_object(v,path);return true;
  }
  if(IS("isExternalStorageEmulated","()Z")||IS("isExternalStorageEmulated","(Ljava/io/File;)Z")){out->i=1;return true;}
  if(IS("isExternalStorageRemovable","()Z")||IS("isExternalStorageRemovable","(Ljava/io/File;)Z")){out->i=0;return true;}
  if(IS("isExternalStorageManager","()Z")){out->i=0;return true;}
  return false;
 }
 if(!strcmp(c,"android/app/AlertDialog$Builder")){
  if(IS("<init>","(Landroid/content/Context;)V")){
   struct dvm_object *o=dvm__obj(v,self);
   if(!o)return false;
   struct builder_state *b=calloc(1,sizeof *b);
   if(!b){dvm__throw(v,"java/lang/OutOfMemoryError","builder");return true;}
   o->data=b;o->data_destroy=builder_state_free;return true;
  }
  struct builder_state *b=builder_of(v,self);
  if(!b)return false;
  if(IS("setTitle","(Ljava/lang/CharSequence;)Landroid/app/AlertDialog$Builder;")||
     IS("setMessage","(Ljava/lang/CharSequence;)Landroid/app/AlertDialog$Builder;")){out->l=self;return true;}
  if(IS("setPositiveButton","(Ljava/lang/CharSequence;Landroid/content/DialogInterface$OnClickListener;)Landroid/app/AlertDialog$Builder;")){
   b->positive=nargs>1&&a?a[1].l:0;out->l=self;return true;
  }
  if(IS("setNegativeButton","(Ljava/lang/CharSequence;Landroid/content/DialogInterface$OnClickListener;)Landroid/app/AlertDialog$Builder;")){
   b->negative=nargs>1&&a?a[1].l:0;out->l=self;return true;
  }
  if(IS("setCancelable","(Z)Landroid/app/AlertDialog$Builder;")){b->cancelable=nargs?a[0].i:1;out->l=self;return true;}
  if(IS("create","()Landroid/app/AlertDialog;")){out->l=make_dialog(v,self);return out->l!=0;}
  if(IS("show","()Landroid/app/AlertDialog;")){
   dvm_ref d=b->dialog?b->dialog:make_dialog(v,self);
   if(d)dialog_show(v,d);
   out->l=d;return d!=0;
  }
  return false;
 }
 if(!strcmp(c,"android/app/AlertDialog")||!strcmp(c,"android/app/Dialog")){
  struct dvm_object *o=dvm__obj(v,self);
  struct dialog_state *st=o&&o->data?o->data:NULL;
  if(IS("show","()V")){dialog_show(v,self);return true;}
  if(IS("dismiss","()V")){if(st)st->shown=0;return true;}
  if(IS("isShowing","()Z")){out->i=st?st->shown:0;return true;}
  return false;
 }
 if(!strcmp(c,"android/text/TextUtils")&&IS("isEmpty","(Ljava/lang/CharSequence;)Z")){
  if(!a[0].l){out->i=1;return true;}int n=0;if(length(v,a[0].l,&n))out->i=n==0;return true;
 }
 /* AOSP TextUtils.equals: identity first, null mismatch second, then an
  * equal-length UTF-16 content comparison through charAt(). */
 if(!strcmp(c,"android/text/TextUtils")&&IS("equals","(Ljava/lang/CharSequence;Ljava/lang/CharSequence;)Z")){
  dvm_ref left=a[0].l,right=a[1].l;
  if(left==right){out->i=1;return true;}
  if(!left||!right){out->i=0;return true;}
  int ln=0,rn=0;
  if(!length(v,left,&ln)||!length(v,right,&rn))return true;
  if(ln!=rn){out->i=0;return true;}
  /* Decode String operands once; only foreign CharSequences pay a charAt()
   * call per code unit. Both paths compare UTF-16 units, like Java. */
  struct dvm_object *lo=dvm__obj(v,left),*ro=dvm__obj(v,right);
  bool ls=lo&&lo->kind==DVM_OBJ_STRING,rs=ro&&ro->kind==DVM_OBJ_STRING;
  uint16_t *lu=ls?units(v,left,&ln):NULL;if(ls&&!lu)return true;
  uint16_t *ru=rs?units(v,right,&rn):NULL;if(rs&&!ru){free(lu);return true;}
  out->i=1;
  if(lu&&ru){if(memcmp(lu,ru,(size_t)ln*sizeof *lu))out->i=0;}
  else for(int i=0;i<ln;++i){uint16_t lc=lu?lu[i]:0,rc=ru?ru[i]:0;if(!lu&&!char_at(v,left,i,&lc)){out->i=0;break;}if(!ru&&!char_at(v,right,i,&rc)){out->i=0;break;}if(lc!=rc){out->i=0;break;}}
  free(lu);free(ru);return true;
 }
 if(!strcmp(c,"android/graphics/Color")&&IS("parseColor","(Ljava/lang/String;)I")){
  if(!a[0].l){dvm__throw(v,"java/lang/NullPointerException","color string");return true;}
  const char *s=dvm_string_utf8(v,a[0].l);uint32_t argb=0;
  if(!s){dvm__throw(v,"java/lang/NullPointerException","color string");return true;}
  if(s[0]=='#'?color_hex(s,&argb):color_name(s,&argb)){out->i=(int32_t)argb;return true;}
  dvm__throw(v,"java/lang/IllegalArgumentException","Unknown color");return true;
 }
 if(!strcmp(c,"java/lang/CharSequence")&&(IS("length","()I")||IS("isEmpty","()Z"))){
  struct dvm_object *o=dvm__obj(v,self);
  if(o&&o->kind==DVM_OBJ_STRING){int n=0;uint16_t *u=units(v,self,&n);if(!u)return true;free(u);out->i=IS("length","()I")?n:n==0;return true;}
  if(o&&o->cls){struct dvm_method *method=dvm_find_method(v,o->cls,"length","()I");if(method&&(method->has_code||method->builtin)){union dvm_value r={0};if(dvm_call(v,method,self,NULL,0,&r)){out->i=IS("length","()I")?r.i:r.i==0;return true;}}}
  dvm__throw(v,"java/lang/UnsupportedOperationException","CharSequence.length");return true;
 }
 if(!strcmp(c,"java/lang/String")&&(IS("length","()I")||IS("isEmpty","()Z")||IS("charAt","(I)C")||IS("indexOf","(I)I")||IS("indexOf","(II)I"))){
  int n=0;uint16_t *u=units(v,self,&n);if(!u){if(!dvm_exception(v))dvm__throw(v,"java/lang/NullPointerException","String receiver");return true;}
  if(IS("length","()I"))out->i=n;else if(IS("isEmpty","()Z"))out->i=n==0;
  else if(IS("charAt","(I)C")){if(a[0].i<0||a[0].i>=n)dvm__throw(v,"java/lang/StringIndexOutOfBoundsException","charAt index");else out->i=u[a[0].i];}
  else {int start=IS("indexOf","(II)I")?a[1].i:0;if(start<0)start=0;int32_t cp=a[0].i;out->i=-1;
   for(int i=start;i<n;++i){if(cp>=0&&cp<=0xffff&&u[i]==cp){out->i=i;break;}if(cp>0xffff&&cp<=0x10ffff&&i+1<n&&u[i]==0xd800+((cp-0x10000)>>10)&&u[i+1]==0xdc00+((cp-0x10000)&1023)){out->i=i;break;}}}
  free(u);return true;
 }
 if(!strcmp(c,"android/os/Looper")){
  if(IS("getMainLooper","()Landroid/os/Looper;")){out->l=main_looper(v);return true;}
  if(IS("myLooper","()Landroid/os/Looper;")){out->l=owner(v)?main_looper(v):0;return true;}
  if(IS("isCurrentThread","()Z")){out->i=self&&self==main_looper(v)&&owner(v);return true;}
  if(IS("getThread","()Ljava/lang/Thread;")){
   if(self!=main_looper(v))return false;
   if(!state.main_thread){
    if(!thread_runtime_is_main(v)){dvm__throw(v,"java/lang/UnsupportedOperationException","main Thread identity not published to this thread");return true;}
    state.main_thread=thread_runtime_current(v);
    if(!state.main_thread){dvm__throw(v,"java/lang/IllegalStateException","main Thread identity unavailable");return true;}
   }
   out->l=state.main_thread;return true;
  }
  /* loop/prepare/quit and queue APIs have no implementation. */
  return false;
 }
 if(!strcmp(c,"android/os/Handler")){
  bool explicit_looper=IS("<init>","(Landroid/os/Looper;)V")||IS("<init>","(Landroid/os/Looper;Landroid/os/Handler$Callback;)V")||IS("<init>","(Landroid/os/Looper;Landroid/os/Handler$Callback;Z)V");
  bool implicit_looper=IS("<init>","()V")||IS("<init>","(Landroid/os/Handler$Callback;)V");
  bool post=IS("post","(Ljava/lang/Runnable;)Z"),post_delayed=IS("postDelayed","(Ljava/lang/Runnable;J)Z");
  bool remove_callbacks=IS("removeCallbacks","(Ljava/lang/Runnable;)V");
  bool remove_callbacks_token=IS("removeCallbacks","(Ljava/lang/Runnable;Ljava/lang/Object;)V");
  bool remove_all=IS("removeCallbacksAndMessages","(Ljava/lang/Object;)V");
  if(!explicit_looper&&!implicit_looper&&!IS("getLooper","()Landroid/os/Looper;")&&!post&&!post_delayed&&!remove_callbacks&&!remove_callbacks_token&&!remove_all)return false;
  struct dvm_object *o=dvm__obj(v,self);if(!o){dvm__throw(v,"java/lang/NullPointerException","Handler receiver");return true;}
  if(explicit_looper||implicit_looper){
   dvm_ref l=explicit_looper?a[0].l:(owner(v)?main_looper(v):0);
   if(implicit_looper&&!l){union dvm_value current={0};if(thread_runtime_call(v,"android/os/Looper","myLooper","()Landroid/os/Looper;",0,NULL,0,&current))l=current.l;}
   if(!l){dvm__throw(v,explicit_looper?"java/lang/NullPointerException":"java/lang/RuntimeException","Handler requires a prepared Looper");return true;}
   if(l!=main_looper(v)&&!thread_runtime_looper(v,l)){dvm__throw(v,"java/lang/UnsupportedOperationException","unmodeled Looper identity");return true;}
   struct handler *h=calloc(1,sizeof *h);if(!h){dvm__throw(v,"java/lang/OutOfMemoryError","Handler");return true;}
   h->looper=l;if(explicit_looper&&strcmp(sig,"(Landroid/os/Looper;)V"))h->callback=a[1].l;
   if(implicit_looper&&!IS("<init>","()V"))h->callback=a[0].l;
   if(IS("<init>","(Landroid/os/Looper;Landroid/os/Handler$Callback;Z)V"))h->async=!!a[2].i;
   free(o->data);o->data=h;return true;
  }
  struct handler *h=o->data;if(!h){dvm__throw(v,"java/lang/IllegalStateException","Handler constructor not called");return true;}
  if(IS("getLooper","()Landroid/os/Looper;")){out->l=h->looper;return true;}
  /* Worker-Looper post/postDelayed were already consumed by thread_runtime.
   * Main-Looper execution belongs to dvm.c's pending-queue drain; worker
   * removal has no queue API here and must not pretend to succeed. */
  if(h->looper!=main_looper(v)){dvm__throw(v,"java/lang/UnsupportedOperationException","Handler is not bound to the VM main Looper");return true;}
  if(post)return main_post(v,self,a[0].l,0,0,out);
  if(post_delayed)return main_post(v,self,a[0].l,a[1].j,0,out);
  if(remove_callbacks){main_remove_callbacks(v,self,a[0].l,0,false);return true;}
  if(remove_callbacks_token){main_remove_callbacks(v,self,a[0].l,a[1].l,a[1].l!=0);return true;}
  main_remove_messages(v,self,a[0].l);return true;
 }

 return false;
}
struct handler_selftest_probe{struct dvm *vm;int runs,wrong_thread,wrong_gil;};
static struct handler_selftest_probe *handler_selftest_probe;
static bool handler_selftest_run(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 (void)self;(void)a;(void)n;memset(out,0,sizeof *out);
 if(!handler_selftest_probe||handler_selftest_probe->vm!=v){dvm__throw(v,"java/lang/IllegalStateException","handler selftest probe missing");return false;}
 ++handler_selftest_probe->runs;
 if(!thread_runtime_is_main(v))++handler_selftest_probe->wrong_thread;
 if(!dvm_gil_held(v))++handler_selftest_probe->wrong_gil;
 return true;
}
/* Exercises the real main-Looper path: post must not run inline, the VM's
 * main-thread drain must run it, removeCallbacks/explicit-message removal must
 * keep a queued Runnable from running, and a negative postDelayed delay must
 * be rejected with IllegalArgumentException before anything is queued. */
bool android_basics_handler_selftest(struct dvm *v,unsigned *checks){
 bool okay=true,acquired=false;unsigned local=0;if(!checks)checks=&local;
#define CHECK(expr) do{++*checks;if(!(expr)){okay=false;sdk_trace_fprintf(stderr,"HANDLER_SELFTEST_FAIL line=%d expression=%s\n",__LINE__,#expr);}}while(0)
 if(!thread_runtime_is_main(v)){sdk_trace_fprintf(stderr,"HANDLER_SELFTEST_FAIL requires the VM main thread\n");return false;}
 if(!dvm_gil_held(v)){dvm_gil_acquire(v);acquired=true;}
 union dvm_value z={0},out={0},args[2]={{0},{0}};
 struct handler_selftest_probe probe={.vm=v};handler_selftest_probe=&probe;
 struct dvm_class *rc=dvm__register_builtin(v,"Lsdktrace/tests/HandlerProbeRunnable;");CHECK(rc!=0);
 if(rc&&!rc->methods){
  rc->methods=calloc(1,sizeof *rc->methods);
  if(rc->methods){rc->nmethods=1;rc->methods[0].cls=rc;rc->methods[0].name="run";rc->methods[0].sig=strdup("()V");
   rc->methods[0].arg_count=rc->methods[0].arg_slots=0;rc->methods[0].ret_kind='V';rc->methods[0].builtin=handler_selftest_run;}
 }
 CHECK(rc&&rc->methods&&rc->nmethods==1);
 dvm_ref runnable=rc?dvm_new_object(v,rc):0;CHECK(runnable!=0);
 dvm_ref handler=dvm_new_object(v,dvm_find_class_or_external(v,"android/os/Handler"));CHECK(handler!=0);
 dvm_ref main=main_looper(v);CHECK(main!=0);
 if(!rc||!rc->methods||!runnable||!handler||!main){handler_selftest_probe=0;if(acquired)dvm_gil_release(v);return false;}
 args[0].l=main;
 CHECK(android_basics_call(v,"android/os/Handler","<init>","(Landroid/os/Looper;)V",handler,args,1,&out)&&!dvm_exception(v));
 union dvm_value got={0};
 CHECK(android_basics_call(v,"android/os/Looper","getThread","()Ljava/lang/Thread;",main,NULL,0,&got)&&!dvm_exception(v)&&got.l&&got.l==thread_runtime_current(v));
 if(dvm_exception(v)){dvm_clear_exception(v);handler_selftest_probe=0;if(acquired)dvm_gil_release(v);return false;}
 args[0].l=runnable;
 CHECK(android_basics_call(v,"android/os/Handler","post","(Ljava/lang/Runnable;)Z",handler,args,1,&out)&&!dvm_exception(v)&&out.i==1&&probe.runs==0);
 dvm__run_pending_threads(v);
 CHECK(!dvm_exception(v)&&probe.runs==1&&probe.wrong_thread==0&&probe.wrong_gil==0);
 dvm_ref runnable2=rc?dvm_new_object(v,rc):0;CHECK(runnable2!=0);
 args[0].l=runnable2;
 CHECK(android_basics_call(v,"android/os/Handler","post","(Ljava/lang/Runnable;)Z",handler,args,1,&out)&&out.i==1&&probe.runs==1);
 CHECK(android_basics_call(v,"android/os/Handler","removeCallbacks","(Ljava/lang/Runnable;)V",handler,args,1,&out)&&!dvm_exception(v));
 dvm__run_pending_threads(v);CHECK(probe.runs==1);
 args[1].j=-1;
 CHECK(android_basics_call(v,"android/os/Handler","postDelayed","(Ljava/lang/Runnable;J)Z",handler,args,2,&out)&&dvm_exception(v)&&probe.runs==1);
 dvm_clear_exception(v);
 args[1].j=60000;
 CHECK(android_basics_call(v,"android/os/Handler","postDelayed","(Ljava/lang/Runnable;J)Z",handler,args,2,&out)&&!dvm_exception(v)&&out.i==1&&probe.runs==1);
 dvm__run_pending_threads(v);CHECK(probe.runs==1);
 CHECK(android_basics_call(v,"android/os/Handler","removeCallbacks","(Ljava/lang/Runnable;)V",handler,args,1,&out)&&!dvm_exception(v));
 dvm__run_pending_threads(v);CHECK(probe.runs==1);
 dvm_ref runnable3=rc?dvm_new_object(v,rc):0;CHECK(runnable3!=0);
 args[0].l=runnable3;
 CHECK(android_basics_call(v,"android/os/Handler","post","(Ljava/lang/Runnable;)Z",handler,args,1,&out)&&out.i==1);
 CHECK(android_basics_call(v,"android/os/Handler","removeCallbacksAndMessages","(Ljava/lang/Object;)V",handler,&z,1,&out)&&!dvm_exception(v));
 dvm__run_pending_threads(v);CHECK(probe.runs==1);
 handler_selftest_probe=0;
 sdk_trace_fprintf(stderr,"HANDLER_SELFTEST %s post_deferred=1 run_on_main=1 remove_cancels=1 remove_messages_cancels=1 negative_delay_rejected=1\n",okay?"PASS":"FAIL");
 if(acquired)dvm_gil_release(v);
 return okay;
#undef CHECK
}

/* Main-thread and unprepared-thread constructor checks; no subscriptions or
 * callbacks are created. The caller must keep the VM alive during this test. */
struct phone_listener_test_worker { struct dvm *vm; dvm_ref receiver; int handled,exception; };
static void *phone_listener_no_looper_worker(void *arg){
 struct phone_listener_test_worker *t=arg;dvm_gil_acquire(t->vm);union dvm_value out={0};
 t->handled=android_basics_call(t->vm,"android/telephony/PhoneStateListener","<init>","()V",t->receiver,NULL,0,&out);
 t->exception=dvm_exception(t->vm)!=0;if(t->exception)dvm_clear_exception(t->vm);dvm_gil_release(t->vm);return NULL;
}
bool android_basics_phone_listener_selftest(struct dvm *v,unsigned *checks){
 unsigned local=0;if(!checks)checks=&local;if(!v||state.vm!=v||!owner(v)||dvm_exception(v))return false;
 bool acquired=!dvm_gil_held(v);if(acquired)dvm_gil_acquire(v);bool okay=true;
#define PHONE_CHECK(x) do{++*checks;if(!(x)){okay=false;sdk_trace_fprintf(stderr,"PHONE_LISTENER_SELFTEST_FAIL line=%d expr=%s\n",__LINE__,#x);}}while(0)
 struct dvm_class *base=dvm__class_by_desc(v,"Landroid/telephony/PhoneStateListener;");
 struct dvm_class *sub=dvm__class_by_desc(v,"Lsdktrace/tests/PhoneListener;");
 if(!base||!sub){if(acquired)dvm_gil_release(v);return false;}sub->super=base;
 dvm_ref first=dvm_new_object(v,sub),second=dvm_new_object(v,base),other=dvm_new_object(v,dvm__class_by_desc(v,"Ljava/lang/Object;")),worker=dvm_new_object(v,base);
 if(!first||!second||!other||!worker){if(acquired)dvm_gil_release(v);return false;}
 union dvm_value out={0};
 PHONE_CHECK(android_basics_call(v,"android/telephony/PhoneStateListener","<init>","()V",first,NULL,0,&out)&&!dvm_exception(v));
 PHONE_CHECK(android_basics_call(v,"android/telephony/PhoneStateListener","<init>","()V",second,NULL,0,&out)&&!dvm_exception(v));
 dvm_ref first_loop=0,second_loop=0;for(unsigned i=0;i<128;i++){if(state.phone_listeners[i].listener==first)first_loop=state.phone_listeners[i].looper;if(state.phone_listeners[i].listener==second)second_loop=state.phone_listeners[i].looper;}
 PHONE_CHECK(first!=second&&first_loop&&first_loop==second_loop&&first_loop==state.main_looper);
 int handled=android_basics_call(v,"android/telephony/PhoneStateListener","<init>","()V",first,NULL,0,&out);PHONE_CHECK(handled&&dvm_exception(v));if(dvm_exception(v))dvm_clear_exception(v);
 handled=android_basics_call(v,"android/telephony/PhoneStateListener","<init>","()V",other,NULL,0,&out);PHONE_CHECK(handled&&dvm_exception(v));if(dvm_exception(v))dvm_clear_exception(v);
 struct phone_listener_test_worker t={.vm=v,.receiver=worker};pthread_t tid;int created=pthread_create(&tid,NULL,phone_listener_no_looper_worker,&t)==0;
 PHONE_CHECK(created);if(created){unsigned depth=dvm_gil_unlock_all(v);int joined=pthread_join(tid,NULL);dvm_gil_relock(v,depth);PHONE_CHECK(!joined&&t.handled&&t.exception);}
 for(unsigned i=0;i<128;i++)if(state.phone_listeners[i].listener==first||state.phone_listeners[i].listener==second||state.phone_listeners[i].listener==worker)memset(&state.phone_listeners[i],0,sizeof state.phone_listeners[i]);
 sdk_trace_fprintf(stderr,"PHONE_LISTENER_SELFTEST %s main_looper=1 object_isolation=1 duplicate_rejected=1 invalid_rejected=1 unprepared_thread_rejected=1 subscription=0\n",okay?"PASS":"FAIL");
 if(acquired)dvm_gil_release(v);return okay;
#undef PHONE_CHECK
}
