#include "activity_runtime.h"
#include "view_state_runtime.h"
#include "window_attributes.h"
#include "display_runtime.h"
#include "theme_runtime.h"
#include "unity_view_mount.h"
#include "android_basics.h"
#include "intent_runtime.h"
#include "platform_runtime.h"
#include "thread_runtime.h"
#include "guest_identity.h"
#include "portable.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdarg.h>
/* Engine-stage diagnostic channel: sdk_trace_fprintf stops reaching the
 * progress file once the SDK runner returns, so append directly instead. */
static void engine_probe_log(const char *fmt,...){
 FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt","a");if(!f)return;
 va_list a;va_start(a,fmt);vfprintf(f,fmt,a);va_end(a);fputc('\n',f);fclose(f);
}
/* Exported for the runner/test wiring without changing activity_runtime.h. */
bool activity_runtime_bind_intent(struct dvm *, dvm_ref);
bool sdk_activity_bind_intent(struct dvm *, dvm_ref);
bool activity_runtime_selftest(struct dvm *, unsigned *);
/* Real display direction from the @ohos.display snapshot (1=portrait,
 * 2=landscape, 0=unknown); *host_present is false for the diagnostic
 * fallback.  Declared locally so the staged platform_runtime.h does not need
 * to change. */
extern int32_t sdk_get_display_orientation(bool *host_present);
#define ACTIVITY_SCREEN_ORIENTATION_UNSPECIFIED (-1)
#define ACTIVITY_SCREEN_ORIENTATION_LANDSCAPE 0
#define ACTIVITY_SCREEN_ORIENTATION_PORTRAIT 1
/* Lifecycle of the single Activity tracked by state.activity.  FINISHED is a
 * real runtime state: finish() only records it, it never closes a host window
 * and never claims that the Activity actually exited. */
#define ACTIVITY_LIFECYCLE_NEW 1
#define ACTIVITY_LIFECYCLE_CREATED 2
#define ACTIVITY_LIFECYCLE_FINISHED 3
/* Activity-family receivers other than state.activity keep their finished
 * state by reference.  The VM has no collector, so a dvm_ref remains valid
 * for the VM lifetime; the table only bounds how many such receivers are
 * tracked. */
#define ACTIVITY_FINISHED_CAP 32
/* Side table, never stored in object->data: ContextWrapper uses its own layout.
 * The window, window-manager and main-handler identities are diagnostic
 * facades. They keep object identity stable, but every operation that would
 * need a host window/display fails closed instead of pretending to render. */
struct activity_state {
 struct dvm *vm;
 dvm_ref activity, application;
 int lifecycle; /* NEW/CREATED/FINISHED for state.activity, 0 before it exists */
 dvm_ref finished[ACTIVITY_FINISHED_CAP];
 int nfinished;
 int finished_overflow;
 dvm_ref layout,content;
 int orientation;
 /* Screen orientation requested by the guest (setRequestedOrientation).
  * Before any explicit request the getter follows the real @ohos.display
  * snapshot; it never invents a direction. */
 int requested_orientation;
 int requested_orientation_set;
 dvm_ref intent;
 dvm_ref window;
 dvm_ref window_manager;
 dvm_ref main_handler;
 dvm_ref theme;
 int started,resumed,window_focused;
 int intent_unbound_logged;
 int window_created_logged;
 int decor_missing_logged;
 int window_setcontent_missing_logged;
 int window_setflags_missing_logged;
 int window_manager_display_missing_logged;
 int resources_missing_logged;
 int display_metrics_missing_logged;
};
static struct activity_state state;
static pthread_mutex_t host_lock=PTHREAD_MUTEX_INITIALIZER;
/* finish() may arrive on a background executor thread while the main thread
 * calls isFinishing().  Only the lifecycle transition and its side table are
 * serialised here; the rest of state keeps its existing single-thread use. */
static pthread_mutex_t lifecycle_lock=PTHREAD_MUTEX_INITIALIZER;
static int (*host_mount)(void *, int);
static void *host_user;
static activity_window_feature_host feature_host;
static void *feature_host_user;
static int (*drawn_host)(void *);
static void *drawn_host_user;
__attribute__((visibility("default"))) void sdk_set_activity_drawn_host(int (*callback)(void *),void *user){pthread_mutex_lock(&host_lock);drawn_host=callback;drawn_host_user=user;pthread_mutex_unlock(&host_lock);}
void activity_runtime_set_window_feature_host(activity_window_feature_host callback,void *user){pthread_mutex_lock(&host_lock);feature_host=callback;feature_host_user=user;pthread_mutex_unlock(&host_lock);}
__attribute__((visibility("default"))) void sdk_set_activity_window_feature_host(activity_window_feature_host callback,void *user){activity_runtime_set_window_feature_host(callback,user);}

void activity_runtime_set_host(int (*mount)(void *, int),void *user){pthread_mutex_lock(&host_lock);host_mount=mount;host_user=user;pthread_mutex_unlock(&host_lock);}
__attribute__((visibility("default"))) void sdk_set_activity_host(int (*mount)(void *, int),void *user){activity_runtime_set_host(mount,user);}
static bool ref_is_type(struct dvm *v,dvm_ref ref,const char *name){
 struct dvm_object *o=dvm__obj(v,ref);
 for(struct dvm_class *k=o?o->cls:NULL;k;k=k->super)
  if(k->name&&!strcmp(k->name,name))return true;
 return false;
}
static bool intent_subtype(struct dvm *v,dvm_ref ref){
 if(!dvm__obj(v,ref))return false;
 struct dvm_class *cls=dvm_object_class(v,ref);
 struct dvm_class *base=dvm__class_by_desc(v,"Landroid/content/Intent;");
 if(!cls||!base)return false;
 return dvm__class_assignable(v,cls,base);
}
/* Activity API calls are resolved against the receiver class chain. The
 * instruction owner may be the concrete Activity subclass or one of the
 * framework superclasses; an unrelated owner must not be claimed. */
static bool receiver_owner_matches(struct dvm *v,dvm_ref self,const char *c){
 if(!strcmp(c,"android/app/Activity")||!strcmp(c,"android/content/ContextWrapper")||!strcmp(c,"android/content/Context"))return true;
 struct dvm_object *o=dvm__obj(v,self);
 for(struct dvm_class *k=o?o->cls:NULL;k;k=k->super)
  if(k->name&&!strcmp(k->name,c))return true;
 return false;
}
/* finish()/finishAffinity()/isFinishing() are Activity methods.  Claim only
 * receivers whose chain contains Activity, or a plain ContextWrapper (the
 * family named by Activity's superclass).  Application is also a
 * ContextWrapper, but it has no Activity.finish method, so the runtime must
 * not claim it. */
static bool activity_family_receiver(struct dvm *v,dvm_ref self){
 struct dvm_object *o=dvm__obj(v,self);
 bool activity=false,wrapper=false,application=false;
 for(struct dvm_class *k=o?o->cls:NULL;k;k=k->super){
  if(!k->name)continue;
  if(!strcmp(k->name,"android/app/Activity"))activity=true;
  else if(!strcmp(k->name,"android/content/ContextWrapper"))wrapper=true;
  else if(!strcmp(k->name,"android/app/Application"))application=true;
 }
 return activity||(wrapper&&!application);
}
/* The instruction owner must itself be Activity-family.  A receiver-chain hit
 * alone is not enough: java/lang/Object is on every chain, but
 * Object.finish() is not an Activity call. */
static bool activity_owner_is_family(struct dvm *v,dvm_ref self,const char *c){
 if(!strcmp(c,"android/app/Activity")||!strcmp(c,"android/content/ContextWrapper")||!strcmp(c,"android/content/Context"))return true;
 /* The common app form is the concrete subclass as owner; avoid a class
  * lookup on every isFinishing() for that hot path. */
 struct dvm_object *o=dvm__obj(v,self);
 if(o&&o->cls&&o->cls->name&&!strcmp(o->cls->name,c))return true;
 struct dvm_class *owner=dvm_find_class_or_external(v,c);
 struct dvm_class *base=dvm__class_by_desc(v,"Landroid/app/Activity;");
 if(!owner||!base)return false;
 return dvm__class_assignable(v,owner,base);
}
static int activity_finished_locked(struct dvm *v,dvm_ref self){
 if(self==state.activity)return state.lifecycle==ACTIVITY_LIFECYCLE_FINISHED;
 for(int i=0;i<state.nfinished;++i)
  if(state.finished[i]==self&&dvm__obj(v,self))return 1;
 return 0;
}
/* Returns whether the receiver was already finished before this call. */
static int activity_mark_finished_locked(struct dvm *v,dvm_ref self){
 int already=activity_finished_locked(v,self);
 if(self==state.activity){state.lifecycle=ACTIVITY_LIFECYCLE_FINISHED;return already;}
 if(already)return 1;
 if(state.nfinished<ACTIVITY_FINISHED_CAP)state.finished[state.nfinished++]=self;
 else state.finished_overflow=1;
 return 0;
}
static dvm_ref new_platform_object(struct dvm *v,const char *name,const char *what){
 dvm_ref ref=dvm_new_object(v,dvm_find_class_or_external(v,name));
 if(!ref)dvm__throw(v,"java/lang/OutOfMemoryError","%s allocation",what);
 return ref;
}
static dvm_ref activity_window(struct dvm *v){
 if(state.window)return state.window;
 dvm_ref window=new_platform_object(v,"android/view/Window","Window");
 if(!window)return 0;
 state.window=window;
 if(!state.window_created_logged){
  state.window_created_logged=1;
  sdk_trace_fprintf(stderr,"ACTIVITY_WINDOW identity=stable host_window=unavailable render=unavailable\n");
 }
 return window;
}
static dvm_ref activity_window_manager(struct dvm *v){
 if(state.window_manager)return state.window_manager;
 dvm_ref manager=new_platform_object(v,"android/view/WindowManager","WindowManager");
 if(!manager)return 0;
 state.window_manager=manager;
 return manager;
}
/* The Handler must be explicitly bound to the main Looper: the implicit
 * Handler() constructor only works on android_basics' owner thread, while
 * Activity.runOnUiThread may be invoked from a worker. */
