#include "activity_runtime.h"
#include "gc_operation.h"
#include "gc_runtime.h"
#include "view_state_runtime.h"
#include "layout_params_runtime.h"
#include "window_attributes.h"
#include "display_runtime.h"
#include "theme_runtime.h"
#include "unity_view_mount.h"
#include "android_basics.h"
#include "intent_runtime.h"
#include "bundle_runtime.h"
#include "platform_runtime.h"
#include "thread_runtime.h"
#include "guest_identity.h"
#include "json_runtime.h"
#include "reflection_runtime.h"
/* Candidate baseline keeps the older header, as in jni_bridge.c. */
bool reflection_runtime_method_handle(struct dvm *, dvm_ref, struct dvm_method **);
#include "portable.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdarg.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>
/* Engine-stage diagnostic channel: sdk_trace_fprintf stops reaching the
 * progress file once the SDK runner returns, so append directly instead. */
static void engine_probe_log(const char *fmt,...){
 FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt","a");if(!f)return;
 va_list a,b;va_start(a,fmt);va_copy(b,a);vfprintf(f,fmt,a);va_end(a);fputc('\n',f);fclose(f);
 f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/shadow-activity.txt","a");if(f){fprintf(f,"pid=%d ",(int)getpid());vfprintf(f,fmt,b);fputc('\n',f);fclose(f);}va_end(b);
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
/* Finished receivers are weak historical identities, generation-checked
 * before lookup and during weak root visitation. */
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
 uint64_t finished_generation[ACTIVITY_FINISHED_CAP];
 int nfinished;
 int finished_overflow;
 dvm_ref layout,content;
 int orientation;
 /* Screen orientation requested by the guest (setRequestedOrientation).
  * Before any explicit request the getter follows the real @ohos.display
  * snapshot; it never invents a direction. */
 int requested_orientation;
 int requested_orientation_set;
 uint32_t requested_features;
 dvm_ref intent;
 /* Last instance's retained object. Initial launches have none. Configuration
  * recreation is not implemented; no production path currently populates it. */
 dvm_ref last_nonconfiguration;
 dvm_ref window;
 dvm_ref window_manager;
 dvm_ref main_handler;
 dvm_ref theme;
 int started,resumed,window_focused;
 int intent_unbound_logged;
 int window_created_logged;
 int decor_missing_logged;
 int destroyed;
 int window_setcontent_missing_logged;
 int window_setflags_missing_logged;
 int window_manager_display_missing_logged;
 int resources_missing_logged;
 int display_metrics_missing_logged;
};
static struct activity_state state;
int activity_runtime_resume_flag(const char *name){
 char p[256];snprintf(p,sizeof p,"/data/storage/el2/base/haps/entry/files/sdkdex/%s",name);
 FILE *f=fopen(p,"r");int n=0;if(f){(void)fscanf(f,"%d",&n);fclose(f);}return n==1;
}
void activity_runtime_lifecycle_snapshot(struct dvm *v,int *life,int *started,int *resumed){
 *life=state.vm==v?state.lifecycle:0;*started=state.vm==v?state.started:0;*resumed=state.vm==v?state.resumed:0;
}
/* No Harmony service-binding host exists: Android bindService legitimately
 * returns false for an unavailable service. Never claim a successful bind. */
static bool resume_service_call(struct dvm *v,const char *m,const char *sig,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out,int enabled){
 if(!enabled)return false;
 if(!strcmp(m,"bindService")&&!strcmp(sig,"(Landroid/content/Intent;Landroid/content/ServiceConnection;I)Z")&&n==3){
  if(!self||!a||!a[0].l||!a[1].l)dvm__throw(v,"java/lang/NullPointerException","bindService context/intent/connection");
  else {out->i=0;engine_probe_log("RESUME_SERVICE bindService supported=0 returned=0 callback_sent=0");}return true;
 }
 if(!strcmp(m,"unbindService")&&!strcmp(sig,"(Landroid/content/ServiceConnection;)V")&&n==1){
  dvm__throw(v,"java/lang/IllegalArgumentException","Service not registered");return true;
 }return false;
}
bool activity_runtime_resume_selftest(struct dvm *v){
 dvm_ref refs[3]={0};const char *names[]={"android/content/Context","android/content/Intent","java/lang/Object"};bool ok=true;
 for(int i=0;i<3;i++){refs[i]=dvm_new_object(v,dvm_find_class_or_external(v,names[i]));if(refs[i])dvm_pin(v,refs[i]);else ok=false;}
 union dvm_value a[3]={{.l=refs[1]},{.l=refs[2]},{.i=1}},out={.i=7};
 const char *sig="(Landroid/content/Intent;Landroid/content/ServiceConnection;I)Z";
 ok=ok&&!resume_service_call(v,"bindService",sig,refs[0],a,3,&out,0)&&!dvm_exception(v);
 ok=ok&&resume_service_call(v,"bindService",sig,refs[0],a,3,&out,1)&&!dvm_exception(v)&&out.i==0;
 FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/resume-fail-selftest.txt","w");if(f){fprintf(f,"pid=%d disabled_unhandled=1 unsupported_bind_false=%d callbacks_sent=0 result=%s HAP=1 moving=0\n",getpid(),out.i==0,ok?"PASS":"FAIL");fclose(f);}else ok=false;
 for(int i=0;i<3;i++)if(refs[i])dvm_unpin(v,refs[i]);return ok;
}
static dvm_ref activity_window_manager(struct dvm *v);
struct guest_activity;
static struct guest_activity *guest_find(dvm_ref ref);
/* Secondary guest activities are independent of the primary Unity mount. */
#include "shadow_view_declarations.inc"
extern bool view_runtime_call(struct dvm *,const char *,const char *,const char *,dvm_ref,const union dvm_value *,int,union dvm_value *);
static struct {dvm_ref ref,context;uint64_t identity,context_identity;} shadow_inflaters[32];
struct shadow_node_state {dvm_ref ref,context,click,touch;uint64_t identity,context_identity,click_identity,touch_identity;unsigned id;int initialized,clickable,visibility;};
static struct shadow_node_state shadow_nodes[128];
static unsigned shadow_depth;
static uint64_t activity_identity(struct dvm *v,dvm_ref ref);
static bool shadow_inflater_identity(unsigned i){
 if(!shadow_inflaters[i].ref)return false;
 if(activity_identity(state.vm,shadow_inflaters[i].ref)!=shadow_inflaters[i].identity||activity_identity(state.vm,shadow_inflaters[i].context)!=shadow_inflaters[i].context_identity){if(shadow_inflaters[i].context&&activity_identity(state.vm,shadow_inflaters[i].context)==shadow_inflaters[i].context_identity)dvm_unpin(state.vm,shadow_inflaters[i].context);memset(&shadow_inflaters[i],0,sizeof shadow_inflaters[i]);return false;}return true;
}
static bool shadow_node_identity(struct shadow_node_state *n){if(!n->ref)return false;if(activity_identity(state.vm,n->ref)!=n->identity){if(n->click&&activity_identity(state.vm,n->click)==n->click_identity)dvm_unpin(state.vm,n->click);if(n->touch&&activity_identity(state.vm,n->touch)==n->touch_identity)dvm_unpin(state.vm,n->touch);memset(n,0,sizeof *n);return false;}if(n->context&&activity_identity(state.vm,n->context)!=n->context_identity)n->context=0;return true;}

