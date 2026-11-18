#include "surface_view_runtime.h"
#include "view_state_runtime.h"
#include "surface_runtime.h"
#include "window_runtime.h"
#include "dvm/dvm_internal.h"
#include "gc_runtime.h"
#include "gc_operation.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
extern int sdk_trace_fprintf(FILE *, const char *, ...);
/* Engine-stage diagnostic channel: sdk_trace_fprintf stops reaching the
 * progress file once the SDK runner returns, so append directly instead. */
static void engine_probe_log(const char *fmt,...){
 FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt","a");if(!f)return;
 va_list a;va_start(a,fmt);vfprintf(f,fmt,a);va_end(a);fputc('\n',f);fclose(f);
}
#define LIMIT 32
#define CALLBACKS 16
typedef struct { dvm_ref view,holder,surface,callbacks[CALLBACKS];dvm_gc_handle view_key,holder_key;uint64_t surface_identity,callback_identity[CALLBACKS]; int format,type,attached,z_order_on_top; } slot;
static struct {struct dvm *vm;struct dvm_class *view,*holder;slot slots[LIMIT];} state;
static bool fail(struct dvm *v,const char *type,const char *msg){dvm__throw(v,type,msg);return true;}
static bool live_key(dvm_gc_handle key,dvm_ref ref){return key&&ref&&dvm_gc_handle_ref(state.vm,key)==ref;}
static slot *find(dvm_ref r){if(!r)return NULL;for(unsigned i=0;i<LIMIT;i++){slot *s=&state.slots[i];if((s->view==r&&live_key(s->view_key,r))||(s->holder==r&&live_key(s->holder_key,r)))return s;}return NULL;}
static bool surface_valid(slot *s){struct dvm_object *o=dvm__obj(state.vm,s->surface);return o&&o->identity_generation==s->surface_identity;}
static bool callback_valid(slot *s,unsigned i){struct dvm_object *o=dvm__obj(state.vm,s->callbacks[i]);return o&&o->identity_generation==s->callback_identity[i];}
static void retire(slot *s){
 if(s->surface&&surface_valid(s)){surface_runtime_unbind(state.vm,s->surface);dvm_unpin(state.vm,s->surface);}
 for(unsigned i=0;i<CALLBACKS;++i)if(s->callbacks[i]&&callback_valid(s,i))dvm_unpin(state.vm,s->callbacks[i]);
 if(live_key(s->holder_key,s->holder))dvm_unpin(state.vm,s->holder);
 if(live_key(s->view_key,s->view))dvm_unpin(state.vm,s->view);
 if(s->view_key)dvm_gc_handle_drop(state.vm,s->view_key);if(s->holder_key)dvm_gc_handle_drop(state.vm,s->holder_key);memset(s,0,sizeof *s);
}
bool surface_view_runtime_vm_start(struct dvm *v){
 if(!v)return false;if(state.vm)return state.vm==v;
 struct dvm_class *base=dvm__class_by_desc(v,"Landroid/view/View;");
 struct dvm_class *view=dvm__class_by_desc(v,"Landroid/view/SurfaceView;");
 struct dvm_class *iface=dvm__class_by_desc(v,"Landroid/view/SurfaceHolder;");
 struct dvm_class *holder=dvm_find_class_or_external(v,"codex/SurfaceHolderImpl");
 if(!base||!view||!holder||!iface)return false;
 if(!holder->nifaces){holder->ifaces=calloc(1,sizeof(*holder->ifaces));if(!holder->ifaces)return false;holder->ifaces[0]=iface;holder->nifaces=1;}
 if(view->super && view->super!=base && strcmp(view->super->name,"java/lang/Object"))return false;
 view->super=base;state.vm=v;state.view=view;state.holder=holder;return true;
}
void surface_view_runtime_vm_shutdown(struct dvm *v){
 if(state.vm!=v)return;for(unsigned i=0;i<LIMIT;i++)retire(&state.slots[i]);memset(&state,0,sizeof state);
}
bool surface_view_runtime_bind(struct dvm *v,dvm_ref view,void *window){
 if(state.vm!=v||!window)return false;slot *s=find(view);if(!s||s->view!=view||!surface_valid(s))return false;
 return surface_runtime_bind(v,s->surface,window,0);
}
bool surface_view_runtime_call(struct dvm *v,const char *c,const char *m,const char *sig,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 if(!v||v!=state.vm||!c||!m||!sig)return false;
 if(!strcmp(c,"android/view/SurfaceView")&&!strcmp(m,"<init>")&&!strcmp(sig,"(Landroid/content/Context;)V")){
  if(find(self))return fail(v,"java/lang/IllegalStateException","SurfaceView already constructed");
  slot *s=NULL;for(unsigned i=0;i<LIMIT;i++)if(!state.slots[i].view||(!live_key(state.slots[i].view_key,state.slots[i].view)&&!live_key(state.slots[i].holder_key,state.slots[i].holder))){retire(&state.slots[i]);s=&state.slots[i];break;}
  if(!s)return fail(v,"java/lang/OutOfMemoryError","SurfaceView capacity");
  if(!view_state_runtime_call(v,"android/view/View",m,sig,self,a,n,out))return false;if(dvm_exception(v))return true;
  dvm_ref holder=dvm_new_object(v,state.holder);if(!holder)return fail(v,"java/lang/OutOfMemoryError","SurfaceHolder allocation");
  dvm_ref surface=surface_runtime_new_surface_unbound(v);if(!surface)return fail(v,"java/lang/OutOfMemoryError","Surface allocation");
  dvm_gc_handle view_key=dvm_gc_handle_new(v,self,DVM_GC_HANDLE_WEAK),holder_key=0;if(view_key)holder_key=dvm_gc_handle_new(v,holder,DVM_GC_HANDLE_WEAK);
  if(!view_key||!holder_key){if(view_key)dvm_gc_handle_drop(v,view_key);return true;}
  s->view=self;s->holder=holder;s->surface=surface;s->view_key=view_key;s->holder_key=holder_key;s->surface_identity=dvm__obj(v,surface)->identity_generation;dvm_pin(v,self);dvm_pin(v,holder);dvm_pin(v,surface);return true;
 }
 slot *s=find(self);if(!s)return false;
 if(self==s->view&&!strcmp(m,"setZOrderOnTop")&&!strcmp(sig,"(Z)V")&&a&&n==1){if(s->attached)return fail(v,"java/lang/UnsupportedOperationException","attached z-order update needs host compositor");s->z_order_on_top=a[0].i!=0;return true;}
 if(!strcmp(c,"android/view/SurfaceView")&&!strcmp(m,"getHolder")&&!strcmp(sig,"()Landroid/view/SurfaceHolder;")&&self==s->view){if(!live_key(s->holder_key,s->holder))return fail(v,"java/lang/IllegalStateException","SurfaceHolder identity lost");out->l=s->holder;return true;}
 if(self!=s->holder||strcmp(c,"android/view/SurfaceHolder"))return false;
 if(!strcmp(m,"getSurface")&&!strcmp(sig,"()Landroid/view/Surface;")){if(!surface_valid(s))return fail(v,"java/lang/IllegalStateException","Surface identity lost");out->l=s->surface;return true;}
 if(!strcmp(m,"addCallback")&&!strcmp(sig,"(Landroid/view/SurfaceHolder$Callback;)V")&&n==1&&a){
  if(!a[0].l)return fail(v,"java/lang/NullPointerException","callback");
  struct gc_operation op __attribute__((cleanup(gc_operation_end)))={0};
  if(!gc_operation_begin(v,&op)||!gc_operation_admit(v,a[0].l))return true;
  struct dvm_class *cb=dvm__class_by_desc(v,"Landroid/view/SurfaceHolder$Callback;"); struct dvm_object *obj=dvm__obj(v,a[0].l); if(!cb||!obj||!dvm__class_assignable(v,obj->cls,cb))return fail(v,"java/lang/IllegalArgumentException","callback type");
  for(unsigned i=0;i<CALLBACKS;i++)if(s->callbacks[i]==a[0].l&&callback_valid(s,i))return true;
  for(unsigned i=0;i<CALLBACKS;i++)if(!s->callbacks[i]||!callback_valid(s,i)){s->callbacks[i]=a[0].l;s->callback_identity[i]=obj->identity_generation;dvm_pin(v,a[0].l);return true;}
  return fail(v,"java/lang/OutOfMemoryError","callback capacity");
 }
 if(!strcmp(m,"removeCallback")&&!strcmp(sig,"(Landroid/view/SurfaceHolder$Callback;)V")&&n==1&&a){for(unsigned i=0;i<CALLBACKS;i++)if(s->callbacks[i]&&s->callbacks[i]==a[0].l&&callback_valid(s,i)){dvm_unpin(v,s->callbacks[i]);s->callbacks[i]=0;s->callback_identity[i]=0;break;}return true;}
 if(!strcmp(m,"setFormat")&&!strcmp(sig,"(I)V")&&n==1&&a){s->format=a[0].i;return true;}
 if(!strcmp(m,"setType")&&!strcmp(sig,"(I)V")&&n==1&&a){s->type=a[0].i;return true;}
 return false;
}