static dvm_ref activity_main_handler(struct dvm *v){
 if(state.main_handler)return state.main_handler;
 union dvm_value looper={0};
 if(!android_basics_call(v,"android/os/Looper","getMainLooper","()Landroid/os/Looper;",0,NULL,0,&looper)){
  if(!dvm_exception(v))dvm__throw(v,"java/lang/IllegalStateException","main Looper unavailable");
  return 0;
 }
 if(dvm_exception(v))return 0;
 if(!looper.l){dvm__throw(v,"java/lang/IllegalStateException","main Looper returned null");return 0;}
 dvm_ref handler=new_platform_object(v,"android/os/Handler","main Handler");
 if(!handler)return 0;
 union dvm_value arg={.l=looper.l},ignored={0};
 if(!android_basics_call(v,"android/os/Handler","<init>","(Landroid/os/Looper;)V",handler,&arg,1,&ignored)){
  if(!dvm_exception(v))dvm__throw(v,"java/lang/UnsupportedOperationException","main Handler constructor unavailable");
  return 0;
 }
 if(dvm_exception(v))return 0;
 state.main_handler=handler;
 return handler;
}
static void window_fail(struct dvm *v,const char *c,const char *m,const char *sig,int *logged,const char *what){
 if(!*logged){*logged=1;sdk_trace_missing_api(c,m,sig);sdk_trace_fprintf(stderr,"ACTIVITY_WINDOW_NO_HOST op=%s result=UnsupportedOperationException\n",m);}
 dvm__throw(v,"java/lang/UnsupportedOperationException","no host window: %s unavailable",what);
}
static void context_fail(struct dvm *v,const char *c,const char *m,const char *sig,int *logged,const char *what){
 if(!*logged){*logged=1;sdk_trace_missing_api(c,m,sig);sdk_trace_fprintf(stderr,"ACTIVITY_CONTEXT_API_MISSING api=%s result=UnsupportedOperationException\n",what);}
 dvm__throw(v,"java/lang/UnsupportedOperationException","%s unavailable from platform_runtime",what);
}
/* Ask platform_runtime first, then the framework declaration owners that a
 * concrete Activity subclass may stand in for. 1 = produced an object,
 * 0 = no implementation, -1 = an exception is already pending. */
static int delegate_context_object(struct dvm *v,const char *owner,const char *m,const char *sig,dvm_ref self,const union dvm_value *a,int n,dvm_ref *result){
 static const char *const owners[]={"android/app/Activity","android/content/ContextWrapper","android/content/Context"};
 const char *tried[4];int ntried=0;
 for(int pass=0;pass<4;++pass){
  const char *c=pass==0?owner:owners[pass-1];
  bool seen=false;
  for(int i=0;i<ntried;++i)if(!strcmp(tried[i],c))seen=true;
  if(seen)continue;
  tried[ntried++]=c;
  union dvm_value candidate={0};
  bool handled=platform_runtime_call(v,c,m,sig,self,a,n,&candidate);
  if(dvm_exception(v))return -1;
  if(handled&&candidate.l){*result=candidate.l;return 1;}
 }
 return 0;
}
/* 宿主 Activity 的真实方法表：DEX 没有 android.app.Activity 的定义，而
 * Unity 的 ReflectionHelper 通过 Class.getDeclaredMethods() 沿类链查找
 * Activity.getIntent 等入口（72pd 实测找不到 getIntent 后抛
 * NoSuchMethodError）。这里把分派器实现的公开方法注册为类自身的 dvm_method
 * （name 指向静态常量，sig 由 dvm_destroy 释放），方法体仍由
 * activity_runtime_call 提供。 */
struct host_method_desc { const char *name; const char *sig; int access; };
static const struct host_method_desc activity_method_table[]={
 {"<init>","()V",0x10001},
 {"getTheme","()Landroid/content/res/Resources$Theme;",0x1},
 {"finish","()V",0x1},
 {"finishAffinity","()V",0x1},
 {"isFinishing","()Z",0x1},
 {"onCreate","(Landroid/os/Bundle;)V",0x4},
 {"onStart","()V",0x4},
 {"onResume","()V",0x4},
 {"onPause","()V",0x4},
 {"onStop","()V",0x4},
 {"onDestroy","()V",0x4},
 {"onRestart","()V",0x4},
 {"onWindowFocusChanged","(Z)V",0x1},
 {"reportFullyDrawn","()V",0x1},
 {"onActivityResult","(IILandroid/content/Intent;)V",0x4},
 {"onNewIntent","(Landroid/content/Intent;)V",0x4},
 {"onSaveInstanceState","(Landroid/os/Bundle;)V",0x4},
 {"getIntent","()Landroid/content/Intent;",0x1},
 {"setIntent","(Landroid/content/Intent;)V",0x1},
 {"getCallingPackage","()Ljava/lang/String;",0x1},
 {"getWindow","()Landroid/view/Window;",0x1},
 {"getWindowManager","()Landroid/view/WindowManager;",0x1},
 {"requestWindowFeature","(I)Z",0x1},
 {"getRequestedOrientation","()I",0x1},
 {"setRequestedOrientation","(I)V",0x1},
 {"setContentView","(Landroid/view/View;)V",0x1},
 {"findViewById","(I)Landroid/view/View;",0x1},
 {"isInMultiWindowMode","()Z",0x1},
 {"runOnUiThread","(Ljava/lang/Runnable;)V",0x1}
};
/* Context 继承的方法注册在 Context 类上：服务分派（network_runtime 的
 * getSystemService("audio") 等）按 c=="android/content/Context" 匹配，
 * 放在 Activity 类上会让这些方法解析到错误的声明类。 */
static const struct host_method_desc context_method_table[]={
 {"getApplicationContext","()Landroid/content/Context;",0x1},
 {"getSystemService","(Ljava/lang/String;)Ljava/lang/Object;",0x1},
 {"getResources","()Landroid/content/res/Resources;",0x1},
 {"getPackageName","()Ljava/lang/String;",0x1},
 {"getPackageCodePath","()Ljava/lang/String;",0x1},
 {"getBaseContext","()Landroid/content/Context;",0x1},
 {"getApplication","()Landroid/app/Application;",0x1},
 {"getMainLooper","()Landroid/os/Looper;",0x1},
 {"getDisplayMetrics","()Landroid/util/DisplayMetrics;",0x1},
 {"getFilesDir","()Ljava/io/File;",0x1},
 {"getCacheDir","()Ljava/io/File;",0x1},
 {"getExternalFilesDir","(Ljava/lang/String;)Ljava/io/File;",0x1},
 {"getAssets","()Landroid/content/res/AssetManager;",0x1},
 {"getPackageManager","()Landroid/content/pm/PackageManager;",0x1},
 {"getObbDirs","()[Ljava/io/File;",0x1},
 {"getSharedPreferences","(Ljava/lang/String;I)Landroid/content/SharedPreferences;",0x1},
 {"getApplicationInfo","()Landroid/content/pm/ApplicationInfo;",0x1}
};
/* Intent 的公开方法由 intent_runtime 分派；DEX 没有 Intent 类定义时，
 * Unity 的 ReflectionHelper 会对 getIntent() 返回的 Intent 反射这些方法。 */
