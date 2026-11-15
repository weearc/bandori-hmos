#include "activity_runtime.h"
#include "android_basics.h"
#include "guest_identity.h"
#include <string.h>
#include <pthread.h>
void sdk_trace_missing_api(const char *,const char *,const char *);
/* Side table, never stored in object->data: ContextWrapper uses its own layout. */
static struct { struct dvm *vm; dvm_ref activity, application; int lifecycle; dvm_ref layout; int orientation; } state;
static pthread_mutex_t host_lock=PTHREAD_MUTEX_INITIALIZER;
static int (*host_mount)(void *, int);
static void *host_user;
void activity_runtime_set_host(int (*mount)(void *, int),void *user){pthread_mutex_lock(&host_lock);host_mount=mount;host_user=user;pthread_mutex_unlock(&host_lock);}
__attribute__((visibility("default"))) void sdk_set_activity_host(int (*mount)(void *, int),void *user){activity_runtime_set_host(mount,user);}
void activity_runtime_vm_start(struct dvm *v) {
 memset(&state,0,sizeof state);state.vm=v;
 struct dvm_class *a=dvm__class_by_desc(v,"Landroid/app/Activity;");
 struct dvm_class *w=dvm__class_by_desc(v,"Landroid/content/ContextWrapper;");
 if(a&&w)a->super=w;
 struct dvm_class *view=dvm__class_by_desc(v,"Landroid/view/View;");
 struct dvm_class *group=dvm__class_by_desc(v,"Landroid/view/ViewGroup;");
 struct dvm_class *layout=dvm__class_by_desc(v,"Landroid/widget/LinearLayout;");
 if(group&&view)group->super=view;if(layout&&group)layout->super=group;
}
bool activity_runtime_call(struct dvm *v,const char *c,const char *m,const char *sig,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 (void)n;
 if(state.vm!=v)return false;
 if(!strcmp(c,"android/app/Activity")&&!strcmp(m,"<init>")&&!strcmp(sig,"()V")){
  if(!self||!dvm__obj(v,self)||state.activity){dvm__throw(v,"java/lang/IllegalStateException","Activity construction unavailable");return true;}
  state.application=android_basics_application(v);
  if(!state.application)return true;
  state.activity=self;state.lifecycle=1; /* NEW: no attach, window or lifecycle dispatch. */
  return true;
 }
 if(self==state.activity && !strcmp(c,"android/app/Activity") && !strcmp(m,"onCreate") && !strcmp(sig,"(Landroid/os/Bundle;)V")){
  if(state.lifecycle!=1){dvm__throw(v,"java/lang/IllegalStateException","Activity.onCreate invalid state");return true;}
  state.lifecycle=2; /* CREATED; no window or UI service is installed. */
  return true;
 }
 if(!strcmp(c,"android/widget/LinearLayout")&&!strcmp(m,"<init>")&&!strcmp(sig,"(Landroid/content/Context;)V")){
  if(!self||!dvm__obj(v,self)||state.layout||state.lifecycle!=2||!a||a[0].l!=state.activity){dvm__throw(v,"java/lang/IllegalStateException","LinearLayout requires the created diagnostic Activity context");return true;}
  state.layout=self;state.orientation=0;return true;
 }
 if(!strcmp(c,"android/widget/LinearLayout")&&!strcmp(m,"setOrientation")&&!strcmp(sig,"(I)V")){
  if(!self||self!=state.layout||!a||(a[0].i!=0&&a[0].i!=1)){dvm__throw(v,"java/lang/IllegalArgumentException","unsupported LinearLayout or orientation");return true;}
  state.orientation=a[0].i;return true;
 }
 if(!self||self!=state.activity||(state.lifecycle!=1&&state.lifecycle!=2))return false;
 if(strcmp(c,"android/app/Activity")&&strcmp(c,"android/content/ContextWrapper")&&strcmp(c,"android/content/Context")&&strcmp(c,"com/bilibili/star/bili/PermissionActivity"))return false;
 if(!strcmp(m,"setContentView")&&!strcmp(sig,"(Landroid/view/View;)V")){
  if(state.lifecycle!=2||!a||!state.layout||a[0].l!=state.layout){dvm__throw(v,"java/lang/UnsupportedOperationException","only the diagnostic LinearLayout can be mounted");return true;}
  pthread_mutex_lock(&host_lock);int (*mount)(void *,int)=host_mount;void *user=host_user;pthread_mutex_unlock(&host_lock);
  if(!mount||mount(user,state.orientation)!=0){sdk_trace_missing_api("android/app/Activity","setContentView","(Landroid/view/View;)V");dvm__throw(v,"java/lang/UnsupportedOperationException","host LinearLayout mount unavailable or rejected");return true;}
  return true;
 }
 if(!strcmp(m,"getApplicationContext")&&!strcmp(sig,"()Landroid/content/Context;")){out->l=state.application;return true;}
 if(!strcmp(m,"getBaseContext")&&!strcmp(sig,"()Landroid/content/Context;")){out->l=state.application;return true;}
 if(!strcmp(m,"getApplication")&&!strcmp(sig,"()Landroid/app/Application;")){out->l=state.application;return true;}
 if(!strcmp(m,"getPackageName")&&!strcmp(sig,"()Ljava/lang/String;")){out->l=dvm_new_string(v,GUEST_EXPECTED_PACKAGE);return true;}
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

