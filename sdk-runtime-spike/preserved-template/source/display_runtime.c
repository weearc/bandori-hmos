#include "display_runtime.h"
#include "dvm/dvm_internal.h"
#include "platform_runtime.h"
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
extern int sdk_trace_fprintf(FILE *,const char *,...);
static struct {struct dvm *vm;struct dvm_class *cls;dvm_ref display,manager;uint64_t generation;} state;
extern int32_t sdk_get_display_orientation(bool *host_present);
static uint64_t next_generation=1;
static pthread_mutex_t mu=PTHREAD_MUTEX_INITIALIZER;
static display_rotation_host host;
static void *host_user;
__attribute__((visibility("default"))) void sdk_set_display_rotation_host(display_rotation_host callback,void *user){pthread_mutex_lock(&mu);host=callback;host_user=user;pthread_mutex_unlock(&mu);}
bool display_runtime_vm_start(struct dvm*v){if(!v)return false;pthread_mutex_lock(&mu);if(state.vm){bool same=state.vm==v;pthread_mutex_unlock(&mu);return same;}if(!next_generation){pthread_mutex_unlock(&mu);return false;}struct dvm_class*c=dvm_find_class_or_external(v,"android/view/Display");if(!c){pthread_mutex_unlock(&mu);return false;}state.vm=v;state.cls=c;state.generation=next_generation++;pthread_mutex_unlock(&mu);return true;}
void display_runtime_vm_shutdown(struct dvm*v){pthread_mutex_lock(&mu);if(state.vm==v){if(state.manager)dvm_unpin(v,state.manager);memset(&state,0,sizeof state);}pthread_mutex_unlock(&mu);}
dvm_ref display_runtime_default(struct dvm*v){if(!v)return 0;pthread_mutex_lock(&mu);if(state.vm!=v){pthread_mutex_unlock(&mu);dvm__throw(v,"java/lang/IllegalStateException","Display VM unavailable");return 0;}if(!state.display)state.display=dvm_new_object(v,state.cls);dvm_ref ref=state.display;pthread_mutex_unlock(&mu);return ref;}
bool display_runtime_call(struct dvm*v,const char*c,const char*m,const char*sig,dvm_ref self,const union dvm_value*a,int n,union dvm_value*out){
 if(!v||!c||!m||!sig)return false;
 if(!strcmp(c,"android/content/Context")&&!strcmp(m,"getSystemService")&&
    !strcmp(sig,"(Ljava/lang/String;)Ljava/lang/Object;")&&n==1&&a&&a[0].l){
  const char *service=dvm_string_utf8(v,a[0].l);
  if(!service||strcmp(service,"display")||dvm_string_utf8_length(v,a[0].l)!=7)return false;
  struct dvm_object *receiver=dvm__obj(v,self);
  struct dvm_class *context=dvm_find_class_or_external(v,"android/content/Context");
  if(!receiver||!context||!dvm__class_assignable(v,receiver->cls,context)){
   dvm__throw(v,"java/lang/IllegalArgumentException","invalid display service Context");return true;
  }
  bool host_present=false;
  sdk_get_display_orientation(&host_present);
  if(!host_present){out->l=0;return true;}
  pthread_mutex_lock(&mu);
  if(state.vm!=v){pthread_mutex_unlock(&mu);dvm__throw(v,"java/lang/IllegalStateException","Display VM unavailable");return true;}
  if(!state.manager){
   state.manager=dvm_new_object(v,dvm_find_class_or_external(v,"android/hardware/display/DisplayManager"));
   if(state.manager)dvm_pin(v,state.manager);
  }
  out->l=state.manager;
  pthread_mutex_unlock(&mu);
  if(!out->l)dvm__throw(v,"java/lang/OutOfMemoryError","DisplayManager");
  return true;
 }
 if(!strcmp(c,"android/hardware/display/DisplayManager")&&!strcmp(m,"getDisplay")&&
    !strcmp(sig,"(I)Landroid/view/Display;")){
  pthread_mutex_lock(&mu);bool valid=state.vm==v&&self&&self==state.manager;pthread_mutex_unlock(&mu);
  if(!valid||n!=1||!a){dvm__throw(v,"java/lang/IllegalArgumentException","invalid DisplayManager.getDisplay");return true;}
  out->l=a[0].i==0?display_runtime_default(v):0;
  return true;
 }
 if(strcmp(c,"android/view/Display")||strcmp(sig,"()I")||
    (strcmp(m,"getRotation")&&strcmp(m,"getDisplayId")&&strcmp(m,"getWidth")&&strcmp(m,"getHeight")))return false;
 pthread_mutex_lock(&mu);if(state.vm!=v||!self||state.display!=self){pthread_mutex_unlock(&mu);dvm__throw(v,"java/lang/IllegalArgumentException","invalid Display receiver or VM");return true;}
 if(n!=0||!out){pthread_mutex_unlock(&mu);dvm__throw(v,"java/lang/IllegalArgumentException","invalid Display.getRotation arguments");return true;}
 if(!strcmp(m,"getDisplayId")){pthread_mutex_unlock(&mu);out->i=0;return true;}
 if(!strcmp(m,"getWidth")||!strcmp(m,"getHeight")){
  pthread_mutex_unlock(&mu);
  bool host_present=false;sdk_get_display_orientation(&host_present);
  if(!host_present){dvm__throw(v,"java/lang/UnsupportedOperationException","Display dimensions host unavailable");return true;}
  dvm_ref metrics=dvm_new_object(v,dvm_find_class_or_external(v,"android/util/DisplayMetrics"));
  if(!metrics){dvm__throw(v,"java/lang/OutOfMemoryError","DisplayMetrics");return true;}
  dvm_pin(v,metrics);
  union dvm_value ignored={0};
  bool initialized=platform_runtime_call(v,"android/util/DisplayMetrics","<init>","()V",metrics,NULL,0,&ignored);
  bool read=initialized&&!dvm_exception(v)&&dvm_get_field(v,metrics,!strcmp(m,"getWidth")?"widthPixels":"heightPixels","I",out);
  dvm_unpin(v,metrics);
  if(!read&&!dvm_exception(v))dvm__throw(v,"java/lang/IllegalStateException","Display dimensions unavailable");
  return true;
 }
 display_rotation_host callback=host;void*user=host_user;uint64_t generation=state.generation;pthread_mutex_unlock(&mu);
 if(!callback){sdk_trace_fprintf(stderr,"DISPLAY_ROTATION result=no_host\n");dvm__throw(v,"java/lang/UnsupportedOperationException","Display rotation host unavailable");return true;}
 int rotation=-1;unsigned depth=dvm_gil_held(v)?dvm_gil_unlock_all(v):0;int result=callback(user,&rotation);if(depth)dvm_gil_relock(v,depth);
 pthread_mutex_lock(&mu);bool valid=state.vm==v&&state.display==self&&state.generation==generation;pthread_mutex_unlock(&mu);
 sdk_trace_fprintf(stderr,"DISPLAY_ROTATION host_result=%d value=%d identity_valid=%d\n",result,rotation,valid);
 if(!valid||result||rotation<0||rotation>3){dvm__throw(v,"java/lang/UnsupportedOperationException","Display rotation unavailable (%d, %d)",result,rotation);return true;}
 out->i=rotation;return true;
}
typedef struct {struct dvm*vm;int value,result,calls,held;} mock_rotation;
static int mock_host(void*user,int*out){mock_rotation*m=user;m->calls++;m->held=dvm_gil_held(m->vm);*out=m->value;return m->result;}
bool display_runtime_selftest(struct dvm*v){
 if(!v||dvm_exception(v))return false;dvm_ref display=display_runtime_default(v);if(!display)return false;
 pthread_mutex_lock(&mu);display_rotation_host saved=host;void*saved_user=host_user;pthread_mutex_unlock(&mu);
 mock_rotation mock={.vm=v,.value=3};sdk_set_display_rotation_host(mock_host,&mock);union dvm_value out={0};
 bool handled=display_runtime_call(v,"android/view/Display","getRotation","()I",display,NULL,0,&out);
 bool ok=handled&&!dvm_exception(v)&&out.i==3&&mock.calls==1&&!mock.held&&display_runtime_default(v)==display;
 mock.value=4;handled=display_runtime_call(v,"android/view/Display","getRotation","()I",display,NULL,0,&out);ok=(handled&&dvm_exception(v))&&ok;if(dvm_exception(v))dvm_clear_exception(v);
 mock.value=0;mock.result=-1;handled=display_runtime_call(v,"android/view/Display","getRotation","()I",display,NULL,0,&out);ok=(handled&&dvm_exception(v))&&ok;if(dvm_exception(v))dvm_clear_exception(v);
 sdk_set_display_rotation_host(NULL,NULL);handled=display_runtime_call(v,"android/view/Display","getRotation","()I",display,NULL,0,&out);ok=(handled&&dvm_exception(v))&&ok;if(dvm_exception(v))dvm_clear_exception(v);
 handled=display_runtime_call(v,"android/view/Display","getRotation","()I",0,NULL,0,&out);ok=(handled&&dvm_exception(v))&&ok;if(dvm_exception(v))dvm_clear_exception(v);
 handled=display_runtime_call(v,"android/view/Display","getRefreshRate","()F",display,NULL,0,&out);ok=!handled&&ok;
 sdk_set_display_rotation_host(saved,saved_user);sdk_trace_fprintf(stderr,"DISPLAY_RUNTIME_SELFTEST %s mock=1 host_real=0 stable_identity=1 invalid_rejected=1\n",ok?"PASS":"FAIL");return ok;
}