static const struct host_method_desc memory_info_method_table[]={
 {"<init>","()V",0x10001}
};
static const struct host_method_desc intent_method_table[]={
 {"<init>","()V",0x10001},
 {"<init>","(Ljava/lang/String;)V",0x10001},
 {"<init>","(Landroid/content/Context;Ljava/lang/Class;)V",0x10001},
 {"setClass","(Landroid/content/Context;Ljava/lang/Class;)Landroid/content/Intent;",0x1},
 {"setClassName","(Landroid/content/Context;Ljava/lang/String;)Landroid/content/Intent;",0x1},
 {"setClassName","(Ljava/lang/String;Ljava/lang/String;)Landroid/content/Intent;",0x1},
 {"getAction","()Ljava/lang/String;",0x1},
 {"setAction","(Ljava/lang/String;)Landroid/content/Intent;",0x1},
 {"getData","()Landroid/net/Uri;",0x1},
 {"getDataString","()Ljava/lang/String;",0x1},
 {"setData","(Landroid/net/Uri;)Landroid/content/Intent;",0x1},
 {"getExtras","()Landroid/os/Bundle;",0x1},
 {"hasExtra","(Ljava/lang/String;)Z",0x1},
 {"putExtra","(Ljava/lang/String;Ljava/lang/String;)Landroid/content/Intent;",0x1},
 {"putExtra","(Ljava/lang/String;I)Landroid/content/Intent;",0x1},
 {"putExtras","(Landroid/os/Bundle;)Landroid/content/Intent;",0x1},
 {"putStringArrayListExtra","(Ljava/lang/String;Ljava/util/ArrayList;)Landroid/content/Intent;",0x1},
 {"getStringExtra","(Ljava/lang/String;)Ljava/lang/String;",0x1},
 {"getStringExtra","(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",0x1},
 {"getBooleanExtra","(Ljava/lang/String;Z)Z",0x1},
 {"getIntExtra","(Ljava/lang/String;I)I",0x1},
 {"getLongExtra","(Ljava/lang/String;J)J",0x1},
 {"getFlags","()I",0x1},
 {"setFlags","(I)Landroid/content/Intent;",0x1},
 {"addFlags","(I)Landroid/content/Intent;",0x1}
};
static void install_method_table(struct dvm_class *c,
 const struct host_method_desc *table,size_t n){
 if(!c||c->nmethods>0||!table||!n)return;
 struct dvm_method *ms=calloc(n,sizeof *ms);
 if(!ms)return;
 for(size_t i=0;i<n;i++){
  struct dvm_method *m=&ms[i];
  m->cls=c;m->name=table[i].name;
  m->sig=strdup(table[i].sig);
  if(!m->sig){for(size_t k=0;k<i;k++)free(ms[k].sig);free(ms);return;}
  m->access=(uint32_t)table[i].access;
  m->arg_slots=dvm_sig_arg_slots(m->sig);
  m->arg_count=dvm_sig_arg_count(m->sig);
  m->ret_kind=dvm_sig_return_kind(m->sig);
 }
 c->methods=ms;c->nmethods=(int)n;
}
void activity_runtime_vm_start(struct dvm *v) {
 memset(&state,0,sizeof state);state.vm=v;
 struct dvm_class *a=dvm__class_by_desc(v,"Landroid/app/Activity;");
 install_method_table(a,activity_method_table,
  sizeof activity_method_table/sizeof activity_method_table[0]);
 {struct dvm_class *ctx=dvm__class_by_desc(v,"Landroid/content/Context;");
  install_method_table(ctx,context_method_table,
   sizeof context_method_table/sizeof context_method_table[0]);}
 {struct dvm_class *intent=dvm__class_by_desc(v,"Landroid/content/Intent;");
  install_method_table(intent,intent_method_table,
   sizeof intent_method_table/sizeof intent_method_table[0]);}
 /* ActivityManager.MemoryInfo 的实例字段（Unity 内存查询的 outInfo）。
  * 同时注册 <init>()V：Unity 的 AndroidJavaObject 通过 getConstructors()
  * 枚举构造器，external 类只有 platform_extra 声明时枚举为空，随后
  * Constructor.newInstance 抛 NoSuchMethodError（72rg 实测）。 */
 {struct dvm_class *mi=dvm_find_class_or_external(v,"android/app/ActivityManager$MemoryInfo");
  install_method_table(mi,memory_info_method_table,
   sizeof memory_info_method_table/sizeof memory_info_method_table[0]);
  if(mi&&!mi->ifields){
   static const char *names[]={"availMem","totalMem","threshold","lowMemory"};
   static const char *types[]={"J","J","J","Z"};
   mi->ifields=calloc(4,sizeof *mi->ifields);
   if(mi->ifields){
    for(int i=0;i<4;i++){
     struct dvm_field*f=&mi->ifields[i];
     f->cls=mi;f->name=names[i];f->type=types[i];
     f->kind=i==3?'Z':'J';f->width=1;f->access=0x1;f->slot=(uint16_t)i;
    }
    mi->nifields=mi->islots=4;
   }
  }
 }
 /* 宿主接口：DEX 没有定义时这些 external 占位类默认 is_interface=false，
  * 而引擎通过 Proxy.newProxyInstance 为它们生成代理（72pq 探针实测被拒的
  * 接口：Runnable、Handler$Callback、Choreographer$FrameCallback、
  * DisplayListener、InputDeviceListener）。Proxy 的接口可见性检查会拒绝
  * 非接口，这里一次性标记。 */
 {
  static const char *const host_ifaces[]={
   "Landroid/os/Handler$Callback;",
   "Landroid/view/Choreographer$FrameCallback;",
   "Ljava/lang/Runnable;",
   "Landroid/hardware/display/DisplayManager$DisplayListener;",
   "Landroid/hardware/input/InputManager$InputDeviceListener;"
  };
  for(size_t i=0;i<sizeof host_ifaces/sizeof host_ifaces[0];++i){
   struct dvm_class *c=dvm__class_by_desc(v,host_ifaces[i]);
   if(c)c->is_interface=true;
  }
 }
 struct dvm_class *w=dvm__class_by_desc(v,"Landroid/content/ContextWrapper;");
 if(a&&w)a->super=w;
 struct dvm_class *view=dvm__class_by_desc(v,"Landroid/view/View;");
 struct dvm_class *group=dvm__class_by_desc(v,"Landroid/view/ViewGroup;");
 struct dvm_class *layout=dvm__class_by_desc(v,"Landroid/widget/LinearLayout;");
 if(group&&view)group->super=view;if(layout&&group)layout->super=group;
}
/* The single Activity object created by this runtime, for host providers such
 * as the CommonUtils.currentActivity static fallback.  Returns 0 rather than
 * borrowing another VM's Activity. */
__attribute__((visibility("default"))) dvm_ref activity_runtime_current(struct dvm *v) {
 return state.vm==v?state.activity:0;
}
/* Bind the launch Intent object created by intent_runtime. The object keeps
 * its private payload; Activity.getIntent() returns the same reference, so
 * getAction()/get*Extra() continue to read the intent_runtime payload. */
