#include "surface_view_runtime.h"
#include "view_state_runtime.h"
#include "surface_runtime.h"
#include "window_runtime.h"
#include "dvm/dvm_internal.h"
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
typedef struct { dvm_ref view,holder,surface,callbacks[CALLBACKS]; int format,type,attached,z_order_on_top; } slot;
static struct {struct dvm *vm;struct dvm_class *view,*holder;slot slots[LIMIT];} state;
static bool fail(struct dvm *v,const char *type,const char *msg){dvm__throw(v,type,msg);return true;}
static slot *find(dvm_ref r){if(!r)return NULL;for(unsigned i=0;i<LIMIT;i++)if(state.slots[i].view==r||state.slots[i].holder==r)return &state.slots[i];return NULL;}
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
 if(state.vm!=v)return;for(unsigned i=0;i<LIMIT;i++){slot *s=&state.slots[i];if(s->surface){surface_runtime_unbind(v,s->surface);dvm_unpin(v,s->surface);}for(unsigned j=0;j<CALLBACKS;j++)if(s->callbacks[j])dvm_unpin(v,s->callbacks[j]);if(s->holder)dvm_unpin(v,s->holder);if(s->view)dvm_unpin(v,s->view);}memset(&state,0,sizeof state);
}
bool surface_view_runtime_bind(struct dvm *v,dvm_ref view,void *window){
 if(state.vm!=v||!window)return false;slot *s=find(view);if(!s||s->view!=view||!s->surface)return false;
 return surface_runtime_bind(v,s->surface,window,0);
}
bool surface_view_runtime_call(struct dvm *v,const char *c,const char *m,const char *sig,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 if(!v||v!=state.vm||!c||!m||!sig)return false;
 if(!strcmp(c,"android/view/SurfaceView")&&!strcmp(m,"<init>")&&!strcmp(sig,"(Landroid/content/Context;)V")){
  if(find(self))return fail(v,"java/lang/IllegalStateException","SurfaceView already constructed");
  slot *s=NULL;for(unsigned i=0;i<LIMIT;i++)if(!state.slots[i].view){s=&state.slots[i];break;}
  if(!s)return fail(v,"java/lang/OutOfMemoryError","SurfaceView capacity");
  if(!view_state_runtime_call(v,"android/view/View",m,sig,self,a,n,out))return false;if(dvm_exception(v))return true;
  dvm_ref holder=dvm_new_object(v,state.holder);if(!holder)return fail(v,"java/lang/OutOfMemoryError","SurfaceHolder allocation");
  dvm_ref surface=surface_runtime_new_surface_unbound(v);if(!surface)return fail(v,"java/lang/OutOfMemoryError","Surface allocation");
  s->view=self;s->holder=holder;s->surface=surface;dvm_pin(v,self);dvm_pin(v,holder);dvm_pin(v,surface);return true;
 }
 slot *s=find(self);if(!s)return false;
 if(self==s->view&&!strcmp(m,"setZOrderOnTop")&&!strcmp(sig,"(Z)V")&&a&&n==1){if(s->attached)return fail(v,"java/lang/UnsupportedOperationException","attached z-order update needs host compositor");s->z_order_on_top=a[0].i!=0;return true;}
 if(!strcmp(c,"android/view/SurfaceView")&&!strcmp(m,"getHolder")&&!strcmp(sig,"()Landroid/view/SurfaceHolder;")&&self==s->view){out->l=s->holder;return true;}
 if(self!=s->holder||strcmp(c,"android/view/SurfaceHolder"))return false;
 if(!strcmp(m,"getSurface")&&!strcmp(sig,"()Landroid/view/Surface;")){out->l=s->surface;return true;}
 if(!strcmp(m,"addCallback")&&!strcmp(sig,"(Landroid/view/SurfaceHolder$Callback;)V")&&n==1&&a){
  if(!a[0].l)return fail(v,"java/lang/NullPointerException","callback");
  struct dvm_class *cb=dvm__class_by_desc(v,"Landroid/view/SurfaceHolder$Callback;"); struct dvm_object *obj=dvm__obj(v,a[0].l); if(!cb||!obj||!dvm__class_assignable(v,obj->cls,cb))return fail(v,"java/lang/IllegalArgumentException","callback type");
  for(unsigned i=0;i<CALLBACKS;i++)if(s->callbacks[i]==a[0].l)return true;
  for(unsigned i=0;i<CALLBACKS;i++)if(!s->callbacks[i]){s->callbacks[i]=a[0].l;dvm_pin(v,a[0].l);return true;}
  return fail(v,"java/lang/OutOfMemoryError","callback capacity");
 }
 if(!strcmp(m,"removeCallback")&&!strcmp(sig,"(Landroid/view/SurfaceHolder$Callback;)V")&&n==1&&a){for(unsigned i=0;i<CALLBACKS;i++)if(s->callbacks[i]&&s->callbacks[i]==a[0].l){dvm_unpin(v,s->callbacks[i]);s->callbacks[i]=0;break;}return true;}
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