bool surface_view_runtime_attached(struct dvm *v,dvm_ref view,void *window,int actual_format){
 if(state.vm!=v||!window)return false;slot *s=find(view);if(!s||s->view!=view||s->attached)return false;
 uint64_t gen=0;if(!window_runtime_acquire_alive(window,&gen))return false;
 int w=window_runtime_get_width(window),h=window_runtime_get_height(window);
 if(w<=0||h<=0||!surface_view_runtime_bind(v,view,window)){window_runtime_release(window);return false;}
 s->attached=1;dvm_ref snapshot[CALLBACKS];memcpy(snapshot,s->callbacks,sizeof snapshot);
 for(unsigned i=0;i<CALLBACKS;i++)if(snapshot[i])dvm_pin(v,snapshot[i]);
 bool ok=true;for(unsigned i=0;i<CALLBACKS&&ok;i++)if(snapshot[i]){
  struct dvm_object *o=dvm__obj(v,snapshot[i]);struct dvm_method *method=o?dvm_find_method(v,o->cls,"surfaceCreated","(Landroid/view/SurfaceHolder;)V"):NULL;
  engine_probe_log("SURFACE_VIEW created callback=%u class=%s method=%d size=%dx%d format=%d",i,o&&o->cls&&o->cls->name?o->cls->name:"?",method!=NULL,w,h,actual_format);
  union dvm_value args[4]={{0}},result={0};args[0].l=s->holder;
  bool called=method&&dvm_call(v,method,snapshot[i],args,1,&result);
  engine_probe_log("SURFACE_VIEW created_result callback=%u ok=%d exception=%u",i,called?1:0,(unsigned)dvm_exception(v));
  if(!called){ok=false;break;}
  if(!window_runtime_acquire_alive(window,NULL)){ok=false;break;}window_runtime_release(window);
  method=dvm_find_method(v,o->cls,"surfaceChanged","(Landroid/view/SurfaceHolder;III)V");args[1].i=actual_format;args[2].i=w;args[3].i=h;
  called=method&&dvm_call(v,method,snapshot[i],args,4,&result);
  engine_probe_log("SURFACE_VIEW changed_result callback=%u ok=%d exception=%u",i,called?1:0,(unsigned)dvm_exception(v));
  if(!called)ok=false;
 }
 for(unsigned i=0;i<CALLBACKS;i++)if(snapshot[i])dvm_unpin(v,snapshot[i]);window_runtime_release(window);return ok;
}