__attribute__((visibility("default"))) bool activity_runtime_bind_intent(struct dvm *v,dvm_ref intent){
 if(state.vm!=v){dvm__throw(v,"java/lang/IllegalStateException","Intent binding belongs to another VM");return false;}
 if(intent&&!intent_subtype(v,intent)){dvm__throw(v,"java/lang/IllegalArgumentException","not an android.content.Intent");return false;}
 state.intent=intent;
 if(!intent)state.intent_unbound_logged=0;
 return true;
}
__attribute__((visibility("default"))) bool sdk_activity_bind_intent(struct dvm *v,dvm_ref intent){return activity_runtime_bind_intent(v,intent);}
bool activity_runtime_call(struct dvm *v,const char *c,const char *m,const char *sig,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 /* Context.getApplicationContext() must return the real process Application
  * for every Activity/Context receiver, not only the diagnostic Activity
  * created by Activity.<init>.  PermissionActivity (a guest Activity
  * subclass) never went through that constructor, so the old narrow
  * receiver check returned false and the whole call became MISSING_API.
  * Android throws NPE only for a null receiver and lets foreign receivers
  * fail method resolution; this runtime claims exactly the Context family. */
 if(!strcmp(m,"getApplicationContext")&&!strcmp(sig,"()Landroid/content/Context;")){
  if(!self){dvm__throw(v,"java/lang/NullPointerException","getApplicationContext receiver");return true;}
  struct dvm_object *o=dvm__obj(v,self);bool context=false;
  for(struct dvm_class *k=o?o->cls:NULL;k;k=k->super)
   if(k->name&&(!strcmp(k->name,"android/content/Context")||!strcmp(k->name,"android/content/ContextWrapper")||
                !strcmp(k->name,"android/app/Application")||!strcmp(k->name,"android/app/Activity"))){context=true;break;}
  if(!context){
   sdk_trace_fprintf(stderr,"EXCEPTION ACTIVITY_GC receiver_chain=");
   if(o&&o->cls){for(struct dvm_class *k=o->cls;k;k=k->super)sdk_trace_fprintf(stderr,"%s;",k->name?k->name:"?");}
   else sdk_trace_fprintf(stderr,"<no-object>");
   sdk_trace_fprintf(stderr,"\n");
   return false;
  }
  dvm_ref application=android_basics_application(v);
  if(!application){if(!dvm_exception(v))dvm__throw(v,"java/lang/IllegalStateException","application instance unavailable");return true;}
  out->l=application;return true;
 }
 if(state.vm!=v)return false;
 if(!strcmp(m,"getTheme")&&!strcmp(sig,"()Landroid/content/res/Resources$Theme;")&&self==state.activity){
  if(!state.theme)state.theme=theme_runtime_create_unity_theme(v);
  if(!state.theme){dvm__throw(v,"java/lang/UnsupportedOperationException","original Unity theme unavailable");return true;}
  out->l=state.theme;return true;
 }
 /* Real finish()/finishAffinity()/isFinishing(): the runtime records the
  * Activity lifecycle transition and reports it with the RESULT prefix.  It
  * does not close a host window, does not call host UI, and never claims the
  * Activity actually exited; only the runtime state changes.  finish() is
  * idempotent, and isFinishing() reports the state that finish() set. */
 if((!strcmp(m,"finish")&&!strcmp(sig,"()V"))||
    (!strcmp(m,"finishAffinity")&&!strcmp(sig,"()V"))||
    (!strcmp(m,"isFinishing")&&!strcmp(sig,"()Z"))){
  if(!self){dvm__throw(v,"java/lang/NullPointerException","%s receiver",m);return true;}
  if(!activity_family_receiver(v,self))return false;
  if(!activity_owner_is_family(v,self,c))return false;
  if(!strcmp(m,"isFinishing")){
   pthread_mutex_lock(&lifecycle_lock);
   int finished=activity_finished_locked(v,self);
   pthread_mutex_unlock(&lifecycle_lock);
   out->i=finished?1:0;
   return true;
  }
  pthread_mutex_lock(&lifecycle_lock);
  int diagnostic=self==state.activity;
  int before=diagnostic?state.lifecycle:-1; /* -1: not tracked by state.activity */
  int idempotent=activity_mark_finished_locked(v,self);
  int after=diagnostic?state.lifecycle:-1;
  int slots=state.nfinished,overflow=state.finished_overflow;
  pthread_mutex_unlock(&lifecycle_lock);
  sdk_trace_fprintf(stderr,
   "RESULT ACTIVITY_FINISH api=%s owner=%s receiver=%u diagnostic=%d lifecycle_before=%d lifecycle_after=%d idempotent=%d finished_slots=%d finished_overflow=%d window=unchanged host_ui=unchanged activity_exit=not_claimed\n",
   m,c,(unsigned)self,diagnostic,before,after,idempotent,slots,overflow);
  return true;
 }
 if(!strcmp(c,"android/app/Activity")&&!strcmp(m,"<init>")&&!strcmp(sig,"()V")){
  if(!self||!dvm__obj(v,self)||state.activity){dvm__throw(v,"java/lang/IllegalStateException","Activity construction unavailable");return true;}
  state.application=android_basics_application(v);
  if(!state.application)return true;
  state.activity=self;state.lifecycle=ACTIVITY_LIFECYCLE_NEW; /* NEW: no attach, window or lifecycle dispatch. */
  state.requested_orientation=ACTIVITY_SCREEN_ORIENTATION_UNSPECIFIED;
  state.requested_orientation_set=0;
  return true;
 }
 if(self==state.activity && !strcmp(c,"android/app/Activity") && !strcmp(m,"onCreate") && !strcmp(sig,"(Landroid/os/Bundle;)V")){
  if(state.lifecycle!=ACTIVITY_LIFECYCLE_NEW){dvm__throw(v,"java/lang/IllegalStateException","Activity.onCreate invalid state");return true;}
  state.lifecycle=ACTIVITY_LIFECYCLE_CREATED; /* CREATED; no window or UI service is installed. */
  return true;
 }
 if(self==state.activity&&!strcmp(c,"android/app/Activity")){
  if(!strcmp(sig,"()V")&&(!strcmp(m,"onStart")||!strcmp(m,"onResume")||!strcmp(m,"onPause")||!strcmp(m,"onStop"))){
   bool legal=state.lifecycle==ACTIVITY_LIFECYCLE_CREATED;
   if(!strcmp(m,"onStart")){legal=legal&&!state.started;if(legal)state.started=1;}
   else if(!strcmp(m,"onResume")){legal=legal&&state.started&&!state.resumed;if(legal)state.resumed=1;}
   else if(!strcmp(m,"onPause")){legal=legal&&state.resumed;if(legal)state.resumed=0;}
   else {legal=legal&&state.started&&!state.resumed;if(legal)state.started=0;}
   if(!legal)dvm__throw(v,"java/lang/IllegalStateException","Activity.%s invalid lifecycle state",m);
   sdk_trace_fprintf(stderr,"ENGINE_ACTIVITY super_callback=%s legal=%d started=%d resumed=%d\n",m,legal,state.started,state.resumed);
   return true;
  }
  if(!strcmp(m,"onWindowFocusChanged")&&!strcmp(sig,"(Z)V")&&n==1&&a){
   if(state.lifecycle!=ACTIVITY_LIFECYCLE_CREATED)dvm__throw(v,"java/lang/IllegalStateException","focus before Activity created");
   else state.window_focused=!!a[0].i;
   return true;
  }
 }
 if(!strcmp(c,"android/widget/LinearLayout")&&!strcmp(m,"<init>")&&!strcmp(sig,"(Landroid/content/Context;)V")){
  if(!self||!dvm__obj(v,self)||state.layout||state.lifecycle!=ACTIVITY_LIFECYCLE_CREATED||!a||a[0].l!=state.activity){dvm__throw(v,"java/lang/IllegalStateException","LinearLayout requires the created diagnostic Activity context");return true;}
  state.layout=self;state.orientation=0;return true;
 }
 if(!strcmp(c,"android/widget/LinearLayout")&&!strcmp(m,"setOrientation")&&!strcmp(sig,"(I)V")){
  if(!self||self!=state.layout||!a||(a[0].i!=0&&a[0].i!=1)){dvm__throw(v,"java/lang/IllegalArgumentException","unsupported LinearLayout or orientation");return true;}
  state.orientation=a[0].i;return true;
 }
 /* Window objects are only the stable facade returned by getWindow(). Their
  * methods never reach a native window because no host window is bound. */
 if(state.window&&self==state.window){
  if(!strcmp(m,"getAttributes")&&!strcmp(sig,"()Landroid/view/WindowManager$LayoutParams;")){out->l=window_attributes_get(v,self);return true;}
  if(!strcmp(m,"getDecorView")&&!strcmp(sig,"()Landroid/view/View;")){out->l=view_state_runtime_create_decor(v,state.activity);return true;}
  if(!strcmp(m,"setContentView")&&(!strcmp(sig,"(Landroid/view/View;)V")||!strcmp(sig,"(Landroid/view/View;Landroid/view/ViewGroup$LayoutParams;)V"))){window_fail(v,c,m,sig,&state.window_setcontent_missing_logged,"Window.setContentView");return true;}
    if((!strcmp(m,"setFlags")&&!strcmp(sig,"(II)V"))||(!strcmp(m,"addFlags")&&!strcmp(sig,"(I)V"))||(!strcmp(m,"clearFlags")&&!strcmp(sig,"(I)V"))){
   bool set=!strcmp(m,"setFlags"),clear=!strcmp(m,"clearFlags");if(!a||n!=(set?2:1)){dvm__throw(v,"java/lang/IllegalArgumentException","Window flags arguments");return true;}
   int mask=set?a[1].i:a[0].i;int flags=clear?0:a[0].i;(void)window_attributes_set_flags(v,self,flags,mask);return true;
  }
  return false;
 }
 if(state.window_manager&&self==state.window_manager){
  if(!strcmp(m,"getDefaultDisplay")&&!strcmp(sig,"()Landroid/view/Display;")){
   out->l=display_runtime_default(v);
   return true;
  }
  {
   static unsigned wm_log;
   if(wm_log<32){wm_log++;
    FILE*f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt","a");
    if(f){fprintf(f,"WINDOW_MANAGER_METHOD name=%s sig=%s\n",m?m:"?",sig?sig:"?");fclose(f);}
   }
  }
  return false;
 }
 /* A finished Activity keeps its accessor and instance methods until it is
  * destroyed; only operations that would mount UI still require CREATED. */
 if(!self||self!=state.activity||(state.lifecycle!=ACTIVITY_LIFECYCLE_NEW&&state.lifecycle!=ACTIVITY_LIFECYCLE_CREATED&&state.lifecycle!=ACTIVITY_LIFECYCLE_FINISHED))return false;
 if(!receiver_owner_matches(v,self,c))return false;
 if(!strcmp(m,"reportFullyDrawn")&&!strcmp(sig,"()V")&&n==0){
  pthread_mutex_lock(&host_lock);int (*callback)(void *)=drawn_host;void *user=drawn_host_user;pthread_mutex_unlock(&host_lock);
  if(!callback){dvm__throw(v,"java/lang/UnsupportedOperationException","reportFullyDrawn host absent");return true;}
  unsigned depth=dvm_gil_held(v)?dvm_gil_unlock_all(v):0;int status=callback(user);
  if(depth)dvm_gil_relock(v,depth);
  engine_probe_log("ACTIVITY_FULLY_DRAWN host_result=%d receiver=%u",status,(unsigned)self);
  if(status)dvm__throw(v,"java/lang/UnsupportedOperationException","reportDrawnCompleted failed (%d)",status);
  return true;
 }
 if(!strcmp(m,"requestWindowFeature")&&!strcmp(sig,"(I)Z")){
  if(!a || n!=1){dvm__throw(v,"java/lang/IllegalArgumentException","requestWindowFeature missing feature");return true;}
  if(state.lifecycle!=ACTIVITY_LIFECYCLE_NEW){dvm__throw(v,"java/lang/IllegalStateException","requestWindowFeature requires an Activity before onCreate");return true;}
  if(a[0].i!=1){out->i=0;sdk_trace_fprintf(stderr,"ACTIVITY_WINDOW_FEATURE feature=%d supported=0\n",a[0].i);return true;}
  pthread_mutex_lock(&host_lock);activity_window_feature_host callback=feature_host;void *user=feature_host_user;pthread_mutex_unlock(&host_lock);
  if(!callback){out->i=0;sdk_trace_fprintf(stderr,"ACTIVITY_WINDOW_FEATURE feature=1 supported=0 host=absent\n");return true;}
  unsigned depth=dvm_gil_held(v)?dvm_gil_unlock_all(v):0;
  int status=callback(user,a[0].i);
  if(depth)dvm_gil_relock(v,depth);
  if(state.vm!=v||state.activity!=self||state.lifecycle!=ACTIVITY_LIFECYCLE_NEW){dvm__throw(v,"java/lang/IllegalStateException","Activity changed during window feature request");return true;}
  sdk_trace_fprintf(stderr,"ACTIVITY_WINDOW_FEATURE feature=1 host_result=%d receiver=%u\n",status,(unsigned)self);
  if(status>0){out->i=0;sdk_trace_fprintf(stderr,"ACTIVITY_WINDOW_FEATURE feature=1 supported=0 enabled=unknown receiver=%u\n",(unsigned)self);return true;}
  if(status!=0){sdk_trace_missing_api(c,m,sig);dvm__throw(v,"java/lang/UnsupportedOperationException","host window feature %d rejected (%d)",a[0].i,status);return true;}
  out->i=1;sdk_trace_fprintf(stderr,"ACTIVITY_WINDOW_FEATURE feature=1 host_confirmed=1 receiver=%u\n",(unsigned)self);return true;
 }
 /* Screen orientation.  Android returns the last requested orientation; the
  * manifest value normally seeds it.  This layer has no ActivityThread, so
  * before any explicit request the getter follows the real @ohos.display
  * direction reported by the host snapshot (1=portrait, 2=landscape) and
  * falls back to UNSPECIFIED only when no verified snapshot exists. */
 if(!strcmp(m,"getRequestedOrientation")&&!strcmp(sig,"()I")){
  int value=ACTIVITY_SCREEN_ORIENTATION_UNSPECIFIED;
  if(state.requested_orientation_set){
   value=state.requested_orientation;
   sdk_trace_fprintf(stderr,"ACTIVITY_ORIENTATION get_requested source=activity_set value=%d receiver=%u\n",value,(unsigned)self);
  }else{
   bool host_present=false;int32_t config=sdk_get_display_orientation(&host_present);
   if(host_present){
    if(config==1)value=ACTIVITY_SCREEN_ORIENTATION_PORTRAIT;
    else if(config==2)value=ACTIVITY_SCREEN_ORIENTATION_LANDSCAPE;
   }
   sdk_trace_fprintf(stderr,"ACTIVITY_ORIENTATION get_requested source=%s display_config_orientation=%d android_orientation=%d host_present=%d receiver=%u\n",
    host_present?"display":"unavailable",(int)config,value,host_present?1:0,(unsigned)self);
  }
  out->i=value;
  return true;
 }
 if(!strcmp(m,"setRequestedOrientation")&&!strcmp(sig,"(I)V")){
  if(!a||n!=1){dvm__throw(v,"java/lang/IllegalArgumentException","setRequestedOrientation missing orientation");return true;}
  state.requested_orientation=a[0].i;
  state.requested_orientation_set=1;
  sdk_trace_fprintf(stderr,"ACTIVITY_ORIENTATION set_requested value=%d recorded=1 host_applied=0 receiver=%u\n",a[0].i,(unsigned)self);
  return true;
 }
 if(!strcmp(m,"setContentView")&&!strcmp(sig,"(Landroid/view/View;)V")){
  engine_probe_log("ACTIVITY_SET_CONTENT_VIEW lifecycle=%d args=%d ref=%u unity_type=%d layout=%u\n",
   state.lifecycle,a?n:-1,(unsigned)(a&&n==1?a[0].l:0),a&&n==1&&ref_is_type(v,a[0].l,"com/unity3d/player/UnityPlayer"),(unsigned)state.layout);
  if(state.lifecycle==ACTIVITY_LIFECYCLE_CREATED&&a&&n==1&&ref_is_type(v,a[0].l,"com/unity3d/player/UnityPlayer")){
   if(unity_view_mount(v,a[0].l))state.content=a[0].l;
   else if(!dvm_exception(v))dvm__throw(v,"java/lang/UnsupportedOperationException","Unity host surface mount rejected");
   return true;
  }
  if(state.lifecycle!=ACTIVITY_LIFECYCLE_CREATED||!a||!state.layout||a[0].l!=state.layout){dvm__throw(v,"java/lang/UnsupportedOperationException","only the diagnostic LinearLayout can be mounted");return true;}
  pthread_mutex_lock(&host_lock);int (*mount)(void *,int)=host_mount;void *user=host_user;pthread_mutex_unlock(&host_lock);
  if(!mount||mount(user,state.orientation)!=0){sdk_trace_missing_api("android/app/Activity","setContentView","(Landroid/view/View;)V");dvm__throw(v,"java/lang/UnsupportedOperationException","host LinearLayout mount unavailable or rejected");return true;}
  state.content=a[0].l;
  return true;
 }
 if(!strcmp(m,"findViewById")&&!strcmp(sig,"(I)Landroid/view/View;")){
  if(!a||n!=1){dvm__throw(v,"java/lang/IllegalArgumentException","findViewById arguments");return true;}
  out->l=0;
  if(a[0].i==-1)return true;
  dvm_ref roots[]={state.content,view_state_runtime_create_decor(v,self)};
  if(dvm_exception(v))return true;
  for(size_t root=0;root<sizeof roots/sizeof roots[0];root++){
   if(!roots[root])continue;
   dvm_ref views[256];
   int count=view_state_runtime_snapshot_tree(v,roots[root],views,sizeof views/sizeof views[0]);
   if(count<0){dvm__throw(v,"java/lang/IllegalStateException","invalid Activity view tree");return true;}
   for(int i=0;i<count;i++){
    union dvm_value id={0};
    if(!view_state_runtime_call(v,"android/view/View","getId","()I",views[i],NULL,0,&id)||dvm_exception(v))return true;
    if(id.i==a[0].i){out->l=views[i];return true;}
   }
  }
  return true;
 }
 if(!strcmp(m,"getIntent")&&!strcmp(sig,"()Landroid/content/Intent;")){
  if(!state.intent&&!state.intent_unbound_logged){state.intent_unbound_logged=1;sdk_trace_fprintf(stderr,"ACTIVITY_INTENT_UNBOUND owner=%s result=null\n",c);}
  out->l=state.intent;return true;
 }
 if(!strcmp(m,"getCallingPackage")&&!strcmp(sig,"()Ljava/lang/String;")){
  /* 诊断启动的 Activity 由宿主直接创建，没有发起方包名；Android 契约允许
   * 返回 null（无调用者）。Unity 的 isUaaLUseCase 用它做能力判断。 */
  out->l=0;return true;
 }
 if(!strcmp(c,"android/app/ActivityManager")&&!strcmp(m,"getMemoryInfo")&&
    !strcmp(sig,"(Landroid/app/ActivityManager$MemoryInfo;)V")&&n==1){
  if(!a||!a[0].l){dvm__throw(v,"java/lang/NullPointerException","getMemoryInfo outInfo");return true;}
  struct dvm_object *mi=dvm__obj(v,a[0].l);
  if(!mi||!mi->cls||strcmp(mi->cls->name,"android/app/ActivityManager$MemoryInfo")){
   dvm__throw(v,"java/lang/IllegalArgumentException","getMemoryInfo outInfo type");return true;
  }
  /* 从 guest 可见的 /proc/meminfo 读取真实内存；threshold 无宿主值，保持 0
   * （不编造系统阈值），lowMemory 由可用内存与阈值比较。 */
  long long total=0,avail=0;FILE *f=fopen("/proc/meminfo","r");
  if(f){char line[256];while(fgets(line,sizeof line,f)){long long kb=0;
   if(sscanf(line,"MemTotal: %lld kB",&kb)==1)total=kb*1024;
   else if(sscanf(line,"MemAvailable: %lld kB",&kb)==1)avail=kb*1024;}}
  fclose(f);
  union dvm_value val={0};
  val.j=avail;if(!dvm_set_field(v,a[0].l,"availMem","J",val))return true;
  val.j=total;if(!dvm_set_field(v,a[0].l,"totalMem","J",val))return true;
  val.j=0;if(!dvm_set_field(v,a[0].l,"threshold","J",val))return true;
  val.i=0;if(!dvm_set_field(v,a[0].l,"lowMemory","Z",val))return true;
  return true;
 }
 if(!strcmp(c,"android/app/ActivityManager$MemoryInfo")&&!strcmp(m,"<init>")&&!strcmp(sig,"()V")&&n==0){
  /* 对象字段由 dvm_new_object 清零；真实数值在 getMemoryInfo 中按
   * /proc/meminfo 填充（threshold 保持 0，不编造系统阈值）。 */
  return true;
 }
 if(!strcmp(m,"setIntent")&&!strcmp(sig,"(Landroid/content/Intent;)V")){
  if(!a){dvm__throw(v,"java/lang/NullPointerException","setIntent arguments");return true;}
  if(a[0].l&&!intent_subtype(v,a[0].l)){dvm__throw(v,"java/lang/IllegalArgumentException","setIntent requires an android.content.Intent or null");return true;}
  state.intent=a[0].l;if(!state.intent)state.intent_unbound_logged=0;
  return true;
 }
 if(!strcmp(m,"getWindow")&&!strcmp(sig,"()Landroid/view/Window;")){
  dvm_ref window=activity_window(v);
  if(!window)return true;
  out->l=window;return true;
 }
 if(!strcmp(m,"isInMultiWindowMode")&&!strcmp(sig,"()Z")){
  /* AOSP Activity defaults to false (not in multi-window).  This SDK exposes
   * no runtime window-mode query (WindowProperties has no windowMode), so
   * report the AOSP default and name the source instead of pretending to
   * have observed a host mode.  Revisit when the host API is available. */
  sdk_trace_fprintf(stderr,"ACTIVITY_MULTI_WINDOW is_in_multi_window=0 source=default_no_host_api\n");
  out->i=0;return true;
 }
 if(!strcmp(m,"getSystemService")&&!strcmp(sig,"(Ljava/lang/String;)Ljava/lang/Object;")&&a&&n==1){
  const char *service=dvm_string_utf8(v,a[0].l);
  if(service&&!strcmp(service,"window")){out->l=activity_window_manager(v);return true;}
 }
 if(!strcmp(m,"getWindowManager")&&!strcmp(sig,"()Landroid/view/WindowManager;")){
  dvm_ref manager=activity_window_manager(v);
  if(!manager)return true;
  out->l=manager;return true;
 }
 if(!strcmp(m,"runOnUiThread")&&!strcmp(sig,"(Ljava/lang/Runnable;)V")){
  if(!a){dvm__throw(v,"java/lang/NullPointerException","runOnUiThread Runnable");return true;}
  dvm_ref handler=activity_main_handler(v);
  if(!handler)return true;
  union dvm_value arg={.l=a[0].l},posted={0};
  if(!android_basics_call(v,"android/os/Handler","post","(Ljava/lang/Runnable;)Z",handler,&arg,1,&posted)){
   if(!dvm_exception(v)){sdk_trace_missing_api(c,m,sig);dvm__throw(v,"java/lang/UnsupportedOperationException","main Handler post unavailable");}
   return true;
  }
  if(dvm_exception(v))return true;
  if(posted.i!=1){sdk_trace_missing_api(c,m,sig);dvm__throw(v,"java/lang/UnsupportedOperationException","main Handler rejected the Runnable");}
  return true;
 }
 if(!strcmp(m,"getMainLooper")&&!strcmp(sig,"()Landroid/os/Looper;")){
  union dvm_value looper={0};
  if(!android_basics_call(v,"android/os/Looper","getMainLooper",sig,0,NULL,0,&looper)){
   if(!dvm_exception(v)){sdk_trace_missing_api(c,m,sig);dvm__throw(v,"java/lang/UnsupportedOperationException","main Looper unavailable");}
   return true;
  }
  if(dvm_exception(v))return true;
  out->l=looper.l;return true;
 }
 if(!strcmp(m,"getResources")&&!strcmp(sig,"()Landroid/content/res/Resources;")){
  dvm_ref resources=0;int rc=delegate_context_object(v,c,m,sig,self,a,n,&resources);
  if(rc<0)return true;
  if(rc==0){context_fail(v,c,m,sig,&state.resources_missing_logged,"Resources");return true;}
  out->l=resources;return true;
 }
 if(!strcmp(m,"getDisplayMetrics")&&!strcmp(sig,"()Landroid/util/DisplayMetrics;")){
  dvm_ref metrics=0;int rc=delegate_context_object(v,c,m,sig,self,a,n,&metrics);
  if(rc<0)return true;
  if(rc==0){context_fail(v,c,m,sig,&state.display_metrics_missing_logged,"DisplayMetrics");return true;}
  out->l=metrics;return true;
 }
 if(!strcmp(m,"getBaseContext")&&!strcmp(sig,"()Landroid/content/Context;")){out->l=android_basics_base_context(v);return true;}
 if(!strcmp(m,"getApplication")&&!strcmp(sig,"()Landroid/app/Application;")){out->l=state.application;return true;}
 if(!strcmp(m,"getPackageName")&&!strcmp(sig,"()Ljava/lang/String;")){out->l=dvm_new_string(v,GUEST_EXPECTED_PACKAGE);return true;}
 /* Unity's native engine thread resolves Context.getPackageCodePath through
  * JNI at startup and dereferences the result.  The compatibility layer has
  * no full APK in the app sandbox; sdkdex/base.apk is a real zip built from
  * the original APK containing the startup Data subset
  * (globalgamemanagers/level0/boot.config/global-metadata.dat/
  * unity_builtin_extra).  It is the data-path zip Unity opens, not a
  * fabricated path; the remaining assets are a separate known gap. */
 if(!strcmp(m,"getPackageCodePath")&&!strcmp(sig,"()Ljava/lang/String;")){
  FILE*gf=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt","a");
  if(gf){fprintf(gf,"GET_PACKAGE_CODE_PATH site=activity thread=%llu\n",(unsigned long long)(uintptr_t)pthread_self());fclose(gf);}
  out->l=dvm_new_string(v,"/data/storage/el2/base/haps/entry/files/sdkdex/base.apk");
  return true;
 }
 if(!strcmp(m,"getString")&&!strcmp(sig,"(I)Ljava/lang/String;")){
  const char *value=NULL;
  switch(a[0].i){
#include "activity_resources.inc"
  default: break;
  }
  if(!value){dvm__throw(v,"android/content/res/Resources$NotFoundException","unknown diagnostic resource");return true;}
  out->l=dvm_new_string(v,value);return true;
 }
 return false;
}
struct activity_selftest_probe {struct dvm *vm;int runs,wrong_thread,wrong_gil;};
static struct activity_selftest_probe *activity_selftest_probe;
static bool activity_selftest_run(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 (void)self;(void)a;(void)n;memset(out,0,sizeof *out);
 if(!activity_selftest_probe||activity_selftest_probe->vm!=v){dvm__throw(v,"java/lang/IllegalStateException","activity selftest probe missing");return false;}
 ++activity_selftest_probe->runs;
 if(!thread_runtime_is_main(v))++activity_selftest_probe->wrong_thread;
 if(!dvm_gil_held(v))++activity_selftest_probe->wrong_gil;
 return true;
}
/* Covers the new Activity/Window surface without a host window: getWindow and
 * getWindowManager keep object identity, every host-dependent operation
 * throws, getIntent is null until a real intent_runtime object is bound, and
 * runOnUiThread posts to the main Looper instead of running inline. */