static struct shadow_node_state *shadow_node(dvm_ref ref){for(unsigned i=0;i<128;i++)if(ref&&shadow_nodes[i].ref==ref&&shadow_node_identity(&shadow_nodes[i]))return &shadow_nodes[i];return NULL;}
static bool shadow_allowed(unsigned layout){for(unsigned i=0;i<sizeof shadow_layout_policies/sizeof *shadow_layout_policies;i++)if(shadow_layout_policies[i].layout==layout)return true;return false;}
static void shadow_hierarchy(struct dvm *v,struct dvm_class *k){
 if(!k||!k->external)return;
 const char *parent=NULL;
 if(!strcmp(k->name,"android/widget/RelativeLayout")||!strcmp(k->name,"android/widget/LinearLayout")||!strcmp(k->name,"android/widget/FrameLayout")||!strcmp(k->name,"android/widget/ScrollView")||!strcmp(k->name,"android/webkit/WebView"))parent="android/view/ViewGroup";
 else if(!strcmp(k->name,"android/widget/ImageView")||!strcmp(k->name,"android/widget/TextView")||!strcmp(k->name,"android/widget/ProgressBar"))parent="android/view/View";
 if(parent)k->super=dvm_find_class_or_external(v,parent);
}
static dvm_ref shadow_build(struct dvm *v,unsigned layout,dvm_ref context){
 if(!shadow_allowed(layout)){dvm__throw(v,"java/lang/UnsupportedOperationException","layout is outside declaration policy");return 0;}
 dvm_ref made[128]={0},root=0;unsigned count=0,expected=0;shadow_depth++;
 for(unsigned i=0;i<sizeof shadow_layout_nodes/sizeof *shadow_layout_nodes;i++)if(shadow_layout_nodes[i].layout==layout)expected++;
 for(unsigned i=0;i<sizeof shadow_layout_nodes/sizeof *shadow_layout_nodes;i++){
  const struct shadow_layout_node *d=&shadow_layout_nodes[i];if(d->layout!=layout)continue;
  if(d->index>=128||d->parent>=(int)d->index){dvm__throw(v,"java/lang/IllegalStateException","invalid layout declaration");break;}
  struct dvm_class *k=dvm_find_class_or_external(v,d->klass);if(!k)break;
  for(struct dvm_class *p=k;p;p=p->super)shadow_hierarchy(v,p);
  dvm_ref ref=dvm_new_object(v,k);if(!ref)break;
  struct shadow_node_state *node=NULL;for(unsigned j=0;j<128;j++)if(!shadow_nodes[j].ref){node=&shadow_nodes[j];break;}
  if(!node){dvm__throw(v,"java/lang/OutOfMemoryError","shadow View registry capacity");break;}
  node->ref=ref;node->identity=activity_identity(v,ref);node->context=context;node->context_identity=activity_identity(v,context);node->id=d->id;
  union dvm_value arg={.l=context},q={0};
  if(k->external){
   if(!strcmp(k->name,"android/webkit/WebView")){if(!android_basics_call(v,k->name,"<init>","(Landroid/content/Context;)V",ref,&arg,1,&q))break;node->initialized=1;}
   else {
    if(!view_state_runtime_call(v,"android/view/View","<init>","(Landroid/content/Context;)V",ref,&arg,1,&q))break;node->initialized=1;
    if(!strcmp(k->name,"android/widget/TextView")&&!view_runtime_call(v,k->name,"<init>","(Landroid/content/Context;)V",ref,&arg,1,&q))break;
   }
  }else {
   struct dvm_method *ctor=dvm_find_method(v,k,"<init>","(Landroid/content/Context;)V");
   if(!ctor){dvm__throw(v,"java/lang/NoSuchMethodError","shadow View Context constructor");break;}
   if(!dvm_call(v,ctor,ref,&arg,1,&q))break;
  }
  if(dvm_exception(v))break;
  /* WebView's host constructor creates View state itself. */
  if(!node->initialized){dvm_ref check[128];if(view_state_runtime_snapshot_tree(v,ref,check,128)>0)node->initialized=1;}
  if(!node->initialized){dvm__throw(v,"java/lang/IllegalStateException","shadow constructor did not initialize View");break;}
  arg.i=d->id?d->id:-1;view_state_runtime_call(v,"android/view/View","setId","(I)V",ref,&arg,1,&q);
  made[d->index]=ref;if(d->parent<0)root=ref;
  else {arg.l=ref;if(!made[d->parent]||!view_state_runtime_call(v,"android/view/ViewGroup","addView","(Landroid/view/View;)V",made[d->parent],&arg,1,&q))break;}
  if(dvm_exception(v))break;count++;
 }
 shadow_depth--;
 if(dvm_exception(v))return 0;
 if(count!=expected){dvm__throw(v,"java/lang/IllegalStateException","incomplete layout declaration construction");return 0;}
 if(!root){dvm__throw(v,"java/lang/IllegalStateException","empty layout declaration");return 0;}
 /* Inflate's completion hook is post-order, after declared children exist. */
 for(int i=127;i>=0;i--)if(made[i]){struct dvm_method *m=dvm_find_method(v,dvm_object_class(v,made[i]),"onFinishInflate","()V");union dvm_value q={0};if(m&&!dvm_call(v,m,made[i],NULL,0,&q))return 0;if(dvm_exception(v))return 0;}
 engine_probe_log("SHADOW_LAYOUT_MOUNT layout=%u root=%u nodes=%u attributes=0 rendered=0",layout,root,count);return root;
}
static bool shadow_layout_call(struct dvm *v,const char *c,const char *m,const char *sig,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 if(state.vm!=v)return false;
 /* View.inflate is inherited by FrameLayout and other framework View classes.
  * Android delegates to LayoutInflater.from(context).inflate(id, root). */
 if(!strcmp(m,"inflate")&&!strcmp(sig,"(Landroid/content/Context;ILandroid/view/ViewGroup;)Landroid/view/View;")&&n==3&&(!strcmp(c,"android/view/View")||!strncmp(c,"android/widget/",15))){
  union dvm_value inflater={0};if(!shadow_layout_call(v,"android/view/LayoutInflater","from","(Landroid/content/Context;)Landroid/view/LayoutInflater;",0,a,1,&inflater)||dvm_exception(v))return true;
  union dvm_value args[2]={a[1],a[2]};return shadow_layout_call(v,"android/view/LayoutInflater","inflate","(ILandroid/view/ViewGroup;)Landroid/view/View;",inflater.l,args,2,out);
 }
 if(!strcmp(m,"getSystemService")&&!strcmp(sig,"(Ljava/lang/String;)Ljava/lang/Object;")&&n==1){const char *service=dvm_string_utf8(v,a[0].l);if(service&&!strcmp(service,"window")){out->l=activity_window_manager(v);return true;}if(service&&!strcmp(service,"layout_inflater")){union dvm_value context={.l=self};return shadow_layout_call(v,"android/view/LayoutInflater","from","(Landroid/content/Context;)Landroid/view/LayoutInflater;",0,&context,1,out);}}
 if(state.window_manager&&self==state.window_manager&&!strcmp(m,"getDefaultDisplay")&&!strcmp(sig,"()Landroid/view/Display;")){out->l=display_runtime_default(v);return true;}
 if(guest_find(self)&&!strcmp(m,"getWindowManager")&&!strcmp(sig,"()Landroid/view/WindowManager;")){out->l=activity_window_manager(v);return true;}
 if(!strcmp(c,"android/view/LayoutInflater")){
  if(!strcmp(m,"from")&&!strcmp(sig,"(Landroid/content/Context;)Landroid/view/LayoutInflater;")&&n==1){
   if(!a[0].l){dvm__throw(v,"java/lang/NullPointerException","LayoutInflater Context");return true;}
   for(unsigned i=0;i<32;i++)if(shadow_inflater_identity(i)&&shadow_inflaters[i].context==a[0].l){out->l=shadow_inflaters[i].ref;return true;}
   for(unsigned i=0;i<32;i++)if(!shadow_inflaters[i].ref){out->l=dvm_new_object(v,dvm_find_class_or_external(v,c));shadow_inflaters[i].ref=out->l;shadow_inflaters[i].identity=activity_identity(v,out->l);shadow_inflaters[i].context=a[0].l;shadow_inflaters[i].context_identity=activity_identity(v,a[0].l);dvm_pin(v,a[0].l);return true;}
   dvm__throw(v,"java/lang/OutOfMemoryError","shadow inflater registry");return true;
  }
  if(!strcmp(m,"inflate")&&((n==2&&!strcmp(sig,"(ILandroid/view/ViewGroup;)Landroid/view/View;"))||(n==3&&!strcmp(sig,"(ILandroid/view/ViewGroup;Z)Landroid/view/View;")))){
   dvm_ref context=0;for(unsigned i=0;i<32;i++)if(shadow_inflater_identity(i)&&shadow_inflaters[i].ref==self)context=shadow_inflaters[i].context;
   if(!context){dvm__throw(v,"java/lang/IllegalStateException","unbound LayoutInflater");return true;}
   bool attach=n==2?a[1].l!=0:a[2].i!=0;
   if(attach&&!a[1].l){dvm__throw(v,"java/lang/IllegalArgumentException","attach requires root ViewGroup");return true;}
   dvm_ref child=shadow_build(v,(unsigned)a[0].i,context);if(!child)return true;
   if(attach){union dvm_value arg={.l=child};if(!view_state_runtime_call(v,"android/view/ViewGroup","addView","(Landroid/view/View;)V",a[1].l,&arg,1,out)){dvm__throw(v,"java/lang/IllegalArgumentException","unconstructed inflate parent");return true;}out->l=a[1].l;}
   else out->l=child;return true;
  }return false;
 }
 struct shadow_node_state *node=shadow_node(self);if(!node)return false;
 if(!strcmp(m,"onFinishInflate")&&!strcmp(sig,"()V")&&!n)return true;
 if(shadow_depth&&!strcmp(m,"<init>")&&(!strncmp(c,"android/widget/",15)||!strcmp(c,"android/view/View"))&&n>=1){
  for(struct dvm_class *p=dvm_object_class(v,self);p;p=p->super)shadow_hierarchy(v,p);
  union dvm_value arg={.l=node->context};
  if(!node->initialized){if(!view_state_runtime_call(v,"android/view/View","<init>","(Landroid/content/Context;)V",self,&arg,1,out))return false;node->initialized=1;}
  if(!strcmp(c,"android/widget/TextView"))return view_runtime_call(v,c,"<init>","(Landroid/content/Context;)V",self,&arg,1,out);
  return true;
 }
 if(n==1&&((!strcmp(m,"setOnClickListener")&&!strcmp(sig,"(Landroid/view/View$OnClickListener;)V"))||(!strcmp(m,"setOnTouchListener")&&!strcmp(sig,"(Landroid/view/View$OnTouchListener;)V")))){
  dvm_ref *slot=!strcmp(m,"setOnClickListener")?&node->click:&node->touch;
  uint64_t *generation=!strcmp(m,"setOnClickListener")?&node->click_identity:&node->touch_identity;
  if(a[0].l)dvm_pin(v,a[0].l);if(*slot&&activity_identity(v,*slot)==*generation)dvm_unpin(v,*slot);*slot=a[0].l;*generation=activity_identity(v,a[0].l);return true;
 }
 if(!strcmp(m,"setClickable")&&!strcmp(sig,"(Z)V")&&n==1){node->clickable=a[0].i!=0;return true;}
 if(!strcmp(m,"isClickable")&&!strcmp(sig,"()Z")&&!n){out->i=node->clickable;return true;}
 if(!strcmp(m,"setVisibility")&&!strcmp(sig,"(I)V")&&n==1){
  if(a[0].i!=0&&a[0].i!=4&&a[0].i!=8){dvm__throw(v,"java/lang/IllegalArgumentException","shadow visibility must be VISIBLE, INVISIBLE or GONE");return true;}
  node->visibility=a[0].i;return true;
 }
 if(!strcmp(m,"getVisibility")&&!strcmp(sig,"()I")&&!n){out->i=node->visibility;return true;}
 if(!strcmp(m,"findViewById")&&!strcmp(sig,"(I)Landroid/view/View;")&&n==1){
  dvm_ref refs[128];int count=view_state_runtime_snapshot_tree(v,self,refs,128);out->l=0;
  for(int i=0;i<count;i++){union dvm_value id={0};view_state_runtime_call(v,"android/view/View","getId","()I",refs[i],NULL,0,&id);if(a[0].i!=-1&&id.i==a[0].i){out->l=refs[i];break;}}return true;
 }
 return false;
}
struct guest_activity {dvm_ref ref,caller,intent,window,root,result,params,last_nonconfiguration;int request,code,phase,delivered,status_color,finish_outside;uint32_t features,layout;uint64_t generations[8];};
static struct guest_activity guests[32];
static bool guest_minimal_layout;
static struct {dvm_ref activity;uint64_t identity;int enter,exit;} transitions[64];
static uint64_t activity_identity(struct dvm *v,dvm_ref ref){struct dvm_object *o=dvm__obj(v,ref);return o?o->identity_generation:0;}
static void guest_slots(struct guest_activity *g,dvm_ref **r){r[0]=&g->ref;r[1]=&g->caller;r[2]=&g->intent;r[3]=&g->window;r[4]=&g->root;r[5]=&g->result;r[6]=&g->params;r[7]=&g->last_nonconfiguration;}
static void guest_remember(struct dvm *v,struct guest_activity *g){dvm_ref *r[8];guest_slots(g,r);for(unsigned j=0;j<8;j++)g->generations[j]=activity_identity(v,*r[j]);}
static bool activity_gc_strong(struct dvm *v,dvm_gc_visit_fn visit,void *u,void *ctx){
 if(ctx!=&state||!dvm_gil_held(v))return false;if(state.vm!=v)return true;
 /* Host primary facade remains process-owned until VM teardown. */
 dvm_ref *host[]={&state.activity,&state.application,&state.layout,&state.content,&state.intent,&state.last_nonconfiguration,&state.window,&state.window_manager,&state.main_handler,&state.theme};
 for(unsigned i=0;i<sizeof host/sizeof *host;i++)if(*host[i])visit(host[i],u);
 for(unsigned i=0;i<32;i++)if(guests[i].ref&&guests[i].phase<4){dvm_ref *r[8];guest_slots(&guests[i],r);for(unsigned j=0;j<8;j++)if(*r[j])visit(r[j],u);}
 return true;
}
static bool activity_gc_weak(struct dvm *v,dvm_gc_visit_fn visit,void *u,void *ctx){
 if(ctx!=&state||!dvm_gil_held(v))return false;if(state.vm!=v)return true;
 if(state.nfinished<0||state.nfinished>ACTIVITY_FINISHED_CAP)return false;
 for(unsigned i=0;i<32;i++)if(shadow_inflater_identity(i)){visit(&shadow_inflaters[i].ref,u);if(!shadow_inflaters[i].ref){if(shadow_inflaters[i].context&&activity_identity(v,shadow_inflaters[i].context)==shadow_inflaters[i].context_identity)dvm_unpin(v,shadow_inflaters[i].context);memset(&shadow_inflaters[i],0,sizeof shadow_inflaters[i]);}}
 for(unsigned i=0;i<128;i++)if(shadow_node_identity(&shadow_nodes[i])){visit(&shadow_nodes[i].ref,u);if(!shadow_nodes[i].ref){if(shadow_nodes[i].click&&activity_identity(v,shadow_nodes[i].click)==shadow_nodes[i].click_identity)dvm_unpin(v,shadow_nodes[i].click);if(shadow_nodes[i].touch&&activity_identity(v,shadow_nodes[i].touch)==shadow_nodes[i].touch_identity)dvm_unpin(v,shadow_nodes[i].touch);memset(&shadow_nodes[i],0,sizeof shadow_nodes[i]);}}
 for(unsigned i=0;i<64;i++){if(transitions[i].activity&&activity_identity(v,transitions[i].activity)!=transitions[i].identity)memset(&transitions[i],0,sizeof transitions[i]);if(transitions[i].activity){visit(&transitions[i].activity,u);if(!transitions[i].activity)memset(&transitions[i],0,sizeof transitions[i]);}}

 for(int i=0;i<state.nfinished;i++){dvm_ref *r=&state.finished[i];if(*r&&activity_identity(v,*r)!=state.finished_generation[i])*r=0;if(*r)visit(r,u);if(!*r)state.finished_generation[i]=0;}
 for(unsigned i=0;i<32;i++)if(guests[i].ref&&guests[i].phase>=4){dvm_ref *r[8];guest_slots(&guests[i],r);for(unsigned j=0;j<8;j++){if(*r[j]&&activity_identity(v,*r[j])!=guests[i].generations[j])*r[j]=0;if(*r[j])visit(r[j],u);if(!*r[j])guests[i].generations[j]=0;}if(!guests[i].ref)memset(&guests[i],0,sizeof guests[i]);}
 return true;
}
static bool activity_gc_ensure(struct dvm *v){
 if(state.vm!=v)return false;
 if(dvm_gc_root_provider_add(v,"activity-runtime/host",DVM_GC_ROOT_STRONG,activity_gc_strong,&state)&&dvm_gc_root_provider_add(v,"activity-runtime/history",DVM_GC_ROOT_WEAK,activity_gc_weak,&state))return true;
 dvm__throw(v,"java/lang/OutOfMemoryError","Activity root registration");return false;
}
void activity_runtime_gc_vm_end(struct dvm *v){
 dvm_gc_root_provider_remove(v,"activity-runtime/history",&state);dvm_gc_root_provider_remove(v,"activity-runtime/host",&state);
 if(state.vm==v){memset(&state,0,sizeof state);memset(guests,0,sizeof guests);memset(transitions,0,sizeof transitions);}
}

void activity_runtime_task5_minimal_layout(struct dvm *v){if(state.vm==v)guest_minimal_layout=true;}
static struct guest_activity *guest_find(dvm_ref ref){for(unsigned i=0;i<32;i++)if(guests[i].ref==ref&&ref){if(guests[i].phase>=4&&activity_identity(state.vm,ref)!=guests[i].generations[0]){memset(&guests[i],0,sizeof guests[i]);return NULL;}return &guests[i];}return NULL;}
/* Explicit diagnostic accessor: expose existing state, never create a presenter. */
bool activity_runtime_probe_context(struct dvm *v,const char *component,dvm_ref *activity,dvm_ref *presenter){
 for(unsigned i=0;i<32;i++)if(guests[i].ref&&guests[i].root){
  struct dvm_object *o=dvm__obj(v,guests[i].ref);
  if(o&&o->cls&&!strcmp(o->cls->name,component)){
   union dvm_value p={0};if(!dvm_get_field(v,guests[i].ref,"mPresenter","Lcom/gsc/base/mvp/a;",&p))return false;
   *activity=guests[i].ref;*presenter=p.l;return p.l!=0;
  }
 }return false;
}
bool activity_runtime_record_transition(struct dvm *v,dvm_ref self,int enter,int exit){
 if(state.vm!=v)return false;
 unsigned i;for(i=0;i<64;i++){if(transitions[i].activity&&activity_identity(v,transitions[i].activity)!=transitions[i].identity)memset(&transitions[i],0,sizeof transitions[i]);if(transitions[i].activity==self||!transitions[i].activity)break;}
 if(i==64){dvm__throw(v,"java/lang/OutOfMemoryError","transition records");return true;}
 transitions[i].activity=self;transitions[i].identity=activity_identity(v,self);transitions[i].enter=enter;transitions[i].exit=exit;
 engine_probe_log("TASK5_TRANSITION activity=%u enter=%d exit=%d rendered=0",self,enter,exit);return true;
}