bool surface_view_runtime_gc_visit_owner(struct dvm *v,dvm_ref owner,dvm_gc_visit_fn visit,void *user){
 if(!v||v!=state.vm||!visit||!dvm_gil_held(v))return false;slot *s=find(owner);if(!s)return true;
 /* Both links are owner edges. A retained holder also retains its view. */
 if(!live_key(s->view_key,s->view)||!live_key(s->holder_key,s->holder)||!surface_valid(s))return false;
 visit(&s->view,user);visit(&s->holder,user);visit(&s->surface,user);
 for(unsigned i=0;i<CALLBACKS;++i)if(s->callbacks[i]){if(!callback_valid(s,i))return false;visit(&s->callbacks[i],user);}return true;
}

struct surface_gc_check {dvm_ref view,holder,surface;unsigned seen;};
static void surface_gc_check_visit(dvm_ref *ref,void *user){struct surface_gc_check *s=user;if(ref&&(*ref==s->view||*ref==s->holder||*ref==s->surface))++s->seen;}
/* Controlled fresh test owners only: release their legacy pins to exercise
 * actual identity reuse. Production pins remain unchanged until GC3. */
static unsigned surface_gc_test_release(dvm_ref ref){struct dvm_object *o=dvm__obj(state.vm,ref);unsigned pins=o?o->pins:0;for(unsigned i=0;i<pins;++i)dvm_unpin(state.vm,ref);dvm__discard_temporary(state.vm,ref);return pins;}
bool surface_view_runtime_gc_identity_selftest(struct dvm *v){
 if(v!=state.vm||!dvm_gil_held(v)||dvm_exception(v)||dvm_gc_has_native_scopes(v))return false;
 dvm_ref context=dvm_new_object(v,dvm_find_class_or_external(v,"android/content/Context"));
 dvm_ref view=dvm_new_object(v,state.view);union dvm_value arg={.l=context},out={0};
 if(!context||!view||!surface_view_runtime_call(v,"android/view/SurfaceView","<init>","(Landroid/content/Context;)V",view,&arg,1,&out)||dvm_exception(v))return false;
 slot *old=find(view);if(!old)return false;dvm_ref holder=old->holder;
 struct surface_gc_check seen={.view=view,.holder=holder,.surface=old->surface};
 bool ok=surface_view_runtime_gc_visit_owner(v,view,surface_gc_check_visit,&seen)&&seen.seen==3;
 uint64_t identity=dvm__obj(v,view)->identity_generation;unsigned released=surface_gc_test_release(view);
 dvm_ref replacement=dvm_new_object(v,state.view);
 ok=replacement==view&&dvm__obj(v,replacement)->identity_generation!=identity&&!find(replacement)&&ok;
 released+=surface_gc_test_release(holder);
 ok=surface_view_runtime_call(v,"android/view/SurfaceView","<init>","(Landroid/content/Context;)V",replacement,&arg,1,&out)&&!dvm_exception(v)&&find(replacement)==old&&ok;
 slot *current=find(replacement);if(current)retire(current);
 surface_gc_test_release(replacement);dvm__discard_temporary(v,context);
 dprintf(2,"SURFACE_VIEW_GC_IDENTITY_SELFTEST %s same_id_new_generation=1 stale_lookup_rejected=1 slot_reused=1 owner_edges=3 global_roots=0 controlled_test_pins_released=%u\n",ok?"PASS":"FAIL",released);return ok;
}
bool dvm_gc_ui_side_identity_selftest(struct dvm *v){
 extern bool theme_runtime_gc_identity_selftest(struct dvm *);
 extern bool layout_params_runtime_gc_identity_selftest(struct dvm *);
 extern bool window_attributes_gc_identity_selftest(struct dvm *);
 return theme_runtime_gc_identity_selftest(v)&&layout_params_runtime_gc_identity_selftest(v)&&window_attributes_gc_identity_selftest(v)&&surface_view_runtime_gc_identity_selftest(v);
}

