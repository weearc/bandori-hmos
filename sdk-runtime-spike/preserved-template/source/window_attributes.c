#include "window_attributes.h"
#include "dvm/dvm_internal.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
extern int sdk_trace_fprintf(FILE *,const char *,...);
#define CAP 128
typedef struct {dvm_ref window,params;int busy;} slot;
static struct {struct dvm *vm;struct dvm_class *window,*params;slot slots[CAP];} state;
static pthread_mutex_t mu=PTHREAD_MUTEX_INITIALIZER;
static window_flags_host flags_host;static void *flags_user;
void sdk_set_window_flags_host(window_flags_host cb,void *user){pthread_mutex_lock(&mu);flags_host=cb;flags_user=user;pthread_mutex_unlock(&mu);}
window_flags_host window_attributes_get_flags_host(void **user){pthread_mutex_lock(&mu);window_flags_host cb=flags_host;if(user)*user=flags_user;pthread_mutex_unlock(&mu);return cb;}
static bool install_field(struct dvm_class *c)
{
 if(!c || !c->external)return false;
 int base=c->super?c->super->islots:0;
 if(c->ifields || c->nifields || c->islots || base>UINT16_MAX-2)return false;
 struct dvm_field *f=calloc(2,sizeof *f);if(!f)return false;
 const char *names[]={"layoutInDisplayCutoutMode","flags"};
 for(int i=0;i<2;i++){f[i].cls=c;f[i].name=names[i];f[i].type="I";f[i].kind='I';f[i].width=1;f[i].access=1;f[i].slot=(uint16_t)(base+i);}
 c->ifields=f;c->nifields=2;c->islots=base+2;return true;
}
bool window_attributes_vm_start(struct dvm*v)
{
 if(!v)return false;pthread_mutex_lock(&mu);
 if(state.vm){bool same=state.vm==v;pthread_mutex_unlock(&mu);return same;}
 struct dvm_class *window=dvm_find_class_or_external(v,"android/view/Window");
 struct dvm_class *params=dvm_find_class_or_external(v,"android/view/WindowManager$LayoutParams");
 if(!window||!params||!install_field(params)){pthread_mutex_unlock(&mu);return false;}
 state.vm=v;state.window=window;state.params=params;pthread_mutex_unlock(&mu);return true;
}
void window_attributes_vm_shutdown(struct dvm*v){pthread_mutex_lock(&mu);if(state.vm==v)memset(&state,0,sizeof state);pthread_mutex_unlock(&mu);}
dvm_ref window_attributes_get(struct dvm*v,dvm_ref window)
{
 if(!v)return 0;pthread_mutex_lock(&mu);
 if(state.vm!=v){pthread_mutex_unlock(&mu);dvm__throw(v,"java/lang/IllegalStateException","Window attributes VM unavailable");return 0;}
 struct dvm_object *o=window?dvm__obj(v,window):NULL;
 if(!o||!o->cls||!dvm__class_assignable(v,o->cls,state.window)){pthread_mutex_unlock(&mu);dvm__throw(v,"java/lang/IllegalArgumentException","invalid Window receiver");return 0;}
 slot *empty=NULL;
 for(int i=0;i<CAP;i++){if(state.slots[i].window==window){dvm_ref ref=state.slots[i].params;pthread_mutex_unlock(&mu);return ref;}if(!empty&&!state.slots[i].window)empty=&state.slots[i];}
 if(!empty){pthread_mutex_unlock(&mu);dvm__throw(v,"java/lang/OutOfMemoryError","Window attributes capacity exhausted");return 0;}
 dvm_ref params=dvm_new_object(v,state.params);
 if(params){empty->window=window;empty->params=params;}
 pthread_mutex_unlock(&mu);return params;
}
bool window_attributes_set_flags(struct dvm *v,dvm_ref window,int flags,int mask){
 if(!v||!dvm_gil_held(v))return false;
 if(((unsigned)mask&~0x400u)||((unsigned)flags&~0x400u)){dvm__throw(v,"java/lang/UnsupportedOperationException","unsupported Window flags");return false;}
 dvm_ref params=window_attributes_get(v,window);if(!params)return false;
 pthread_mutex_lock(&mu);slot *e=NULL;for(int i=0;i<CAP;i++)if(state.slots[i].window==window){e=&state.slots[i];break;}
 if(!e||e->busy||!flags_host){pthread_mutex_unlock(&mu);dvm__throw(v,"java/lang/UnsupportedOperationException","Window flags host unavailable or busy");return false;}
 window_flags_host cb=flags_host;void *user=flags_user;uint64_t serial=v->serial;e->busy=1;dvm_pin(v,window);dvm_pin(v,params);pthread_mutex_unlock(&mu);
 unsigned depth=dvm_gil_unlock_all(v);int result=cb(user,flags,mask);dvm_gil_relock(v,depth);
 pthread_mutex_lock(&mu);bool valid=state.vm==v&&v->serial==serial&&e->window==window&&e->params==params&&e->busy;if(valid)e->busy=0;pthread_mutex_unlock(&mu);
 bool ok=valid&&result==0;union dvm_value old={0};if(ok)ok=dvm_get_field(v,params,"flags","I",&old);if(ok){old.i=(old.i&~mask)|(flags&mask);ok=dvm_set_field(v,params,"flags","I",old);}
 dvm_unpin(v,params);dvm_unpin(v,window);if(!ok&&!dvm_exception(v))dvm__throw(v,"java/lang/UnsupportedOperationException","Window flags host rejected");
 sdk_trace_fprintf(stderr,"WINDOW_FLAGS flags=0x%x mask=0x%x accepted=%d visibility_verified=0\n",flags,mask,ok);return ok;
}static void remove_window(struct dvm*v,dvm_ref ref){pthread_mutex_lock(&mu);if(state.vm==v)for(int i=0;i<CAP;i++)if(state.slots[i].window==ref)memset(&state.slots[i],0,sizeof state.slots[i]);pthread_mutex_unlock(&mu);}
bool window_attributes_selftest(struct dvm*v)
{
 if(!v||dvm_exception(v))return false;pthread_mutex_lock(&mu);struct dvm_class *window=state.vm==v?state.window:NULL;pthread_mutex_unlock(&mu);if(!window)return false;
 dvm_ref a=dvm_new_object(v,window),b=dvm_new_object(v,window);if(!a||!b)return false;
 dvm_ref pa=window_attributes_get(v,a),again=window_attributes_get(v,a),pb=window_attributes_get(v,b);bool ok=pa&&pa==again&&pb&&pa!=pb&&!dvm_exception(v);
 union dvm_value x={0};bool got=pa&&dvm_get_field(v,pa,"layoutInDisplayCutoutMode","I",&x);ok=got&&x.i==0&&!dvm_exception(v)&&ok;
 x.i=3;bool set=pa&&dvm_set_field(v,pa,"layoutInDisplayCutoutMode","I",x);x.i=-1;got=pa&&dvm_get_field(v,pa,"layoutInDisplayCutoutMode","I",&x);ok=set&&got&&x.i==3&&!dvm_exception(v)&&ok;
 x.i=-1;got=pb&&dvm_get_field(v,pb,"layoutInDisplayCutoutMode","I",&x);ok=got&&x.i==0&&!dvm_exception(v)&&ok;
 dvm_ref invalid=window_attributes_get(v,0);bool rejected=!invalid&&dvm_exception(v);ok=rejected&&ok;if(dvm_exception(v))dvm_clear_exception(v);
 remove_window(v,a);remove_window(v,b);
 sdk_trace_fprintf(stderr,"WINDOW_ATTRIBUTES_SELFTEST %s stable_identity=1 field_roundtrip=1 window_isolation=1 invalid_rejected=1 host_cutout_applied=0\n",ok?"PASS":"FAIL");return ok;
}