static bool guest_invoke(struct dvm *v,dvm_ref ref,const char *name,const char *sig,const union dvm_value *a,int n){
 struct dvm_method *method=dvm_find_method(v,dvm__obj(v,ref)->cls,name,sig);union dvm_value out={0};
 if(!method){dvm__throw(v,"java/lang/NoSuchMethodError","Activity.%s%s",name,sig);return false;}
 return dvm_call(v,method,ref,a,n,&out)&&!dvm_exception(v);
}
static bool guest_call(struct dvm *,const char *,const char *,const char *,dvm_ref,const union dvm_value *,int,union dvm_value *);
/* Logical SDK context only: no Android UI lifecycle and no content root. */
bool activity_runtime_arkts_login_backend(struct dvm *v,dvm_ref *activity,dvm_ref *presenter){
 if(state.vm!=v||!activity_gc_ensure(v))return false;
 struct guest_activity *g=NULL;for(unsigned i=0;i<32;i++)if(!guests[i].ref){g=&guests[i];break;}
 if(!g){dvm__throw(v,"java/lang/OutOfMemoryError","login backend Activity records");return false;}
 struct dvm_class *cls=dvm_find_class(v,"com/gsc/account_login/AccountLoginActivity");
 if(!cls||!dvm_init_class(v,cls))return false;
 g->ref=dvm_new_object(v,cls);g->caller=state.activity;g->request=-1;
 if(!g->ref||!guest_invoke(v,g->ref,"<init>","()V",NULL,0))return false;
 /* Preserve BaseActivity's non-UI setup, including original Router service
  * injection. UserInfoService/RouteProcessService finish the real SDK login
  * and persist its state; a presenter alone is insufficient. */
 struct dvm_class *router=dvm_find_class(v,"com/base/router/launcher/Router");
 struct dvm_method *get=router?dvm_find_method(v,router,"getInstance","()Lcom/base/router/launcher/Router;"):NULL;
 union dvm_value value={0},self={.l=g->ref};
 if(!get||!dvm_call(v,get,0,NULL,0,&value)||dvm_exception(v)||!value.l||
    !guest_invoke(v,value.l,"inject","(Ljava/lang/Object;)V",&self,1)||
    !dvm_set_field(v,g->ref,"mContext","Landroid/content/Context;",self)||
    !guest_invoke(v,g->ref,"setPresenter","()V",NULL,0))return false;
 union dvm_value service={0},user={0};
 if(!dvm_get_field(v,g->ref,"m","Lcom/gsc/base/service/RouteProcessService;",&service)||!service.l||
    !dvm_get_field(v,g->ref,"n","Lcom/gsc/base/service/UserInfoService;",&user)||!user.l){
  if(!dvm_exception(v))dvm__throw(v,"java/lang/IllegalStateException","login backend SDK services unavailable");return false;
 }
 union dvm_value q={0};if(!dvm_get_field(v,g->ref,"mPresenter","Lcom/gsc/base/mvp/a;",&q)||!q.l)return false;
 *activity=g->ref;*presenter=q.l;
 engine_probe_log("LOGIN_BACKEND arkts=1 sdk_context=1 presenter=1 original_layout_inflated=0");return true;
}
bool activity_runtime_arkts_saved_login(struct dvm *v,dvm_ref model){
 if(state.vm!=v||!model||!activity_gc_ensure(v))return false;
 struct guest_activity *g=NULL;for(unsigned i=0;i<32;i++)if(!guests[i].ref){g=&guests[i];break;}
 if(!g)return false;
 struct dvm_class *cls=dvm_find_class(v,"com/gsc/auto_login/AutoLoginActivity");
 if(!cls||!dvm_init_class(v,cls))return false;
 g->ref=dvm_new_object(v,cls);g->caller=state.activity;g->request=-1;
 union dvm_value q={0},arg={.l=g->ref};
 if(!g->ref||!guest_invoke(v,g->ref,"<init>","()V",NULL,0))return false;
 struct dvm_class *ic=dvm_find_class_or_external(v,"android/content/Intent");
 g->intent=ic?dvm_new_object(v,ic):0;
 if(!g->intent||!intent_runtime_call(v,"android/content/Intent","<init>","()V",g->intent,NULL,0,&q)||dvm_exception(v))return false;
 dvm_ref bundle=bundle_runtime_new(v);union dvm_value pair[2]={{.l=dvm_new_string(v,"model")},{.l=model}};
 if(!bundle||!bundle_runtime_call(v,"android/os/Bundle","putParcelable","(Ljava/lang/String;Landroid/os/Parcelable;)V",bundle,pair,2,&q)||dvm_exception(v))return false;
 union dvm_value extras={.l=bundle};
 if(!intent_runtime_call(v,"android/content/Intent","putExtras","(Landroid/os/Bundle;)Landroid/content/Intent;",g->intent,&extras,1,&q)||dvm_exception(v))return false;
 struct dvm_class *rc=dvm_find_class(v,"com/base/router/launcher/Router");
 struct dvm_method *get=rc?dvm_find_method(v,rc,"getInstance","()Lcom/base/router/launcher/Router;"):NULL;
 if(!get||!dvm_call(v,get,0,NULL,0,&q)||dvm_exception(v)||!q.l||
    !guest_invoke(v,q.l,"inject","(Ljava/lang/Object;)V",&arg,1)||
    !dvm_set_field(v,g->ref,"mContext","Landroid/content/Context;",arg)||
    !guest_invoke(v,g->ref,"initData","()V",NULL,0))return false;
 union dvm_value service={0};
 if(!dvm_get_field(v,g->ref,"a","Lcom/gsc/base/service/ICommonService;",&service)||!service.l)return false;
 struct dvm_class *cc=dvm_find_class(v,"com/gsc/auto_login/AutoLoginActivity$a");
 dvm_ref callback=cc?dvm_new_object(v,cc):0;
 if(!callback||!guest_invoke(v,callback,"<init>","(Lcom/gsc/auto_login/AutoLoginActivity;)V",&arg,1))return false;
 union dvm_value args[3]={{.l=model},{.l=g->ref},{.l=callback}};
 bool ok=guest_invoke(v,service.l,"doAutoLogin","(Lcom/gsc/base/model/UserInfoModel;Landroid/content/Context;Lcom/gsc/base/interfaces/InAutoLoginCallback;)V",args,3);
 engine_probe_log("SAVED_LOGIN original_service=doAutoLogin original_callback=AutoLoginActivity$a original_layout_inflated=0 started=%d exception=%u",ok,dvm_exception(v));return ok;
}
static bool announcement_skip_class(const char *name){return name&&!strcmp(name,"com/gsc/announcement/AnnouncementActivity");}
bool activity_runtime_ui_policy_selftest(struct dvm *v){
 FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/ui-correct-selftest.txt","w");if(!f)return false;
 bool ok=announcement_skip_class("com/gsc/announcement/AnnouncementActivity")&&!announcement_skip_class("com/gsc/web/AgreementWebActivity")&&!announcement_skip_class("com/gsc/phone_login/PhoneLoginActivity")&&!announcement_skip_class(NULL);
 fprintf(f,"pid=%d announcement_exact=%d agreement_preserved=1 login_not_skipped=1 layout_inflated=0 result=%s\n",getpid(),ok,ok?"PASS":"FAIL");fclose(f);return ok;
}
bool activity_runtime_launch_guest(struct dvm *v,dvm_ref caller,struct dvm_class *target,dvm_ref intent,int request){
 if(state.vm!=v||!target||target->external)return false;
 if(!activity_gc_ensure(v))return true;
 struct dvm_class *base=dvm_find_class_or_external(v,"android/app/Activity");
 if(!base||!dvm__class_assignable(v,target,base))return false;
 struct guest_activity *g=NULL;for(unsigned i=0;i<32;i++)if(!guests[i].ref){g=&guests[i];break;}
 if(!g){dvm__throw(v,"java/lang/OutOfMemoryError","guest Activity records");return true;}
 if(!dvm_init_class(v,target))return true;
 g->ref=dvm_new_object(v,target);g->caller=caller;g->intent=intent;g->request=request;
 if(!g->ref)return true;
 if(announcement_skip_class(target->name)){
  /* Explicit host UI policy: no Android content is constructed. Use the same
   * finish/result delivery as an ordinary close, without inventing a result. */
  struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);
  engine_probe_log("ANNOUNCEMENT_SKIPPED class=%s mono_ms=%llu ref=%u request=%d original_layout_inflated=0",target->name,(unsigned long long)t.tv_sec*1000+t.tv_nsec/1000000,g->ref,request);
  union dvm_value q={0};if(!guest_invoke(v,g->ref,"<init>","()V",NULL,0))return true;
  /* onCreate normally injects /auth/announcement. Its onDestroy emits both
   * cb_announcement (initialization) and notifyFinish (post-login routing).
   * Skipping Android content must preserve both original notifications. */
  struct dvm_class *router=dvm_find_class(v,"com/base/router/launcher/Router");
  struct dvm_method *get_router=router?dvm_find_method(v,router,"getInstance","()Lcom/base/router/launcher/Router;"):NULL;
  union dvm_value activity_arg={.l=g->ref};
  bool injected=get_router&&dvm_call(v,get_router,0,NULL,0,&q)&&!dvm_exception(v)&&q.l;
  if(injected)injected=guest_invoke(v,q.l,"inject","(Ljava/lang/Object;)V",&activity_arg,1);
  if(!injected){if(!dvm_exception(v))dvm__throw(v,"java/lang/IllegalStateException","announcement route injection unavailable");return true;}
  guest_call(v,"android/app/Activity","finish","()V",g->ref,NULL,0,&q);
  /* Skipping onCreate also skips onDestroy, which normally emits this close
   * notification. Deliver the original SDK callback (empty Bundle), otherwise
   * InitAnnouncementRouteService never completes and login returns 3004. */
  if(!dvm_exception(v)){
   struct dvm_class *mc=dvm_find_class(v,"com/base/commonlib/callback/CallbackManager");
   struct dvm_method *get=mc?dvm_find_method(v,mc,"getInstance","()Lcom/base/commonlib/callback/CallbackManager;"):NULL;
   bool ok=get&&dvm_call(v,get,0,NULL,0,&q)&&!dvm_exception(v);dvm_ref manager=q.l;
   /* Retrieve the original callback object; no synthesized init result. */
   struct dvm_method *method=ok?dvm_find_method(v,dvm__obj(v,manager)->cls,"getCallback","()Lcom/base/commonlib/callback/CallbackManager$InternalCallback;"):NULL;
   ok=ok&&method&&dvm_call(v,method,manager,NULL,0,&q)&&!dvm_exception(v);dvm_ref callback=q.l;
   dvm_ref bundle=ok?bundle_runtime_new(v):0;
   ok=ok&&bundle&&!dvm_exception(v);
   union dvm_value args[2]={{.l=ok?dvm_new_string(v,"cb_announcement"):0},{.l=bundle}};
   if(ok)ok=guest_invoke(v,callback,"callback","(Ljava/lang/String;Landroid/os/Bundle;)V",args,2);
   engine_probe_log("ANNOUNCEMENT_CLOSE_CALLBACK delivered=%d original_key=cb_announcement layout_inflated=0 exception=%u",ok,dvm_exception(v));
   if(!ok&&!dvm_exception(v))dvm__throw(v,"java/lang/IllegalStateException","announcement close callback unavailable");
  }
  if(!dvm_exception(v)){
   union dvm_value route={0};
   bool ok=dvm_get_field(v,g->ref,"o","Lcom/gsc/base/service/RouteProcessService;",&route)&&route.l;
   if(ok)ok=guest_invoke(v,route.l,"notifyFinish","()V",NULL,0);
   engine_probe_log("ANNOUNCEMENT_ROUTE_FINISHED injected=%d service_present=%d delivered=%d original_layout_inflated=0 exception=%u",injected,route.l!=0,ok,dvm_exception(v));
   if(!ok&&!dvm_exception(v))dvm__throw(v,"java/lang/IllegalStateException","announcement route continuation unavailable");
  }
  engine_probe_log("ANNOUNCEMENT_FINISHED ref=%u phase=%d delivered=%d exception=%u",g->ref,g->phase,g->delivered,dvm_exception(v));
  return true;
 }
 if(!strcmp(target->name,"com/gsc/welcome/WelcomeActivity")){
  /* The SDK's three-second greeting ends in b(): finishAll + notifyFinish.
   * Preserve that exact continuation, with the real injected model/services,
   * while omitting Android content under the ArkTS UI policy. */
  union dvm_value q={0},arg={.l=g->ref},service={0},model={0};
  bool ok=guest_invoke(v,g->ref,"<init>","()V",NULL,0);
  struct dvm_class *router=ok?dvm_find_class(v,"com/base/router/launcher/Router"):NULL;
  struct dvm_method *get=router?dvm_find_method(v,router,"getInstance","()Lcom/base/router/launcher/Router;"):NULL;
  ok=ok&&get&&dvm_call(v,get,0,NULL,0,&q)&&!dvm_exception(v)&&q.l;
  if(ok)ok=guest_invoke(v,q.l,"inject","(Ljava/lang/Object;)V",&arg,1);
  ok=ok&&dvm_get_field(v,g->ref,"e","Lcom/gsc/base/model/UserInfoModel;",&model)&&model.l&&
     dvm_get_field(v,g->ref,"f","Lcom/gsc/base/service/RouteProcessService;",&service)&&service.l;
  if(ok)ok=guest_invoke(v,g->ref,"b","()V",NULL,0);
  if(ok)guest_call(v,"android/app/Activity","finish","()V",g->ref,NULL,0,&q);
  engine_probe_log("WELCOME_ARKTS_SKIP original_close=b model_present=%d service_present=%d delivered=%d original_layout_inflated=0 exception=%u",model.l!=0,service.l!=0,ok,dvm_exception(v));
  if(!ok&&!dvm_exception(v))dvm__throw(v,"java/lang/IllegalStateException","welcome SDK continuation unavailable");
  return true;
 }
 const char *methods[]={"<init>","onCreate","onStart","onResume"};
 union dvm_value bundle={0};
 for(int i=0;i<4;i++){
  bool ok=guest_invoke(v,g->ref,methods[i],i==1?"(Landroid/os/Bundle;)V":"()V",i==1?&bundle:NULL,i==1?1:0);
  char error[1024]={0};if(dvm_exception(v))dvm_describe_exception(v,dvm_exception(v),error,sizeof error);
  engine_probe_log("ACTIVITY_GUEST class=%s ref=%u callback=%s ok=%d exception=%s",target->name,g->ref,methods[i],ok,error);
  if(!ok)return true;
 }
 return true;
}
static bool guest_params_assignable(struct dvm *v,dvm_ref ref){
 struct gc_operation gc7 __attribute__((cleanup(gc_operation_end)));
 if(!gc_operation_begin(v,&gc7)||!gc_operation_admit(v,ref))return false;
 struct dvm_class *expected=dvm_find_class_or_external(v,"android/view/WindowManager$LayoutParams");
 struct dvm_object *params=gc_operation_object(v,ref);
 return params&&dvm__class_assignable(v,params->cls,expected);
}
static bool guest_call(struct dvm *v,const char *c,const char *m,const char *sig,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 if(state.vm!=v)return false;
 struct guest_activity *g=guest_find(self);
 for(unsigned i=0;!g&&i<32;i++)if(guests[i].window==self&&self){
  g=&guests[i];
  if(!strcmp(m,"getAttributes")&&!strcmp(sig,"()Landroid/view/WindowManager$LayoutParams;")){if(!g->params)g->params=window_attributes_get(v,self);out->l=g->params;return true;}
  if(!strcmp(m,"setAttributes")&&!strcmp(sig,"(Landroid/view/WindowManager$LayoutParams;)V")&&n==1){
   if(!guest_params_assignable(v,a[0].l)){dvm__throw(v,"java/lang/IllegalArgumentException","Window LayoutParams");return true;}
   g->params=a[0].l;engine_probe_log("TASK5_WINDOW_ATTRIBUTES window=%u params=%u guest_only=1 host_applied=0",self,g->params);return true;
  }
  if(!strcmp(m,"getDecorView")&&!strcmp(sig,"()Landroid/view/View;")){out->l=view_state_runtime_create_activity_decor(v,g->ref);return true;}
  if((!strcmp(m,"addFlags")&&!strcmp(sig,"(I)V"))||(!strcmp(m,"clearFlags")&&!strcmp(sig,"(I)V"))||(!strcmp(m,"setFlags")&&!strcmp(sig,"(II)V"))){
   if(!g->params)g->params=window_attributes_get(v,self);union dvm_value flags={0};
   if(dvm_exception(v)||!dvm_get_field(v,g->params,"flags","I",&flags))return true;
   int mask=!strcmp(m,"setFlags")?a[1].i:a[0].i;
   flags.i=(flags.i&~mask)|(!strcmp(m,"clearFlags")?0:(a[0].i&mask));dvm_set_field(v,g->params,"flags","I",flags);
   engine_probe_log("TASK5_GUEST_WINDOW_FLAGS window=%u value=0x%x host_applied=0",self,flags.i);return true;
  }
  if(!strcmp(m,"setStatusBarColor")&&!strcmp(sig,"(I)V")&&n==1){g->status_color=a[0].i;return true;}
  if(!strcmp(m,"getStatusBarColor")&&!strcmp(sig,"()I")){out->i=g->status_color;return true;}
  if((!strcmp(m,"setContentView")&&!strcmp(sig,"(Landroid/view/View;)V"))||(!strcmp(m,"findViewById")&&!strcmp(sig,"(I)Landroid/view/View;")))return guest_call(v,"android/app/Activity",m,sig,g->ref,a,n,out);
  return false;
 }
 if(!g)return false;
 /* Captcha already has a declared SDK binding tree and a real ArkWeb host.
  * Keep that existing headless tree available independently of the removed
  * diagnostic-layout switch. This path never invokes host_mount. */
 struct dvm_object *guest_object=dvm__obj(v,g->ref);
 /* These exact WebView pages share the existing declared binding tree and
  * ArkWeb renderer; never mount Android content over Unity. */
 bool captcha_arkts=guest_object&&guest_object->cls&&
  (!strcmp(guest_object->cls->name,"com/gsc/captcha/CaptchaActivity")||
   !strcmp(guest_object->cls->name,"com/base/gsc_agreement_library/AgreementWebActivity"));
 if(!strcmp(m,"requestWindowFeature")&&!strcmp(sig,"(I)Z")&&n==1){
  if(a[0].i<0||a[0].i>=32){dvm__throw(v,"java/lang/IllegalArgumentException","window feature id");return true;}
  g->features|=1u<<a[0].i;out->i=1;engine_probe_log("TASK5_GUEST_FEATURE activity=%u feature=%d mask=0x%x host_applied=0",self,a[0].i,g->features);return true;
 }
 if(!strcmp(c,"android/app/Activity")&&!strcmp(m,"<init>")&&!strcmp(sig,"()V"))return true;
 if(!strcmp(c,"android/app/Activity")&&!strcmp(m,"onCreate")&&!strcmp(sig,"(Landroid/os/Bundle;)V")){g->phase=1;return true;}
 if(!strcmp(c,"android/app/Activity")&&!strcmp(sig,"()V")){
  if(!strcmp(m,"onStart")){g->phase=2;return true;}
  if(!strcmp(m,"onResume")){g->phase=3;return true;}
  if(!strcmp(m,"onPause")){g->phase=2;return true;}
  if(!strcmp(m,"onStop")){g->phase=1;return true;}
  if(!strcmp(m,"onDestroy")){guest_remember(v,g);g->phase=5;return true;}
 }
 /* Guest Window policy storage only; no host input/gesture dispatch. */
 if(!strcmp(m,"setFinishOnTouchOutside")&&!strcmp(sig,"(Z)V")&&n==1){
  g->finish_outside=a[0].i!=0;engine_probe_log("TASK5_GUEST_WINDOW_POLICY activity=%u finish_outside=%d host_input_changed=0",self,g->finish_outside);return true;
 }
 if(!strcmp(m,"getIntent")&&!strcmp(sig,"()Landroid/content/Intent;")){out->l=g->intent;return true;}
 if(!strcmp(m,"setIntent")&&!strcmp(sig,"(Landroid/content/Intent;)V")&&n==1){g->intent=a[0].l;return true;}
 if(!strcmp(m,"getWindow")&&!strcmp(sig,"()Landroid/view/Window;")){
  if(!g->window)g->window=dvm_new_object(v,dvm_find_class_or_external(v,"android/view/Window"));out->l=g->window;return true;
 }
 if(!strcmp(m,"getApplication")&&!strcmp(sig,"()Landroid/app/Application;")){out->l=android_basics_application(v);return true;}
 if((guest_minimal_layout||captcha_arkts)&&!strcmp(m,"setContentView")&&!strcmp(sig,"(I)V")&&n==1){
  g->layout=(uint32_t)a[0].i;
  if(captcha_arkts){engine_probe_log("CAPTCHA_ARKTS sdk_binding_tree=1 host_mount=0 layout=%u",g->layout);if(!shadow_allowed(g->layout)){dvm__throw(v,"java/lang/UnsupportedOperationException","captcha layout outside existing declaration policy");return true;}}
  if(shadow_allowed(g->layout)){dvm_ref declared=shadow_build(v,g->layout,self);if(!declared)return true;union dvm_value arg={.l=declared};return guest_call(v,"android/app/Activity","setContentView","(Landroid/view/View;)V",self,&arg,1,out);}
  dvm_ref root=dvm_new_object(v,dvm_find_class_or_external(v,"android/widget/FrameLayout"));
  dvm_ref child=dvm_new_object(v,dvm_find_class_or_external(v,"android/view/View"));union dvm_value context={.l=self},x={.l=child};
  if(!view_state_runtime_call(v,"android/widget/FrameLayout","<init>","(Landroid/content/Context;)V",root,&context,1,out)||dvm_exception(v))return true;
  if(!view_state_runtime_call(v,"android/view/View","<init>","(Landroid/content/Context;)V",child,&context,1,out)||dvm_exception(v))return true;
  union dvm_value id={.i=1};view_state_runtime_call(v,"android/view/View","setId","(I)V",child,&id,1,out);
  if(!view_state_runtime_call(v,"android/view/ViewGroup","addView","(Landroid/view/View;)V",root,&x,1,out)||dvm_exception(v))return true;
  engine_probe_log("TASK5_MINIMAL_LAYOUT requested_resource=%d xml_inflated=0 widget_created=0",a[0].i);
  x.l=root;return guest_call(v,"android/app/Activity","setContentView","(Landroid/view/View;)V",self,&x,1,out);
 }
 if(!strcmp(m,"setContentView")&&!strcmp(sig,"(Landroid/view/View;)V")&&n==1){
  dvm_ref views[1024];int count=view_state_runtime_snapshot_tree(v,a[0].l,views,1024);
  if(count<1){dvm__throw(v,"java/lang/IllegalArgumentException","unconstructed guest content View");return true;}
  dvm_ref decor=view_state_runtime_create_activity_decor(v,self);union dvm_value child={.l=a[0].l};
  if(g->root){view_state_runtime_set_attached(v,decor,false);union dvm_value old={.l=g->root};view_state_runtime_call(v,"android/view/ViewGroup","removeView","(Landroid/view/View;)V",decor,&old,1,out);}
  if(dvm_exception(v))return true;
  union dvm_value params={0};view_state_runtime_call(v,"android/view/View","getLayoutParams","()Landroid/view/ViewGroup$LayoutParams;",child.l,NULL,0,&params);
  if(!params.l){params.l=dvm_new_object(v,dvm_find_class_or_external(v,"android/widget/FrameLayout$LayoutParams"));union dvm_value wh[2]={{.i=-1},{.i=-1}};if(!params.l||!layout_params_runtime_call(v,"android/widget/FrameLayout$LayoutParams","<init>","(II)V",params.l,wh,2,out)||dvm_exception(v))return true;}
  union dvm_value mount[2]={child,params};if(!view_state_runtime_call(v,"android/view/ViewGroup","addView","(Landroid/view/View;Landroid/view/ViewGroup$LayoutParams;)V",decor,mount,2,out)){dvm__throw(v,"java/lang/UnsupportedOperationException","guest decor addView unavailable");return true;}
  if(dvm_exception(v))return true;
  if(!view_state_runtime_set_attached(v,decor,true)){dvm__throw(v,"java/lang/IllegalStateException","guest decor attach failed");return true;}g->root=a[0].l;
  engine_probe_log("ACTIVITY_GUEST_CONTENT activity=%s root=%s root_ref=%u descendants=%d attached=1 rendered=0",dvm__obj(v,self)->cls->name,dvm__obj(v,g->root)->cls->name,g->root,count-1);
  for(int i=0;i<count;i++)engine_probe_log("TASK5_CONTENT_NODE index=%d ref=%u class=%s",i,views[i],dvm__obj(v,views[i])->cls->name);
  return true;
 }
 if(!strcmp(m,"findViewById")&&!strcmp(sig,"(I)Landroid/view/View;")&&n==1){
  dvm_ref views[1024];int count=g->root?view_state_runtime_snapshot_tree(v,g->root,views,1024):0;out->l=0;
  for(int i=0;i<count;i++){union dvm_value id={0};view_state_runtime_call(v,"android/view/View","getId","()I",views[i],NULL,0,&id);if(id.i==a[0].i){out->l=views[i];break;}}
  if(!out->l&&g->root&&(guest_minimal_layout||captcha_arkts)&&shadow_allowed(g->layout)){
   for(unsigned i=0;i<sizeof shadow_view_decls/sizeof *shadow_view_decls;i++){
    const struct shadow_view_decl *d=&shadow_view_decls[i];if(d->layout!=g->layout||d->id!=(uint32_t)a[0].i)continue;
    struct dvm_class *cls=dvm_find_class_or_external(v,d->klass);dvm_ref child=cls?dvm_new_object(v,cls):0;
    union dvm_value context={.l=self},q={0};if(!child)return true;
    if(cls->external){if(!view_state_runtime_call(v,d->klass,"<init>","(Landroid/content/Context;)V",child,&context,1,&q))return false;}
    else {struct dvm_method *ctor=dvm_find_method(v,cls,"<init>","(Landroid/content/Context;)V");if(!ctor||!dvm_call(v,ctor,child,&context,1,&q))return true;}
    if(dvm_exception(v))return true;
    union dvm_value id={.i=a[0].i},value={.l=child};view_state_runtime_call(v,"android/view/View","setId","(I)V",child,&id,1,&q);
    view_state_runtime_call(v,"android/view/ViewGroup","addView","(Landroid/view/View;)V",g->root,&value,1,&q);
    engine_probe_log("SHADOW_VIEW_DECL layout=%u id=%u class=%s ref=%u xml_geometry=0",g->layout,d->id,d->klass,child);out->l=child;break;
   }
  }return true;
 }
 if(!strcmp(m,"setResult")&&((!strcmp(sig,"(I)V")&&n==1)||(!strcmp(sig,"(ILandroid/content/Intent;)V")&&n==2))){g->code=a[0].i;g->result=n==2?a[1].l:0;return true;}
 if(!strcmp(m,"isFinishing")&&!strcmp(sig,"()Z")){out->i=g->phase==4;return true;}
 if(!strcmp(m,"isDestroyed")&&!strcmp(sig,"()Z")){out->i=g->phase==5;return true;}
 if(!strcmp(m,"finish")&&!strcmp(sig,"()V")){
  if(g->delivered)return true;guest_remember(v,g);g->phase=4;g->delivered=1;
  if(g->caller&&g->request>=0){union dvm_value result[3]={{.i=g->request},{.i=g->code},{.l=g->result}};
   bool ok=guest_invoke(v,g->caller,"onActivityResult","(IILandroid/content/Intent;)V",result,3);
   engine_probe_log("ACTIVITY_GUEST_RESULT child=%u caller=%u request=%d result=%d data=%u ok=%d",self,g->caller,g->request,g->code,g->result,ok);
  }return true;
 }
 if(!strcmp(c,"android/app/Activity")&&!strcmp(m,"onActivityResult")&&!strcmp(sig,"(IILandroid/content/Intent;)V"))return true;
 return false;
}
/* Secondary Web Activity teardown. Never finishes the primary Unity Activity.
 * Normal close first executes the guest's registered listener; force is only
 * the escape path for a blocked/failed page. */
