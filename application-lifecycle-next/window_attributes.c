#include "window_attributes.h"
#include "dvm/dvm_internal.h"
#include "gc_runtime.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
extern int sdk_trace_fprintf(FILE *,const char *,...);
#define CAP 128
typedef struct {dvm_ref window,params;dvm_gc_handle owner;uint64_t params_identity;int busy;} slot;
static struct {struct dvm *vm;struct dvm_class *window,*params;slot slots[CAP];} state;
static pthread_mutex_t mu=PTHREAD_MUTEX_INITIALIZER;
static void retire(slot *s){if(s->owner)dvm_gc_handle_drop(state.vm,s->owner);memset(s,0,sizeof *s);}
static bool valid(slot *s,dvm_ref window){return s->window==window&&s->owner&&dvm_gc_handle_ref(state.vm,s->owner)==window;}
static bool params_valid(slot *s){struct dvm_object *o=dvm__obj(state.vm,s->params);return o&&o->identity_generation==s->params_identity;}
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
void window_attributes_vm_shutdown(struct dvm*v){pthread_mutex_lock(&mu);if(state.vm==v){for(int i=0;i<CAP;++i)retire(&state.slots[i]);memset(&state,0,sizeof state);}pthread_mutex_unlock(&mu);}
dvm_ref window_attributes_get(struct dvm*v,dvm_ref window)
{
 if(!v)return 0;pthread_mutex_lock(&mu);
 if(state.vm!=v){pthread_mutex_unlock(&mu);dvm__throw(v,"java/lang/IllegalStateException","Window attributes VM unavailable");return 0;}
 struct dvm_object *o=window?dvm__obj(v,window):NULL;
 if(!o||!o->cls||!dvm__class_assignable(v,o->cls,state.window)){pthread_mutex_unlock(&mu);dvm__throw(v,"java/lang/IllegalArgumentException","invalid Window receiver");return 0;}
 slot *empty=NULL;
 for(int i=0;i<CAP;i++){slot *s=&state.slots[i];if(valid(s,window)){dvm_ref ref=params_valid(s)?s->params:0;pthread_mutex_unlock(&mu);if(!ref)dvm__throw(v,"java/lang/IllegalStateException","Window attributes child identity lost");return ref;}if(!s->busy&&(!s->window||!dvm_gc_handle_ref(v,s->owner))){retire(s);if(!empty)empty=s;}}
 if(!empty){pthread_mutex_unlock(&mu);dvm__throw(v,"java/lang/OutOfMemoryError","Window attributes capacity exhausted");return 0;}
 dvm_ref params=dvm_new_object(v,state.params);
 if(params){dvm_gc_handle owner=dvm_gc_handle_new(v,window,DVM_GC_HANDLE_WEAK);if(!owner){pthread_mutex_unlock(&mu);return 0;}empty->window=window;empty->params=params;empty->owner=owner;empty->params_identity=dvm__obj(v,params)->identity_generation;}
 pthread_mutex_unlock(&mu);return params;
}
bool window_attributes_set_flags(struct dvm *v,dvm_ref window,int flags,int mask){
 if(!v||!dvm_gil_held(v))return false;
 if(((unsigned)mask&~0x400u)||((unsigned)flags&~0x400u)){dvm__throw(v,"java/lang/UnsupportedOperationException","unsupported Window flags");return false;}
 dvm_ref params=window_attributes_get(v,window);if(!params)return false;
 pthread_mutex_lock(&mu);slot *e=NULL;for(int i=0;i<CAP;i++)if(valid(&state.slots[i],window)){e=&state.slots[i];break;}
 if(!e||e->busy||!flags_host){pthread_mutex_unlock(&mu);dvm__throw(v,"java/lang/UnsupportedOperationException","Window flags host unavailable or busy");return false;}
 window_flags_host cb=flags_host;void *user=flags_user;uint64_t serial=v->serial;e->busy=1;dvm_pin(v,window);dvm_pin(v,params);pthread_mutex_unlock(&mu);
 unsigned depth=dvm_gil_unlock_all(v);int result=cb(user,flags,mask);dvm_gil_relock(v,depth);
 pthread_mutex_lock(&mu);bool still_valid=state.vm==v&&v->serial==serial&&valid(e,window)&&e->params==params&&params_valid(e)&&e->busy;if(still_valid)e->busy=0;pthread_mutex_unlock(&mu);
 bool ok=still_valid&&result==0;union dvm_value old={0};if(ok)ok=dvm_get_field(v,params,"flags","I",&old);if(ok){old.i=(old.i&~mask)|(flags&mask);ok=dvm_set_field(v,params,"flags","I",old);}
 dvm_unpin(v,params);dvm_unpin(v,window);if(!ok&&!dvm_exception(v))dvm__throw(v,"java/lang/UnsupportedOperationException","Window flags host rejected");
 sdk_trace_fprintf(stderr,"WINDOW_FLAGS flags=0x%x mask=0x%x accepted=%d visibility_verified=0\n",flags,mask,ok);return ok;
}static void remove_window(struct dvm*v,dvm_ref ref){pthread_mutex_lock(&mu);if(state.vm==v)for(int i=0;i<CAP;i++)if(valid(&state.slots[i],ref))retire(&state.slots[i]);pthread_mutex_unlock(&mu);}
bool window_attributes_gc_visit_owner(struct dvm *v,dvm_ref owner,dvm_gc_visit_fn visit,void *user){
 if(!v||v!=state.vm||!visit||!dvm_gil_held(v)||pthread_mutex_trylock(&mu))return false;
 bool ok=true;for(int i=0;i<CAP;++i)if(valid(&state.slots[i],owner)){if(!params_valid(&state.slots[i]))ok=false;else visit(&state.slots[i].params,user);}
 pthread_mutex_unlock(&mu);return ok;
}
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

struct window_gc_check {dvm_ref wanted;unsigned seen;};
static void window_gc_check_visit(dvm_ref *slot,void *user){struct window_gc_check *s=user;if(slot&&*slot==s->wanted)++s->seen;}
bool window_attributes_gc_identity_selftest(struct dvm *v){
 if(v!=state.vm||!dvm_gil_held(v)||dvm_exception(v)||dvm_gc_has_native_scopes(v))return false;
 dvm_ref owner=dvm_new_object(v,state.window),params=window_attributes_get(v,owner);if(!params)return false;
 slot *old=NULL;for(int i=0;i<CAP;++i)if(valid(&state.slots[i],owner))old=&state.slots[i];
 struct window_gc_check seen={.wanted=params};bool ok=old&&window_attributes_gc_visit_owner(v,owner,window_gc_check_visit,&seen)&&seen.seen==1;
 uint64_t identity=dvm__obj(v,owner)->identity_generation;dvm__discard_temporary(v,owner);dvm_ref replacement=dvm_new_object(v,state.window);
 ok=replacement==owner&&dvm__obj(v,replacement)->identity_generation!=identity&&!valid(old,replacement)&&ok;
 dvm_ref newparams=window_attributes_get(v,replacement);ok=newparams&&newparams!=params&&valid(old,replacement)&&ok;
 remove_window(v,replacement);if(newparams)dvm__discard_temporary(v,newparams);dvm__discard_temporary(v,params);dvm__discard_temporary(v,replacement);
 dprintf(2,"WINDOW_ATTRIBUTES_GC_IDENTITY_SELFTEST %s same_id_new_generation=1 stale_lookup_rejected=1 slot_reused=1 owner_edge=1 global_roots=0\n",ok?"PASS":"FAIL");return ok;
}