struct feature_selftest {struct dvm *vm;int calls;int status;};
static int feature_test_host(void *user,int feature){struct feature_selftest *t=user;if(feature!=1||dvm_gil_held(t->vm))return -1;++t->calls;return t->status;}
/* Deterministic Window-flags bridge for the selftest.  window_attributes runs
 * the callback without the DVM lock and then updates LayoutParams.flags, so
 * returning 0 here exercises the real success path without depending on an OH
 * window; the embedder bridge is saved and restored around the test. */
struct window_flags_selftest {int calls;int flags;int mask;};
static int window_flags_test_host(void *user,int flags,int mask){struct window_flags_selftest *t=user;if(!t)return -1;++t->calls;t->flags=flags;t->mask=mask;return 0;}
__attribute__((visibility("default"))) bool activity_runtime_selftest(struct dvm *v,unsigned *checks){
 bool okay=true,acquired=false;unsigned local=0;if(!checks)checks=&local;
 struct activity_state saved;
 struct dvm_class *ac=NULL,*rc=NULL,*probe_cls=NULL;
 dvm_ref activity=0,intent=0,intent2=0,window=0,window2=0,manager=0,manager2=0,runnable=0,looper=0,action=0,application=0,foreign=0,guest_activity=0,probe1=0,probe2=0;
 union dvm_value out={0},args[2]={{0},{0}};
 struct activity_selftest_probe probe={0};
 static const char action_text[]="selftest.action";
#define CHECK(expr) do{++*checks;if(!(expr)){okay=false;sdk_trace_fprintf(stderr,"ACTIVITY_SELFTEST_FAIL line=%d expression=%s\n",__LINE__,#expr);if(dvm_exception(v))dvm_clear_exception(v);}}while(0)
 if(!v||state.vm!=v){sdk_trace_fprintf(stderr,"ACTIVITY_SELFTEST_FAIL requires an installed VM\n");return false;}
 if(!thread_runtime_is_main(v)){sdk_trace_fprintf(stderr,"ACTIVITY_SELFTEST_FAIL requires the VM main thread\n");return false;}
 if(!dvm_gil_held(v)){dvm_gil_acquire(v);acquired=true;}
 saved=state;
 memset(&state,0,sizeof state);state.vm=v;
 probe.vm=v;
 ac=dvm_find_class_or_external(v,"android/app/Activity");
 activity=ac?dvm_new_object(v,ac):0;
 CHECK(activity!=0);
 if(!activity)goto done;
 CHECK(activity_runtime_call(v,"android/app/Activity","<init>","()V",activity,NULL,0,&out)&&!dvm_exception(v));
 CHECK(state.activity==activity&&state.lifecycle==ACTIVITY_LIFECYCLE_NEW);
 if(dvm_exception(v)||state.activity!=activity)goto done;
 /* Exercise host confirmation twice; restore the caller's host hook before
  * leaving this block. The callback checks the DVM lock was dropped. */
 pthread_mutex_lock(&host_lock);activity_window_feature_host saved_feature=feature_host;void *saved_feature_user=feature_host_user;pthread_mutex_unlock(&host_lock);
 struct feature_selftest feature_test={v,0,0};activity_runtime_set_window_feature_host(feature_test_host,&feature_test);
 args[0].i=1;memset(&out,0,sizeof out);
 CHECK(activity_runtime_call(v,"android/app/Activity","requestWindowFeature","(I)Z",activity,args,1,&out)&&!dvm_exception(v)&&out.i==1);
 memset(&out,0,sizeof out);
 CHECK(activity_runtime_call(v,"android/app/Activity","requestWindowFeature","(I)Z",activity,args,1,&out)&&!dvm_exception(v)&&out.i==1&&feature_test.calls==2);
 feature_test.status=1;memset(&out,0,sizeof out);
 CHECK(activity_runtime_call(v,"android/app/Activity","requestWindowFeature","(I)Z",activity,args,1,&out)&&!dvm_exception(v)&&out.i==0&&feature_test.calls==3);
 feature_test.status=-1;memset(&out,0,sizeof out);
 CHECK(activity_runtime_call(v,"android/app/Activity","requestWindowFeature","(I)Z",activity,args,1,&out)&&dvm_exception(v)&&feature_test.calls==4);
 dvm_clear_exception(v);
 activity_runtime_set_window_feature_host(NULL,NULL);memset(&out,0,sizeof out);
 CHECK(activity_runtime_call(v,"android/app/Activity","requestWindowFeature","(I)Z",activity,args,1,&out)&&!dvm_exception(v)&&out.i==0);
 activity_runtime_set_window_feature_host(saved_feature,saved_feature_user);
 /* 屏幕方向：未显式 set 时跟随真�?@ohos.display 快照；set 后返回请求值�?
  * 宿主快照不存在时必须诚实地回 UNSPECIFIED，而不是猜一个方向�?*/
 {
  bool host_present=false;int32_t display_config=sdk_get_display_orientation(&host_present);
  int expected=ACTIVITY_SCREEN_ORIENTATION_UNSPECIFIED;
  if(host_present){
   if(display_config==1)expected=ACTIVITY_SCREEN_ORIENTATION_PORTRAIT;
   else if(display_config==2)expected=ACTIVITY_SCREEN_ORIENTATION_LANDSCAPE;
  }
  memset(&out,0,sizeof out);
  CHECK(activity_runtime_call(v,"android/app/Activity","getRequestedOrientation","()I",activity,NULL,0,&out)&&!dvm_exception(v)&&out.i==expected);
  args[0].i=6;memset(&out,0,sizeof out);
  CHECK(activity_runtime_call(v,"android/app/Activity","setRequestedOrientation","(I)V",activity,args,1,&out)&&!dvm_exception(v));
  memset(&out,0,sizeof out);
  CHECK(activity_runtime_call(v,"android/app/Activity","getRequestedOrientation","()I",activity,NULL,0,&out)&&!dvm_exception(v)&&out.i==6);
  sdk_trace_fprintf(stderr,"ACTIVITY_ORIENTATION_SELFTEST host_present=%d display_config=%d initial=%d\n",host_present?1:0,(int)display_config,expected);
 }
 args[0].l=0;
 CHECK(activity_runtime_call(v,"android/app/Activity","onCreate","(Landroid/os/Bundle;)V",activity,args,1,&out)&&!dvm_exception(v));
 if(dvm_exception(v)||state.lifecycle!=ACTIVITY_LIFECYCLE_CREATED)goto done;
 /* getApplicationContext 的身份：必须是真�?Application 单例，不能是
  * Activity 自己、null 或诊�?state.application 的空壳�?*/
 memset(&out,0,sizeof out);
 CHECK(activity_runtime_call(v,"android/app/Activity","getApplicationContext","()Landroid/content/Context;",activity,NULL,0,&out)&&!dvm_exception(v)&&out.l);
 application=out.l;
 CHECK(application&&application==android_basics_application(v)&&application!=activity);
 /* guest Activity 子类（类链含 android/app/Activity）不经过诊断 <init>�?
  * 也必须拿到同一�?Application；这正是 PermissionActivity 的调用形态�?*/
 guest_activity=dvm_new_object(v,dvm_find_class_or_external(v,"android/app/Activity"));
 CHECK(guest_activity!=0);
 memset(&out,0,sizeof out);
 CHECK(guest_activity&&activity_runtime_call(v,"com/bilibili/star/bili/PermissionActivity","getApplicationContext","()Landroid/content/Context;",guest_activity,NULL,0,&out)&&!dvm_exception(v)&&out.l==application);
 /* Android 语义：null receiver �?NPE；foreign receiver 不由 Activity 认领�?*/
 CHECK(activity_runtime_call(v,"android/app/Activity","getApplicationContext","()Landroid/content/Context;",0,NULL,0,&out)&&dvm_exception(v));
 dvm_clear_exception(v);
 foreign=dvm_new_object(v,dvm_find_class_or_external(v,"java/lang/Object"));
 CHECK(foreign!=0);
 CHECK(foreign&&!activity_runtime_call(v,"java/lang/Object","getApplicationContext","()Landroid/content/Context;",foreign,NULL,0,&out)&&!dvm_exception(v));
 memset(&out,0,sizeof out);
 CHECK(activity_runtime_call(v,"android/app/Activity","getWindow","()Landroid/view/Window;",activity,NULL,0,&out)&&!dvm_exception(v)&&out.l);
 window=out.l;
 memset(&out,0,sizeof out);
 CHECK(activity_runtime_call(v,"android/app/Activity","getWindow","()Landroid/view/Window;",activity,NULL,0,&out)&&!dvm_exception(v)&&out.l==window);
 window2=out.l;
 CHECK(window&&window2==window&&ref_is_type(v,window,"android/view/Window"));
 if(!window)goto done;
 memset(&out,0,sizeof out);
 CHECK(activity_runtime_call(v,"android/view/Window","getDecorView","()Landroid/view/View;",window,NULL,0,&out)&&!dvm_exception(v)&&out.l);
 dvm_ref checked_decor=out.l;
 CHECK(activity_runtime_call(v,"android/view/Window","getDecorView","()Landroid/view/View;",window,NULL,0,&out)&&!dvm_exception(v)&&out.l==checked_decor);
 args[0].l=0;
 CHECK(activity_runtime_call(v,"android/view/Window","setContentView","(Landroid/view/View;)V",window,args,1,&out)&&dvm_exception(v));
 dvm_clear_exception(v);
 /* Window flags now go through window_attributes + the host bridge (this is
  * what Unity.hideStatusBar uses).  Install a deterministic host, assert the
  * success path, the callback arguments and the LayoutParams.flags field,
  * then restore the embedder's real bridge. */
 {
  struct window_flags_selftest flags_test={0,0,0};
  void *saved_user=NULL;
  window_flags_host saved=window_attributes_get_flags_host(&saved_user);
  sdk_set_window_flags_host(window_flags_test_host,&flags_test);
  args[0].i=1024;args[1].i=1024;
  CHECK(activity_runtime_call(v,"android/view/Window","setFlags","(II)V",window,args,2,&out)&&!dvm_exception(v)&&flags_test.calls==1&&flags_test.flags==1024&&flags_test.mask==1024);
  {
   union dvm_value flags_value={0};
   dvm_ref flags_params=window_attributes_get(v,window);
   CHECK(flags_params!=0&&!dvm_exception(v)&&dvm_get_field(v,flags_params,"flags","I",&flags_value)&&flags_value.i==1024);
  }
  sdk_set_window_flags_host(saved,saved_user);
  if(dvm_exception(v))dvm_clear_exception(v);
 }
 memset(&out,0,sizeof out);
 CHECK(activity_runtime_call(v,"android/app/Activity","getWindowManager","()Landroid/view/WindowManager;",activity,NULL,0,&out)&&!dvm_exception(v)&&out.l);
 manager=out.l;
 memset(&out,0,sizeof out);
 CHECK(activity_runtime_call(v,"android/app/Activity","getWindowManager","()Landroid/view/WindowManager;",activity,NULL,0,&out)&&!dvm_exception(v)&&out.l==manager);
 manager2=out.l;
 CHECK(manager&&manager2==manager&&ref_is_type(v,manager,"android/view/WindowManager"));
 if(manager){
  memset(&out,0,sizeof out);
  CHECK(activity_runtime_call(v,"android/view/WindowManager","getDefaultDisplay","()Landroid/view/Display;",manager,NULL,0,&out)&&!dvm_exception(v)&&out.l);
  dvm_clear_exception(v);
 }
 memset(&out,0,sizeof out);
 CHECK(activity_runtime_call(v,"android/app/Activity","getIntent","()Landroid/content/Intent;",activity,NULL,0,&out)&&!dvm_exception(v)&&out.l==0);
 ac=dvm_find_class_or_external(v,"android/content/Intent");
 intent=ac?dvm_new_object(v,ac):0;
 CHECK(intent!=0);
 if(!intent)goto done;
 memset(&out,0,sizeof out);
 CHECK(intent_runtime_call(v,"android/content/Intent","<init>","()V",intent,NULL,0,&out)&&!dvm_exception(v));
 action=dvm_new_string(v,action_text);
 CHECK(action!=0);
 if(action){
  args[0].l=action;
  memset(&out,0,sizeof out);
  CHECK(intent_runtime_call(v,"android/content/Intent","setAction","(Ljava/lang/String;)Landroid/content/Intent;",intent,args,1,&out)&&out.l==intent&&!dvm_exception(v));
 }
 CHECK(activity_runtime_bind_intent(v,intent)&&!dvm_exception(v));
 memset(&out,0,sizeof out);
 CHECK(activity_runtime_call(v,"android/app/Activity","getIntent","()Landroid/content/Intent;",activity,NULL,0,&out)&&!dvm_exception(v)&&out.l==intent);
 memset(&out,0,sizeof out);
 CHECK(intent_runtime_call(v,"android/content/Intent","getAction","()Ljava/lang/String;",intent,NULL,0,&out)&&!dvm_exception(v)&&out.l&&!strcmp(dvm_string_utf8(v,out.l),action_text));
 intent2=ac?dvm_new_object(v,ac):0;
 CHECK(intent2!=0);
 if(intent2){
  memset(&out,0,sizeof out);
  CHECK(intent_runtime_call(v,"android/content/Intent","<init>","()V",intent2,NULL,0,&out)&&!dvm_exception(v));
  args[0].l=intent2;
  CHECK(activity_runtime_call(v,"android/app/Activity","setIntent","(Landroid/content/Intent;)V",activity,args,1,&out)&&!dvm_exception(v));
  memset(&out,0,sizeof out);
  CHECK(activity_runtime_call(v,"android/app/Activity","getIntent","()Landroid/content/Intent;",activity,NULL,0,&out)&&!dvm_exception(v)&&out.l==intent2);
 }
 CHECK(!activity_runtime_bind_intent(v,activity)&&dvm_exception(v));
 dvm_clear_exception(v);
 rc=dvm__register_builtin(v,"Lsdktrace/tests/ActivityProbeRunnable;");
 CHECK(rc!=0);
 if(rc&&!rc->methods){
  rc->methods=calloc(1,sizeof *rc->methods);
  if(rc->methods){rc->nmethods=1;rc->methods[0].cls=rc;rc->methods[0].name="run";rc->methods[0].sig=strdup("()V");
   rc->methods[0].arg_count=rc->methods[0].arg_slots=0;rc->methods[0].ret_kind='V';rc->methods[0].builtin=activity_selftest_run;}
 }
 CHECK(rc&&rc->methods&&rc->nmethods==1);
 runnable=rc?dvm_new_object(v,rc):0;
 CHECK(runnable!=0);
 if(runnable){
  activity_selftest_probe=&probe;
  args[0].l=runnable;
  memset(&out,0,sizeof out);
  CHECK(activity_runtime_call(v,"android/app/Activity","runOnUiThread","(Ljava/lang/Runnable;)V",activity,args,1,&out)&&!dvm_exception(v)&&probe.runs==0);
  dvm__run_pending_threads(v);
  CHECK(!dvm_exception(v)&&probe.runs==1&&probe.wrong_thread==0&&probe.wrong_gil==0);
  activity_selftest_probe=0;
 }
 memset(&out,0,sizeof out);
 CHECK(activity_runtime_call(v,"android/app/Activity","getMainLooper","()Landroid/os/Looper;",activity,NULL,0,&out)&&!dvm_exception(v)&&out.l);
 looper=out.l;
 memset(&out,0,sizeof out);
 CHECK(android_basics_call(v,"android/os/Looper","getMainLooper","()Landroid/os/Looper;",0,NULL,0,&out)&&!dvm_exception(v)&&out.l==looper);
 /* finish()/finishAffinity()/isFinishing() 的真实状态转换：只记录生命周期，
  * 不关闭宿主窗口、不调用宿主 UI、不伪�?Activity 退出�?*/
 memset(&out,0,sizeof out);
 CHECK(activity_runtime_call(v,"android/app/Activity","isFinishing","()Z",activity,NULL,0,&out)&&!dvm_exception(v)&&out.i==0);
 CHECK(activity_runtime_call(v,"android/app/Activity","finish","()V",0,NULL,0,&out)&&dvm_exception(v));
 dvm_clear_exception(v);
 CHECK(foreign&&!activity_runtime_call(v,"android/app/Activity","isFinishing","()Z",foreign,NULL,0,&out)&&!dvm_exception(v));
 CHECK(foreign&&!activity_runtime_call(v,"java/lang/Object","finish","()V",foreign,NULL,0,&out)&&!dvm_exception(v));
 ac=dvm_find_class_or_external(v,"android/app/Activity");
 probe_cls=dvm__register_builtin(v,"Lsdktrace/tests/PermissionActivityProbe;");
 CHECK(probe_cls!=0);
 if(probe_cls)probe_cls->super=ac;
 probe1=probe_cls?dvm_new_object(v,probe_cls):0;
 probe2=probe_cls?dvm_new_object(v,probe_cls):0;
 CHECK(probe1!=0&&probe2!=0);
 if(probe1&&probe2){
  /* 具体 Activity 子类作为 owner：PermissionActivity.finish() 的真实形态�?*/
  memset(&out,0,sizeof out);
  CHECK(activity_runtime_call(v,"sdktrace/tests/PermissionActivityProbe","isFinishing","()Z",probe1,NULL,0,&out)&&!dvm_exception(v)&&out.i==0);
  CHECK(activity_runtime_call(v,"sdktrace/tests/PermissionActivityProbe","finish","()V",probe1,NULL,0,&out)&&!dvm_exception(v));
  memset(&out,0,sizeof out);
  CHECK(activity_runtime_call(v,"sdktrace/tests/PermissionActivityProbe","isFinishing","()Z",probe1,NULL,0,&out)&&!dvm_exception(v)&&out.i==1);
  /* 重复 finish 幂等，框�?owner 形式读取同一 receiver 状态�?*/
  CHECK(activity_runtime_call(v,"sdktrace/tests/PermissionActivityProbe","finish","()V",probe1,NULL,0,&out)&&!dvm_exception(v));
  memset(&out,0,sizeof out);
  CHECK(activity_runtime_call(v,"android/app/Activity","isFinishing","()Z",probe1,NULL,0,&out)&&!dvm_exception(v)&&out.i==1);
  /* Object.finish() 不是 Activity 调用，owner 校验必须拒绝�?*/
  CHECK(!activity_runtime_call(v,"java/lang/Object","finish","()V",probe1,NULL,0,&out)&&!dvm_exception(v));
  /* finishAffinity() 同样真实标记结束�?*/
  CHECK(activity_runtime_call(v,"sdktrace/tests/PermissionActivityProbe","finishAffinity","()V",probe2,NULL,0,&out)&&!dvm_exception(v));
  memset(&out,0,sizeof out);
  CHECK(activity_runtime_call(v,"sdktrace/tests/PermissionActivityProbe","isFinishing","()Z",probe2,NULL,0,&out)&&!dvm_exception(v)&&out.i==1);
  /* 其他 Activity receiver �?finish 不影响诊�?Activity 的状态�?*/
  memset(&out,0,sizeof out);
  CHECK(activity_runtime_call(v,"android/app/Activity","isFinishing","()Z",activity,NULL,0,&out)&&!dvm_exception(v)&&out.i==0);
 }
 CHECK(state.lifecycle==ACTIVITY_LIFECYCLE_CREATED);
 CHECK(activity_runtime_call(v,"android/app/Activity","finish","()V",activity,NULL,0,&out)&&!dvm_exception(v)&&state.lifecycle==ACTIVITY_LIFECYCLE_FINISHED);
 memset(&out,0,sizeof out);
 CHECK(activity_runtime_call(v,"android/app/Activity","isFinishing","()Z",activity,NULL,0,&out)&&!dvm_exception(v)&&out.i==1);
 /* 重复 finish()/finishAffinity() 幂等：状态仍�?FINISHED�?*/
 CHECK(activity_runtime_call(v,"android/app/Activity","finish","()V",activity,NULL,0,&out)&&!dvm_exception(v)&&state.lifecycle==ACTIVITY_LIFECYCLE_FINISHED);
 CHECK(activity_runtime_call(v,"android/app/Activity","finishAffinity","()V",activity,NULL,0,&out)&&!dvm_exception(v)&&state.lifecycle==ACTIVITY_LIFECYCLE_FINISHED);
 /* FINISHED 后访问器仍可用（Android �?destroy 前仍可查询）�?*/
 memset(&out,0,sizeof out);
 CHECK(activity_runtime_call(v,"android/app/Activity","getIntent","()Landroid/content/Intent;",activity,NULL,0,&out)&&!dvm_exception(v)&&out.l==intent2);
done:
 activity_selftest_probe=0;
 if(dvm_exception(v))dvm_clear_exception(v);
 state=saved;
 if(okay)sdk_trace_fprintf(stderr,"ACTIVITY_SELFTEST PASS window_feature_confirmed_mock=1 window_feature_unsupported_false=1 window_feature_error_throws=1 window_stable=1 window_no_host_fail_closed=1 windowmanager_stable=1 display_fail_closed=1 getIntent_unbound_null=1 intent_binding_roundtrip=1 setIntent=1 runOnUiThread_deferred=1 run_on_main=1 mainLooper=1 finish_state=1 finish_idempotent=1 finishAffinity_state=1 isFinishing_real_state=1 finish_null_npe=1 finish_owner_family=1 finished_receiver_isolated=1\n");
 else sdk_trace_fprintf(stderr,"ACTIVITY_SELFTEST FAIL checks=%u\n",*checks);
 if(acquired)dvm_gil_release(v);
 return okay;
#undef CHECK
}