bool activity_runtime_web_finished(struct dvm *v,dvm_ref context){struct guest_activity *g=state.vm==v?guest_find(context):NULL;return g&&g->phase>=4;}
bool activity_runtime_web_finish(struct dvm *v,dvm_ref context,int force){
 struct guest_activity *g=state.vm==v?guest_find(context):NULL;if(!g)return false;if(g->phase==5)return true;
 union dvm_value q={0};
 if(g->phase<4){if(!force)return false;if(!guest_call(v,"android/app/Activity","finish","()V",context,NULL,0,&q)||dvm_exception(v))return false;}
 const char *callbacks[]={"onPause","onStop","onDestroy"};
 for(unsigned i=0;i<3;i++){struct dvm_method *m=dvm_find_method(v,dvm_object_class(v,context),callbacks[i],"()V");if(m&&!dvm_call(v,m,context,NULL,0,&q))return false;if(dvm_exception(v))return false;}
 /* onDestroy may already have removed CommonWebView from the content root.
  * Release this Activity's complete declaration ownership, including those
  * detached subtrees and replaced roots, rather than only the remaining tree. */
 for(unsigned i=0;i<128;i++){struct shadow_node_state *node=&shadow_nodes[i];if(node->ref&&node->context==context){view_state_runtime_set_attached(v,node->ref,false);if(node->click&&activity_identity(v,node->click)==node->click_identity)dvm_unpin(v,node->click);if(node->touch&&activity_identity(v,node->touch)==node->touch_identity)dvm_unpin(v,node->touch);memset(node,0,sizeof *node);}}
 guest_remember(v,g);g->phase=5;engine_probe_log("WEBVIEW_ACTIVITY_CLOSED ref=%u callbacks=onPause,onStop,onDestroy force=%d",context,force);return true;
}
/* Explicit diagnostic selection is an argument, never an application-class
 * rule in lifecycle dispatch. Does not click, populate inputs or submit. */
bool activity_runtime_task4_component_probe(struct dvm *v,dvm_ref caller,const char *component){
 struct dvm_class *target=dvm_find_class(v,component);union dvm_value out={0};
 if(!target){engine_probe_log("TASK4_COMPONENT_PROBE component=%s found=0",component);return false;}
 dvm_ref intent=dvm_new_object(v,dvm_find_class_or_external(v,"android/content/Intent"));
 union dvm_value ctor[2]={{.l=caller},{.l=dvm_class_object(v,target)}};
 if(!intent_runtime_call(v,"android/content/Intent","<init>","(Landroid/content/Context;Ljava/lang/Class;)V",intent,ctor,2,&out)||dvm_exception(v))return false;
 union dvm_value args[2]={{.l=intent},{.i=4042}};
 bool handled=intent_runtime_call(v,"android/app/Activity","startActivityForResult","(Landroid/content/Intent;I)V",caller,args,2,&out);
 struct guest_activity *g=NULL;for(unsigned i=0;i<32;i++)if(guests[i].intent==intent)g=&guests[i];
 char error[1024]={0};if(dvm_exception(v))dvm_describe_exception(v,dvm_exception(v),error,sizeof error);
 engine_probe_log("TASK4_COMPONENT_PROBE component=%s handled=%d phase=%d root=%u exception=%s inputs_set=0 submit=0",component,handled,g?g->phase:-1,g?g->root:0,error);
 if(guest_minimal_layout&&g){
  dvm_ref error_ref=dvm_exception(v);bool parked=v->parked,guard_ran=false,bind_ran=false,bind_ok=false;
  union dvm_value presenter={0},view={0},guard={0},attached={0};
  /* Minimal layouts deliberately have no SDK widgets. If initView fails after
   * mounting, independently exercise the original binding entry. Preserve the
   * lifecycle exception; this diagnostic never claims onCreate completed. */
  if(g->root){
   dvm_clear_exception(v);
   dvm_get_field(v,g->ref,"mPresenter","Lcom/gsc/base/mvp/a;",&presenter);
   if(!presenter.l&&!dvm_exception(v)){
    struct dvm_method *bind=dvm_find_method(v,dvm__obj(v,g->ref)->cls,"setPresenter","()V");
    if(bind){union dvm_value ignored={0};bind_ran=true;bind_ok=dvm_call(v,bind,g->ref,NULL,0,&ignored)&&!dvm_exception(v);}
    if(!dvm_exception(v))dvm_get_field(v,g->ref,"mPresenter","Lcom/gsc/base/mvp/a;",&presenter);
   }
   if(presenter.l&&!dvm_exception(v)){
    struct dvm_class *pc=dvm__obj(v,presenter.l)->cls;
    struct dvm_method *get=dvm_find_method(v,pc,"getView","()Lcom/gsc/base/mvp/c;");
    struct dvm_method *check=dvm_find_method(v,pc,"isNotViewAttached","()Z");
    if(get)dvm_call(v,get,presenter.l,NULL,0,&view);
    if(!dvm_exception(v)&&check)guard_ran=dvm_call(v,check,presenter.l,NULL,0,&guard)&&!dvm_exception(v);
   }
   if(!dvm_exception(v))view_state_runtime_call(v,"android/view/View","isAttachedToWindow","()Z",g->root,NULL,0,&attached);
  }
  engine_probe_log("TASK5_PRESENTER_EVIDENCE activity=%s lifecycle_completed=%d root=%u isAttachedToWindow=%s presenter=%u getView=%u view_is_activity=%d isNotViewAttached=%s bind_probe_ran=%d bind_probe_ok=%d lifecycle_exception=%u probe_exception=%u login_called=0",component,!error_ref&&g->phase==3,g->root,attached.i?"true":"false",presenter.l,view.l,view.l&&view.l==g->ref,guard_ran?(guard.i?"true":"false"):"NOT_RUN",bind_ran,bind_ok,error_ref,dvm_exception(v));
  if(error_ref){v->exception=error_ref;v->parked=parked;}
 }

 return handled&&!dvm_exception(v)&&g&&g->phase==3&&g->root;
}
static int guest_test_callbacks,guest_test_request,guest_test_code;
static unsigned guest_test_lifecycle;
static dvm_ref guest_test_data;
static bool guest_test_ctor(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 guest_test_lifecycle=guest_test_lifecycle*10+1;
 return guest_call(v,"android/app/Activity","<init>","()V",self,a,n,out);
}
static bool guest_test_create(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 guest_test_lifecycle=guest_test_lifecycle*10+2;
 if(!guest_call(v,"android/app/Activity","onCreate","(Landroid/os/Bundle;)V",self,a,n,out))return false;
 dvm_ref root=dvm_new_object(v,dvm_find_class_or_external(v,"android/widget/FrameLayout"));
 dvm_ref child=dvm_new_object(v,dvm_find_class_or_external(v,"android/view/View"));union dvm_value context={.l=self},view={.l=child};
 if(!view_state_runtime_call(v,"android/widget/FrameLayout","<init>","(Landroid/content/Context;)V",root,&context,1,out)||dvm_exception(v))return false;
 if(!view_state_runtime_call(v,"android/view/View","<init>","(Landroid/content/Context;)V",child,&context,1,out)||dvm_exception(v))return false;
 if(!view_state_runtime_call(v,"android/view/ViewGroup","addView","(Landroid/view/View;)V",root,&view,1,out)||dvm_exception(v))return false;
 view.l=root;return guest_call(v,"android/app/Activity","setContentView","(Landroid/view/View;)V",self,&view,1,out)&&!dvm_exception(v);
}
static bool guest_test_start(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 guest_test_lifecycle=guest_test_lifecycle*10+3;return guest_call(v,"android/app/Activity","onStart","()V",self,a,n,out);
}
static bool guest_test_resume(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 guest_test_lifecycle=guest_test_lifecycle*10+4;return guest_call(v,"android/app/Activity","onResume","()V",self,a,n,out);
}
static bool guest_test_result(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 (void)v;(void)self;(void)out;if(n!=3)return false;guest_test_callbacks++;guest_test_request=a[0].i;guest_test_code=a[1].i;guest_test_data=a[2].l;return true;
}
static void guest_test_method(struct dvm_class *k,int i,const char *name,const char *sig,dvm_builtin_fn fn){
 struct dvm_method *m=&k->methods[i];m->cls=k;m->name=name;m->sig=strdup(sig);m->access=1;m->ret_kind='V';m->builtin=fn;
}
bool activity_runtime_task4_selftest(struct dvm *v){
 struct dvm_class *base=dvm_find_class_or_external(v,"android/app/Activity");
 struct dvm_class *child=dvm__register_builtin(v,"Lsdktrace/tests/ResultActivity;");
 struct dvm_class *parent=dvm__register_builtin(v,"Lsdktrace/tests/ResultCaller;");
 if(!base||!child||!parent)return false;
 child->super=parent->super=base;child->methods=calloc(4,sizeof *child->methods);parent->methods=calloc(1,sizeof *parent->methods);
 if(!child->methods||!parent->methods)return false;
 child->nmethods=4;parent->nmethods=1;child->init_state=parent->init_state=2;
 guest_test_method(child,0,"<init>","()V",guest_test_ctor);guest_test_method(child,1,"onCreate","(Landroid/os/Bundle;)V",guest_test_create);
 guest_test_method(child,2,"onStart","()V",guest_test_start);guest_test_method(child,3,"onResume","()V",guest_test_resume);
 guest_test_method(parent,0,"onActivityResult","(IILandroid/content/Intent;)V",guest_test_result);
 dvm_ref caller=dvm_new_object(v,parent);union dvm_value out={0};unsigned checks=0;int depth=v->depth;v->depth++;
#define GUEST_CHECK(x) do{if(!(x)){sdk_trace_fprintf(stderr,"TASK4_ACTIVITY_SELFTEST FAIL line=%d checks=%u exception=%u\n",__LINE__,checks,dvm_exception(v));v->depth=depth;return false;}checks++;}while(0)
 {
  unsigned before=checks;
  struct dvm_class *mi=dvm_find_class_or_external(v,"android/app/ActivityManager$MemoryInfo");
  struct dvm_method *ctor=mi?dvm_find_method(v,mi,"<init>","()V"):NULL;
  dvm_ref info=mi?dvm_new_object(v,mi):0;
  GUEST_CHECK(info&&ctor&&(ctor->access&1)&&dvm_call(v,ctor,info,NULL,0,&out)&&!dvm_exception(v));
  const char *names[]={"totalMem","availMem","threshold","lowMemory"};
  for(int i=0;i<4;i++){
   const char *type=i==3?"Z":"J";union dvm_value value={0};
   GUEST_CHECK(dvm_get_field(v,info,names[i],type,&value)&&!dvm_exception(v)&&(i==3?value.i==0:value.j==0));
   bool public_field=false;for(unsigned f=0;f<mi->nifields;f++)if(!strcmp(mi->ifields[f].name,names[i])&&!strcmp(mi->ifields[f].type,type))public_field=(mi->ifields[f].access&1)!=0;
   GUEST_CHECK(public_field);
  }
  /* Writable public Java fields, independently stored per instance. */
  dvm_ref other=dvm_new_object(v,mi);union dvm_value written={.j=1234567890123LL},read={0};
  GUEST_CHECK(other&&dvm_set_field(v,info,"totalMem","J",written)&&dvm_get_field(v,info,"totalMem","J",&read)&&read.j==written.j);
  GUEST_CHECK(dvm_get_field(v,other,"totalMem","J",&read)&&read.j==0);
  union dvm_value arg={.l=info};
  /* Enumerate the registered declarations before any dynamic method lookup,
   * exactly the reflection surface Unity uses before JNI fallback. */
  struct dvm_class *manager=dvm_find_class_or_external(v,"android/app/ActivityManager");
  dvm_ref manager_class=dvm_class_object(v,manager);
  GUEST_CHECK(manager&&manager->nmethods>=1&&manager_class);
  GUEST_CHECK(reflection_runtime_call(v,"java/lang/Class","getDeclaredMethods","()[Ljava/lang/reflect/Method;",manager_class,NULL,0,&out)&&!dvm_exception(v)&&out.l&&dvm_array_length(v,out.l)==(uint32_t)manager->nmethods);
  dvm_ref *declared=dvm_array_data(v,out.l);struct dvm_method *memory_method=NULL;
  GUEST_CHECK(declared);
  for(int i=0;i<manager->nmethods;i++){
   struct dvm_method *candidate=NULL;
   GUEST_CHECK(reflection_runtime_method_handle(v,declared[i],&candidate)&&candidate&&candidate->cls==manager);
   if(!strcmp(candidate->name,"getMemoryInfo")&&!strcmp(candidate->sig,"(Landroid/app/ActivityManager$MemoryInfo;)V"))memory_method=candidate;
  }
  GUEST_CHECK(memory_method&&(memory_method->access&1)&&!(memory_method->access&8));
  dvm_ref manager_object=dvm_new_object(v,manager);
  GUEST_CHECK(manager_object&&dvm_call(v,memory_method,manager_object,&arg,1,&out)&&!dvm_exception(v));
  GUEST_CHECK(!dvm_find_method(v,manager,"getMemoryInfo","(Landroid.app.ActivityManager$MemoryInfo;)V"));
  sdk_trace_fprintf(stderr,"TASK7A_MEMORYINFO_REFLECTION PASS declarations=1 public_instance=1 reflected_dispatch=1 strict_jni_signature=1\n");
  GUEST_CHECK(activity_runtime_call(v,"android/app/ActivityManager","getMemoryInfo","(Landroid/app/ActivityManager$MemoryInfo;)V",0,&arg,1,&out)&&!dvm_exception(v));
  union dvm_value total={0},avail={0};
  GUEST_CHECK(dvm_get_field(v,info,"totalMem","J",&total)&&dvm_get_field(v,info,"availMem","J",&avail)&&total.j>=0&&avail.j>=0&&avail.j<=total.j);
  GUEST_CHECK(dvm_get_field(v,info,"threshold","J",&read)&&read.j==0);
  GUEST_CHECK(dvm_get_field(v,info,"lowMemory","Z",&read)&&read.i==0&&!dvm_exception(v));
  sdk_trace_fprintf(stderr,"TASK5_MEMORYINFO_SELFTEST PASS checks=%u ctor_dispatch=1 zero_fields=4 public_fields=4 writable=1 isolated=1 totalMem=%lld availMem=%lld threshold=0 lowMemory=0\n",checks-before,(long long)total.j,(long long)avail.j);
 }
 for(int variant=0;variant<4;variant++){
  dvm_ref intent=dvm_new_object(v,dvm_find_class_or_external(v,"android/content/Intent"));
  union dvm_value component[2]={{.l=caller},{.l=dvm_class_object(v,child)}};
  GUEST_CHECK(intent_runtime_call(v,"android/content/Intent","<init>","(Landroid/content/Context;Ljava/lang/Class;)V",intent,component,2,&out)&&!dvm_exception(v));
  union dvm_value action={.l=dvm_new_string(v,"sdktrace.LAUNCHER_FIXTURE")};
  union dvm_value extra[2]={{.l=dvm_new_string(v,"launcher_marker")},{.l=dvm_new_string(v,"fixture-only")}};
  GUEST_CHECK(intent_runtime_call(v,"android/content/Intent","setAction","(Ljava/lang/String;)Landroid/content/Intent;",intent,&action,1,&out)&&!dvm_exception(v));
  GUEST_CHECK(intent_runtime_call(v,"android/content/Intent","putExtra","(Ljava/lang/String;Ljava/lang/String;)Landroid/content/Intent;",intent,extra,2,&out)&&!dvm_exception(v));
  union dvm_value args[3]={{.l=intent},{.i=40+variant},{0}};
  child->external=false;if(variant>=2)args[1].l=0;
  const char *owner=variant<2?"android/app/Activity":"android/content/Context",*method=variant<2?"startActivityForResult":"startActivity";
  const char *signature=variant<2?(variant?"(Landroid/content/Intent;ILandroid/os/Bundle;)V":"(Landroid/content/Intent;I)V"):(variant==3?"(Landroid/content/Intent;Landroid/os/Bundle;)V":"(Landroid/content/Intent;)V");
  struct dvm_method *entry=dvm_find_method(v,dvm_find_class_or_external(v,owner),method,signature);
  guest_test_lifecycle=0;
  bool handled=entry&&dvm_call(v,entry,caller,args,variant<2?(variant?3:2):(variant==3?2:1),&out);
  child->external=true;GUEST_CHECK(handled&&!dvm_exception(v));
  struct guest_activity *g=NULL;for(unsigned i=0;i<32;i++)if(guests[i].intent==intent)g=&guests[i];
  GUEST_CHECK(g&&g->phase==3&&g->root&&g->caller==caller);
  struct dvm_method *retained=dvm_find_method(v,base,"getLastNonConfigurationInstance","()Ljava/lang/Object;");
  GUEST_CHECK(retained&&dvm_call(v,retained,g->ref,NULL,0,&out)&&!dvm_exception(v)&&out.l==0);
  g->last_nonconfiguration=intent;
  GUEST_CHECK(dvm_call(v,retained,g->ref,NULL,0,&out)&&!dvm_exception(v)&&out.l==intent);
  g->last_nonconfiguration=0;
  GUEST_CHECK(guest_test_lifecycle==1234);
  GUEST_CHECK(guest_call(v,"android/app/Activity","getIntent","()Landroid/content/Intent;",g->ref,NULL,0,&out)&&out.l==intent);
  GUEST_CHECK(intent_runtime_call(v,"android/content/Intent","getAction","()Ljava/lang/String;",out.l,NULL,0,&out)&&out.l&&!strcmp(dvm_string_utf8(v,out.l),"sdktrace.LAUNCHER_FIXTURE"));
  GUEST_CHECK(intent_runtime_call(v,"android/content/Intent","getStringExtra","(Ljava/lang/String;)Ljava/lang/String;",intent,extra,1,&out)&&out.l&&!strcmp(dvm_string_utf8(v,out.l),"fixture-only"));
  dvm_ref tree[8];GUEST_CHECK(view_state_runtime_snapshot_tree(v,g->root,tree,8)==2);
  union dvm_value query={0},id={.i=505};
  GUEST_CHECK(view_state_runtime_call(v,"android/view/View","isAttachedToWindow","()Z",g->root,NULL,0,&query)&&query.i==1);
  GUEST_CHECK(view_state_runtime_call(v,"android/view/View","isAttachedToWindow","()Z",tree[1],NULL,0,&query)&&query.i==1);
  GUEST_CHECK(view_state_runtime_call(v,"android/view/View","setId","(I)V",tree[1],&id,1,&query));
  GUEST_CHECK(view_state_runtime_call(v,"android/view/View","findViewById","(I)Landroid/view/View;",g->root,&id,1,&query)&&query.l==tree[1]);
  GUEST_CHECK(guest_call(v,"android/app/Activity","getWindow","()Landroid/view/Window;",g->ref,NULL,0,&query)&&query.l);
  dvm_ref window=query.l;
  GUEST_CHECK(guest_call(v,"android/view/Window","getDecorView","()Landroid/view/View;",window,NULL,0,&query)&&query.l);
  dvm_ref decor=query.l;
  GUEST_CHECK(guest_call(v,"android/view/Window","getDecorView","()Landroid/view/View;",window,NULL,0,&query)&&query.l==decor);
  GUEST_CHECK(guest_call(v,"android/view/Window","getAttributes","()Landroid/view/WindowManager$LayoutParams;",window,NULL,0,&query)&&query.l);
  union dvm_value params={.l=query.l};
  GUEST_CHECK(guest_call(v,"android/view/Window","setAttributes","(Landroid/view/WindowManager$LayoutParams;)V",window,&params,1,&query));
  GUEST_CHECK(guest_call(v,"android/view/Window","getAttributes","()Landroid/view/WindowManager$LayoutParams;",window,NULL,0,&query)&&query.l==params.l);
  GUEST_CHECK(guest_call(v,"android/view/Window","findViewById","(I)Landroid/view/View;",window,&id,1,&query)&&query.l==tree[1]);
  GUEST_CHECK(view_state_runtime_call(v,"android/view/View","getWidth","()I",g->root,NULL,0,&query)&&query.i==1);
  GUEST_CHECK(view_state_runtime_call(v,"android/view/View","getHeight","()I",g->root,NULL,0,&query)&&query.i==1);
  GUEST_CHECK(view_state_runtime_call(v,"android/view/View","getVisibility","()I",g->root,NULL,0,&query)&&query.i==0);
  union dvm_value feature={.i=7};
  GUEST_CHECK(guest_call(v,"android/app/Activity","requestWindowFeature","(I)Z",g->ref,&feature,1,&query)&&query.i&&(g->features&(1u<<7)));
  /* Route Window.setContentView through the same mount; replaces existing root. */
  union dvm_value mounted={.l=g->root};
  GUEST_CHECK(guest_call(v,"android/view/Window","setContentView","(Landroid/view/View;)V",window,&mounted,1,&query)&&!dvm_exception(v)&&g->root==mounted.l);
  union dvm_value anim[2]={{.i=17+variant},{.i=29+variant}};
  struct dvm_method *transition=dvm_find_method(v,base,"overridePendingTransition","(II)V");
  GUEST_CHECK(transition&&dvm_call(v,transition,g->ref,anim,2,&query)&&!dvm_exception(v));
  bool animation_saved=false;for(unsigned t=0;t<64;t++)if(transitions[t].activity==g->ref&&activity_identity(v,g->ref)==transitions[t].identity)animation_saved=transitions[t].enter==anim[0].i&&transitions[t].exit==anim[1].i;
  GUEST_CHECK(animation_saved);
  union dvm_value result[2]={{.i=-1},{.l=intent}};
  GUEST_CHECK(guest_call(v,"android/app/Activity","setResult","(ILandroid/content/Intent;)V",g->ref,result,2,&out));
  GUEST_CHECK(guest_call(v,"android/app/Activity","finish","()V",g->ref,NULL,0,&out)&&!dvm_exception(v));
  GUEST_CHECK(guest_test_callbacks==(variant<2?variant+1:2)&&(variant>=2||(guest_test_request==40+variant&&guest_test_code==-1&&guest_test_data==intent)));
  GUEST_CHECK(guest_call(v,"android/app/Activity","finish","()V",g->ref,NULL,0,&out)&&guest_test_callbacks==(variant<2?variant+1:2));
  sdk_trace_fprintf(stderr,"TASK5_LAUNCHER_CASE PASS variant=%d method=%s.%s%s dvm_entry=1 component=sdktrace/tests/ResultActivity action=sdktrace.LAUNCHER_FIXTURE extra=fixture-only lifecycle=1234 request=%d result=-1 result_data=%u callbacks=%d window_mount=1 decor_stable=1 transition_enter=%d transition_exit=%d rendered=0 fixture_only=1\n",variant,owner,method,signature,g->request,g->result,guest_test_callbacks,anim[0].i,anim[1].i);
 }
 {
  dvm_ref web=dvm_new_object(v,dvm_find_class_or_external(v,"android/webkit/WebView"));union dvm_value context={.l=caller},q={0};
  GUEST_CHECK(android_basics_call(v,"android/webkit/WebView","<init>","(Landroid/content/Context;)V",web,&context,1,&q)&&!dvm_exception(v));
  GUEST_CHECK(android_basics_call(v,"android/webkit/WebView","getContext","()Landroid/content/Context;",web,NULL,0,&q)&&q.l==caller);
  union dvm_value url={.l=dvm_new_string(v,"about:blank#task5-fixture")};
  GUEST_CHECK(android_basics_call(v,"android/webkit/WebView","loadUrl","(Ljava/lang/String;)V",web,&url,1,&q)&&!dvm_exception(v));
  GUEST_CHECK(android_basics_call(v,"android/webkit/WebView","getUrl","()Ljava/lang/String;",web,NULL,0,&q)&&q.l==url.l);
  GUEST_CHECK(android_basics_call(v,"android/webkit/WebView","destroy","()V",web,NULL,0,&q));
  GUEST_CHECK(android_basics_call(v,"android/webkit/WebView","destroy","()V",web,NULL,0,&q));
  GUEST_CHECK(android_basics_call(v,"android/webkit/WebView","getUrl","()Ljava/lang/String;",web,NULL,0,&q)&&!q.l);
  GUEST_CHECK(android_basics_call(v,"android/webkit/WebView","loadUrl","(Ljava/lang/String;)V",web,&url,1,&q)&&dvm_exception(v));dvm_clear_exception(v);
  dvm_ref fragment=dvm_new_object(v,dvm_find_class_or_external(v,"android/app/DialogFragment"));
  GUEST_CHECK(android_basics_call(v,"android/app/DialogFragment","<init>","()V",fragment,NULL,0,&q)&&!dvm_exception(v));
  GUEST_CHECK(android_basics_call(v,"android/app/DialogFragment","getShowsDialog","()Z",fragment,NULL,0,&q)&&q.i);
  union dvm_value flag={.i=0};
  GUEST_CHECK(android_basics_call(v,"android/app/DialogFragment","setShowsDialog","(Z)V",fragment,&flag,1,&q));
  GUEST_CHECK(android_basics_call(v,"android/app/DialogFragment","getShowsDialog","()Z",fragment,NULL,0,&q)&&!q.i);
  sdk_trace_fprintf(stderr,"TASK5_COMPONENT_SELFTEST PASS web_context=1 web_url=1 fragment_default=1 fragment_state=1 fixture_url=about:blank#task5-fixture\n");
  dvm_ref tok=dvm_new_object(v,dvm_find_class_or_external(v,"org/json/JSONTokener"));
  union dvm_value input={.l=dvm_new_string(v," /*c*/  {\"a\":1}  true 42 \"x\\n\" [1,2] null !")};
  GUEST_CHECK(json_runtime_call(v,"org/json/JSONTokener","<init>","(Ljava/lang/String;)V",tok,&input,1,&q)&&!dvm_exception(v));
  GUEST_CHECK(json_runtime_call(v,"org/json/JSONTokener","nextValue","()Ljava/lang/Object;",tok,NULL,0,&q)&&q.l&&!dvm_exception(v)&&!strcmp(dvm__obj(v,q.l)->cls->name,"org/json/JSONObject"));
  GUEST_CHECK(json_runtime_call(v,"org/json/JSONTokener","nextValue","()Ljava/lang/Object;",tok,NULL,0,&q)&&q.l&&!strcmp(dvm__obj(v,q.l)->cls->name,"java/lang/Boolean"));
  GUEST_CHECK(json_runtime_call(v,"org/json/JSONTokener","nextValue","()Ljava/lang/Object;",tok,NULL,0,&q)&&q.l&&!strcmp(dvm__obj(v,q.l)->cls->name,"java/lang/Integer"));
  GUEST_CHECK(json_runtime_call(v,"org/json/JSONTokener","nextValue","()Ljava/lang/Object;",tok,NULL,0,&q)&&q.l&&!strcmp(dvm_string_utf8(v,q.l),"x\n"));
  GUEST_CHECK(json_runtime_call(v,"org/json/JSONTokener","nextValue","()Ljava/lang/Object;",tok,NULL,0,&q)&&q.l&&!strcmp(dvm__obj(v,q.l)->cls->name,"org/json/JSONArray"));
  GUEST_CHECK(json_runtime_call(v,"org/json/JSONTokener","nextValue","()Ljava/lang/Object;",tok,NULL,0,&q)&&q.l&&!dvm_exception(v));
  GUEST_CHECK(json_runtime_call(v,"org/json/JSONTokener","nextClean","()C",tok,NULL,0,&q)&&q.i=='!');
  GUEST_CHECK(json_runtime_call(v,"org/json/JSONTokener","nextClean","()C",tok,NULL,0,&q)&&q.i==0);
  sdk_trace_fprintf(stderr,"TASK5_TOKENER_SELFTEST PASS source_cursor=1 comments=1 object=1 array=1 string_escape=1 boolean=1 integer=1 null=1 eof=1\n");
 }
 v->depth=depth;sdk_trace_fprintf(stderr,"TASK4_ACTIVITY_SELFTEST PASS checks=%u fixture_only=1 overloads=4 lifecycle=1 view_tree=1 result_roundtrip=1 idempotent=1 login_tree_not_claimed=1\n",checks);return true;
#undef GUEST_CHECK
}
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
  if(state.finished[i]==self&&activity_identity(v,self)==state.finished_generation[i])return 1;
 return 0;
}
/* Returns whether the receiver was already finished before this call. */
static int activity_mark_finished_locked(struct dvm *v,dvm_ref self){
 int already=activity_finished_locked(v,self);
 if(self==state.activity){state.lifecycle=ACTIVITY_LIFECYCLE_FINISHED;return already;}
 if(already)return 1;
 if(state.nfinished<ACTIVITY_FINISHED_CAP){int i=state.nfinished++;state.finished[i]=self;state.finished_generation[i]=activity_identity(v,self);}
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
 {"isDestroyed","()Z",0x1},
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
 {"startActivityForResult","(Landroid/content/Intent;I)V",0x1},
 {"startActivityForResult","(Landroid/content/Intent;ILandroid/os/Bundle;)V",0x1},
 {"setResult","(I)V",0x1},
 {"setResult","(ILandroid/content/Intent;)V",0x1},
 {"onNewIntent","(Landroid/content/Intent;)V",0x4},
 {"onSaveInstanceState","(Landroid/os/Bundle;)V",0x4},
 {"getIntent","()Landroid/content/Intent;",0x1},
 {"getLastNonConfigurationInstance","()Ljava/lang/Object;",0x1},
 {"setIntent","(Landroid/content/Intent;)V",0x1},
 {"getCallingPackage","()Ljava/lang/String;",0x1},
 {"getWindow","()Landroid/view/Window;",0x1},
 {"getWindowManager","()Landroid/view/WindowManager;",0x1},
 {"requestWindowFeature","(I)Z",0x1},
 {"overridePendingTransition","(II)V",0x1},
 {"startActivityForResult","(Landroid/content/Intent;I)V",0x1},
 {"startActivityForResult","(Landroid/content/Intent;ILandroid/os/Bundle;)V",0x1},
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
 {"bindService","(Landroid/content/Intent;Landroid/content/ServiceConnection;I)Z",0x1},
 {"unbindService","(Landroid/content/ServiceConnection;)V",0x1},
 {"startActivity","(Landroid/content/Intent;)V",0x1},
 {"startActivity","(Landroid/content/Intent;Landroid/os/Bundle;)V",0x1},
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
static const struct host_method_desc activity_manager_method_table[]={
 {"getMemoryInfo","(Landroid/app/ActivityManager$MemoryInfo;)V",0x1}
};
static const struct host_method_desc intent_method_table[]={
 {"<init>","()V",0x10001},
 {"<init>","(Ljava/lang/String;)V",0x10001},
 {"<init>","(Ljava/lang/String;Landroid/net/Uri;)V",0x10001},
 {"<init>","(Landroid/content/Context;Ljava/lang/Class;)V",0x10001},
 {"setClass","(Landroid/content/Context;Ljava/lang/Class;)Landroid/content/Intent;",0x1},
 {"setClassName","(Landroid/content/Context;Ljava/lang/String;)Landroid/content/Intent;",0x1},
 {"setClassName","(Ljava/lang/String;Ljava/lang/String;)Landroid/content/Intent;",0x1},
 {"getAction","()Ljava/lang/String;",0x1},
 {"setAction","(Ljava/lang/String;)Landroid/content/Intent;",0x1},
 {"addCategory","(Ljava/lang/String;)Landroid/content/Intent;",0x1},
 {"hasCategory","(Ljava/lang/String;)Z",0x1},
 {"getCategories","()Ljava/util/Set;",0x1},
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
 const char *controls[]={"resume-service","resume-guard","freeze180-observe","play-stall-duration-ms"};
 for(unsigned i=0;i<sizeof controls/sizeof controls[0];i++){char p[256];snprintf(p,sizeof p,"/data/storage/el2/base/haps/entry/files/sdkdex/%s",controls[i]);FILE *f=fopen(p,"wx");if(f){fputs("0\n",f);fclose(f);chmod(p,0666);}}
 memset(shadow_nodes,0,sizeof shadow_nodes);memset(shadow_inflaters,0,sizeof shadow_inflaters);shadow_depth=0;
 memset(guests,0,sizeof guests);memset(transitions,0,sizeof transitions);
 guest_minimal_layout=false;
 memset(&state,0,sizeof state);state.vm=v;
 if(!activity_gc_ensure(v))return;
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
 {struct dvm_class *manager=dvm_find_class_or_external(v,"android/app/ActivityManager");
  install_method_table(manager,activity_manager_method_table,
   sizeof activity_manager_method_table/sizeof activity_manager_method_table[0]);}
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
 if(state.vm==v&&!activity_gc_ensure(v))return true;
 if((!strcmp(m,"bindService")||!strcmp(m,"unbindService"))&&activity_family_receiver(v,self)&&activity_owner_is_family(v,self,c)&&resume_service_call(v,m,sig,self,a,n,out,activity_runtime_resume_flag("resume-service")))return true;
 if(!strcmp(m,"getLastNonConfigurationInstance")&&!strcmp(sig,"()Ljava/lang/Object;")&&n==0){
  memset(out,0,sizeof *out);
  if(!self){dvm__throw(v,"java/lang/NullPointerException","getLastNonConfigurationInstance receiver");return true;}
  if(state.vm!=v||!activity_family_receiver(v,self)||!activity_owner_is_family(v,self,c))return false;
  struct guest_activity *g=guest_find(self);
  out->l=g?g->last_nonconfiguration:(self==state.activity?state.last_nonconfiguration:0);
  return true;
 }
 if(shadow_layout_call(v,c,m,sig,self,a,n,out))return true;
 if(guest_call(v,c,m,sig,self,a,n,out))return true;
 if(!strcmp(c,"android/app/Activity")&&!strcmp(m,"onActivityResult")&&!strcmp(sig,"(IILandroid/content/Intent;)V"))return true;
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
 if(!strcmp(m,"isDestroyed")&&!strcmp(sig,"()Z")&&self==state.activity){out->i=state.destroyed;return true;}
 if(!strcmp(c,"android/app/Activity")&&!strcmp(m,"onDestroy")&&!strcmp(sig,"()V")&&self==state.activity){state.destroyed=1;return true;}
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
  struct dvm_object *receiver=dvm__obj(v,self);
  bool is_activity=false,strict_activity=false;
  unsigned chain_length=0;
  for(struct dvm_class *k=receiver?receiver->cls:NULL;k;k=k->super){
   ++chain_length;
   const char *names[]={k->name,k->super?NULL:dvm_class_super_name(k)};
   for(unsigned i=0;i<2;++i){
    const char *name=names[i];if(!name)continue;
    size_t length=strlen(name);
    if(!strcmp(name,"android/app/Activity")){
     is_activity=true;if(i==0)strict_activity=true;
    }else if((!strncmp(name,"android/app/",12)&&length>=8&&!strcmp(name+length-8,"Activity"))||
             !strcmp(name,"com/unity3d/player/UnityPlayerActivity"))is_activity=true;
   }
  }
  sdk_trace_fprintf(stderr,"activity_guard pid=%d class=%s super_chain_length=%u relaxed=%d valid=%d primary_exists=%d repeat_primary=%d\n",
   (int)getpid(),receiver&&receiver->cls&&receiver->cls->name?receiver->cls->name:"<invalid>",
   chain_length,receiver&&(!strict_activity||self==state.activity),receiver!=NULL,state.activity!=0,self&&self==state.activity);
  if(!receiver){dvm__throw(v,"java/lang/IllegalStateException","Activity construction unavailable");return true;}
  /* A guest may construct an unattached Activity subclass as a helper.
   * Its base constructor does not attach it or replace the host's primary
   * Activity. In particular it must not reset the primary lifecycle. */
  if(state.activity||!is_activity)return true;
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
 /* MemoryInfo/ActivityManager are not Activity receivers. Dispatch before
  * the primary-Activity-only guard. */
 if(!strcmp(c,"android/app/ActivityManager")&&!strcmp(m,"getMemoryInfo")&&
    !strcmp(sig,"(Landroid/app/ActivityManager$MemoryInfo;)V")&&n==1){
  if(!a||!a[0].l){dvm__throw(v,"java/lang/NullPointerException","getMemoryInfo outInfo");return true;}
  struct dvm_object *mi=dvm__obj(v,a[0].l);
  if(!mi||!mi->cls||strcmp(mi->cls->name,"android/app/ActivityManager$MemoryInfo")){
   dvm__throw(v,"java/lang/IllegalArgumentException","getMemoryInfo outInfo type");return true;
  }
  /* 从 guest 可见的 /proc/meminfo 读取真实内存；threshold 无宿主值，保持 0
   * （不编造系统阈值），lowMemory 由可用内存与阈值比较。 */
  long long total=0,avail=-1;FILE *f=fopen("/proc/meminfo","r");
  if(f){char line[256];while(fgets(line,sizeof line,f)){long long kb=0;
   if(sscanf(line,"MemTotal: %lld kB",&kb)==1)total=kb*1024;
   else if(sscanf(line,"MemAvailable: %lld kB",&kb)==1)avail=kb*1024;}}
  if(f)fclose(f);
  if(total<=0||avail<0){engine_probe_log("ENGINE_LOG_MEMORY_INFO_FAILED total=%lld available=%lld source=/proc/meminfo",total,avail);dvm__throw(v,"java/lang/IllegalStateException","device /proc/meminfo missing MemTotal/MemAvailable");return true;}
  /* Harmony has no Android ActivityManager low-memory policy service. Use the
   * actual Linux zone low watermarks if exposed, never an invented percentage
   * of RAM. Unknown threshold is explicit in diagnostics. */
  long long low_pages=0;bool threshold_known=false;f=fopen("/proc/zoneinfo","r");
  if(f){char line[256];while(fgets(line,sizeof line,f)){char key[32];long long pages;if(sscanf(line," %31s %lld",key,&pages)==2&&!strcmp(key,"low")&&pages>=0){low_pages+=pages;threshold_known=true;}}fclose(f);}
  long page_size=sysconf(_SC_PAGESIZE);long long threshold=threshold_known&&page_size>0?low_pages*page_size:0;
  static unsigned memory_logs;if(memory_logs++<8)engine_probe_log("ENGINE_LOG_MEMORY_INFO totalMem=%lld availMem=%lld threshold=%lld threshold_known=%d lowMemory=%d source=/proc/meminfo threshold_source=/proc/zoneinfo:low",total,avail,threshold,threshold_known,threshold_known&&avail<threshold);
  union dvm_value val={0};
  val.j=avail;if(!dvm_set_field(v,a[0].l,"availMem","J",val))return true;
  val.j=total;if(!dvm_set_field(v,a[0].l,"totalMem","J",val))return true;
  val.j=threshold;if(!dvm_set_field(v,a[0].l,"threshold","J",val))return true;
  val.i=threshold_known&&avail<threshold;if(!dvm_set_field(v,a[0].l,"lowMemory","Z",val))return true;
  return true;
 }
 if(!strcmp(c,"android/app/ActivityManager$MemoryInfo")&&!strcmp(m,"<init>")&&!strcmp(sig,"()V")&&n==0){
  /* 对象字段由 dvm_new_object 清零；真实数值在 getMemoryInfo 中按
   * /proc/meminfo 填充（threshold 保持 0，不编造系统阈值）。 */
  return true;
 }
 /* Main-thread dispatch belongs to every real Activity receiver, including
  * guest instances; the diagnostic Activity identity is unrelated. */
 if(!strcmp(m,"runOnUiThread")&&!strcmp(sig,"(Ljava/lang/Runnable;)V")){
  if(!self){dvm__throw(v,"java/lang/NullPointerException","runOnUiThread receiver");return true;}
  if(!receiver_owner_matches(v,self,"android/app/Activity"))return false;
  if(!a){dvm__throw(v,"java/lang/NullPointerException","runOnUiThread Runnable");return true;}
  if(v->cur_thread==v->main_thread){
   struct dvm_class *rc=dvm_object_class(v,a[0].l);struct dvm_method *run=rc?dvm_find_method(v,rc,"run","()V"):NULL;
   if(!run){dvm__throw(v,"java/lang/NullPointerException","runOnUiThread Runnable");return true;}
   union dvm_value ignored={0};dvm_call(v,run,a[0].l,NULL,0,&ignored);return true;
  }
  sdk_trace_fprintf(stderr,"ACTIVITY_MAIN_POST receiver=%u thread=%u main=%u runnable=%u\n",self,v->cur_thread,v->main_thread,a[0].l);
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
  if(!a||n!=1||a[0].i<0||a[0].i>=32){dvm__throw(v,"java/lang/IllegalArgumentException","window feature id");return true;}
  if(state.lifecycle!=ACTIVITY_LIFECYCLE_NEW){dvm__throw(v,"java/lang/IllegalStateException","requestWindowFeature requires Activity before onCreate");return true;}
  state.requested_features|=1u<<a[0].i;
  int status=1;
  pthread_mutex_lock(&host_lock);activity_window_feature_host callback=feature_host;void *user=feature_host_user;pthread_mutex_unlock(&host_lock);
  if(a[0].i==1&&callback){unsigned depth=dvm_gil_held(v)?dvm_gil_unlock_all(v):0;status=callback(user,a[0].i);if(depth)dvm_gil_relock(v,depth);}
  /* Window skeleton accepts and records the request. Optional real host
   * application is reported separately; a true result is not render proof. */
  out->i=1;sdk_trace_fprintf(stderr,"TASK5_WINDOW_FEATURE activity=%u feature=%d mask=0x%x accepted=1 host_result=%d host_applied=%d\n",self,a[0].i,state.requested_features,status,status==0);return true;
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
  /* Context.getString must use the original APK resource table too. */
  extern bool platform_runtime_call(struct dvm *,const char *,const char *,const char *,dvm_ref,const union dvm_value *,int,union dvm_value *);
  return platform_runtime_call(v,"android/content/Context",m,sig,self,a,n,out);
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
 if(!activity_gc_ensure(v)){okay=false;goto done;}
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
 CHECK(activity_runtime_call(v,"android/app/Activity","requestWindowFeature","(I)Z",activity,args,1,&out)&&!dvm_exception(v)&&out.i==1&&feature_test.calls==3);
 feature_test.status=-1;memset(&out,0,sizeof out);
 CHECK(activity_runtime_call(v,"android/app/Activity","requestWindowFeature","(I)Z",activity,args,1,&out)&&!dvm_exception(v)&&out.i==1&&feature_test.calls==4);
 dvm_clear_exception(v);
 activity_runtime_set_window_feature_host(NULL,NULL);memset(&out,0,sizeof out);
 CHECK(activity_runtime_call(v,"android/app/Activity","requestWindowFeature","(I)Z",activity,args,1,&out)&&!dvm_exception(v)&&out.i==1&&(state.requested_features&2));
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
 {
  struct dvm_method *retained=dvm_find_method(v,ac,"getLastNonConfigurationInstance","()Ljava/lang/Object;");
  CHECK(retained&&dvm_call(v,retained,activity,NULL,0,&out)&&!dvm_exception(v)&&out.l==0);
  dvm_ref fixture=dvm_new_string(v,"retained-instance-fixture");state.last_nonconfiguration=fixture;
  CHECK(fixture&&retained&&dvm_call(v,retained,activity,NULL,0,&out)&&!dvm_exception(v)&&out.l==fixture);
  state.last_nonconfiguration=0;
 }
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
  dvm_ref saved_thread=v->cur_thread;
  v->cur_thread=dvm_new_object(v,dvm_find_class_or_external(v,"java/lang/Thread"));
  CHECK(v->cur_thread&&v->cur_thread!=v->main_thread);
  CHECK(activity_runtime_call(v,"android/app/Activity","runOnUiThread","(Ljava/lang/Runnable;)V",activity,args,1,&out)&&!dvm_exception(v)&&probe.runs==0);
  v->cur_thread=saved_thread;
  dvm__run_pending_threads(v);
  CHECK(!dvm_exception(v)&&probe.runs==1&&probe.wrong_thread==0&&probe.wrong_gil==0);
  v->cur_thread=v->main_thread;
  CHECK(activity_runtime_call(v,"android/app/Activity","runOnUiThread","(Ljava/lang/Runnable;)V",activity,args,1,&out)&&!dvm_exception(v)&&probe.runs==2);
  v->cur_thread=saved_thread;
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
 if(okay)sdk_trace_fprintf(stderr,"ACTIVITY_SELFTEST PASS window_feature_host_mock=1 window_feature_recorded_true=1 window_feature_host_status_separate=1 window_stable=1 window_no_host_fail_closed=1 windowmanager_stable=1 display_fail_closed=1 getIntent_unbound_null=1 intent_binding_roundtrip=1 setIntent=1 runOnUiThread_deferred=1 run_on_main=1 mainLooper=1 finish_state=1 finish_idempotent=1 finishAffinity_state=1 isFinishing_real_state=1 finish_null_npe=1 finish_owner_family=1 finished_receiver_isolated=1\n");
 else sdk_trace_fprintf(stderr,"ACTIVITY_SELFTEST FAIL checks=%u\n",*checks);
 if(acquired)dvm_gil_release(v);
 return okay;
#undef CHECK
}


bool activity_runtime_gc_visit_owner(struct dvm *v,dvm_ref owner_ref,dvm_gc_visit_fn visit,void *u){
 if(state.vm!=v||!dvm_gil_held(v)||!visit)return false;
 for(unsigned i=0;i<32;i++)if(shadow_inflaters[i].ref==owner_ref&&shadow_inflater_identity(i)&&shadow_inflaters[i].context)visit(&shadow_inflaters[i].context,u);
 struct shadow_node_state *n=shadow_node(owner_ref);if(n){if(n->context)visit(&n->context,u);if(n->click)visit(&n->click,u);if(n->touch)visit(&n->touch,u);}
 struct guest_activity *g=guest_find(owner_ref);if(g){dvm_ref *r[8];guest_slots(g,r);for(unsigned j=1;j<8;j++)if(*r[j]){if(g->phase>=4&&activity_identity(v,*r[j])!=g->generations[j]){*r[j]=0;g->generations[j]=0;}else visit(r[j],u);}}
 return true;
}

/* Real heap reuse test, called directly by GC2 harness outside native scopes. */
bool activity_runtime_gc_identity_selftest(struct dvm *v){
 if(state.vm!=v||!dvm_gil_held(v)||dvm_gc_has_native_scopes(v))return false;
 unsigned ni=128,ii=32,ti=64;for(unsigned i=0;i<128;i++)if(!shadow_nodes[i].ref){ni=i;break;}for(unsigned i=0;i<32;i++)if(!shadow_inflaters[i].ref){ii=i;break;}for(unsigned i=0;i<64;i++)if(!transitions[i].activity){ti=i;break;}if(ni==128||ii==32||ti==64)return false;
 struct dvm_class *k=dvm_find_class_or_external(v,"java/lang/Object");dvm_ref old=dvm_new_object(v,k);if(!old)return false;uint64_t gen=activity_identity(v,old);
 shadow_nodes[ni].ref=old;shadow_nodes[ni].identity=gen;shadow_inflaters[ii].ref=old;shadow_inflaters[ii].identity=gen;shadow_inflaters[ii].context=old;shadow_inflaters[ii].context_identity=gen;transitions[ti].activity=old;transitions[ti].identity=gen;
 dvm__discard_temporary(v,old);dvm_ref fresh=dvm_new_object(v,k);
 bool ok=fresh==old&&activity_identity(v,fresh)!=gen&&!shadow_node(old)&&!shadow_inflater_identity(ii);
 if(transitions[ti].activity&&activity_identity(v,transitions[ti].activity)!=transitions[ti].identity)memset(&transitions[ti],0,sizeof transitions[ti]);ok=ok&&!transitions[ti].activity;
 memset(&shadow_nodes[ni],0,sizeof shadow_nodes[ni]);memset(&shadow_inflaters[ii],0,sizeof shadow_inflaters[ii]);memset(&transitions[ti],0,sizeof transitions[ti]);dvm__discard_temporary(v,fresh);
 sdk_trace_fprintf(stderr,"GC2_NATIVE_IDENTITY activity_shadow_reuse=%d same_id=%d\n",ok,fresh==old);return ok;
}
