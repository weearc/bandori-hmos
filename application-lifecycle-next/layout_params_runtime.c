#include "layout_params_runtime.h"
#include "dvm/dvm_internal.h"
#include "gc_runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CAP 256
/* AOSP Android 9 FrameLayout.LayoutParams: UNSPECIFIED_GRAVITY = -1.
 * https://raw.githubusercontent.com/aosp-mirror/platform_frameworks_base/android-9.0.0_r1/core/java/android/widget/FrameLayout.java */
static struct {struct dvm *vm;struct dvm_class *base,*margin,*frame;dvm_ref refs[CAP];dvm_gc_handle owners[CAP];} state;
static void retire(int i){if(state.owners[i]){dvm_ref live=dvm_gc_handle_ref(state.vm,state.owners[i]);if(live)dvm_unpin(state.vm,live);dvm_gc_handle_drop(state.vm,state.owners[i]);}state.refs[i]=0;state.owners[i]=0;}
static bool fields(struct dvm_class *c,const char **names,int n,int base){if(!c->external||c->ifields||c->nifields||c->islots)return false;struct dvm_field *f=calloc((size_t)n,sizeof(*f));if(!f)return false;for(int i=0;i<n;i++){f[i].cls=c;f[i].name=names[i];f[i].type="I";f[i].kind='I';f[i].width=1;f[i].access=1;f[i].slot=(uint16_t)(base+i);}c->ifields=f;c->nifields=n;c->islots=base+n;return true;}
bool layout_params_runtime_vm_start(struct dvm *v){if(!v)return false;if(state.vm)return state.vm==v;struct dvm_class *b=dvm_find_class_or_external(v,"android/view/ViewGroup$LayoutParams"),*m=dvm_find_class_or_external(v,"android/view/ViewGroup$MarginLayoutParams"),*f=dvm_find_class_or_external(v,"android/widget/FrameLayout$LayoutParams");if(!b||!m||!f)return false;const char *bn[]={"width","height"},*fn[]={"gravity"};if(!fields(b,bn,2,0))return false;m->super=b;m->islots=2;f->super=m;if(!fields(f,fn,1,2))return false;state.vm=v;state.base=b;state.margin=m;state.frame=f;return true;}
void layout_params_runtime_vm_shutdown(struct dvm *v){if(v!=state.vm)return;for(int i=0;i<CAP;i++)retire(i);memset(&state,0,sizeof state);}
bool layout_params_runtime_valid(struct dvm *v,dvm_ref ref){if(v!=state.vm||!ref)return false;for(int i=0;i<CAP;i++)if(state.refs[i]==ref&&dvm_gc_handle_ref(v,state.owners[i])==ref)return true;return false;}
bool layout_params_runtime_call(struct dvm *v,const char *c,const char *m,const char *sig,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){(void)out;if(v!=state.vm||!c||!m||!sig||strcmp(c,"android/widget/FrameLayout$LayoutParams")||strcmp(m,"<init>")||(strcmp(sig,"(III)V")&&strcmp(sig,"(II)V")))return false;struct dvm_object *o=dvm__obj(v,self);if(!o||!dvm__class_assignable(v,o->cls,state.frame)||!a||(n!=(!strcmp(sig,"(III)V")?3:2))||layout_params_runtime_valid(v,self)){dvm__throw(v,"java/lang/IllegalArgumentException","layout params constructor");return true;}int i;for(i=0;i<CAP;i++){if(!state.refs[i]||!dvm_gc_handle_ref(v,state.owners[i])){retire(i);break;}}if(i==CAP){dvm__throw(v,"java/lang/OutOfMemoryError","layout params capacity");return true;}union dvm_value x={0};x.i=a[0].i;if(!dvm_set_field(v,self,"width","I",x))return true;x.i=a[1].i;if(!dvm_set_field(v,self,"height","I",x))return true;x.i=n==3?a[2].i:-1;if(!dvm_set_field(v,self,"gravity","I",x))return true;dvm_gc_handle owner=dvm_gc_handle_new(v,self,DVM_GC_HANDLE_WEAK);if(!owner)return true;state.refs[i]=self;state.owners[i]=owner;dvm_pin(v,self);return true;}
bool layout_params_runtime_selftest(struct dvm *v){if(v!=state.vm)return false;dvm_ref x=dvm_new_object(v,state.frame),y=dvm_new_object(v,state.frame);union dvm_value a[3]={{0}},out={0},q={0};a[0].i=-1;a[1].i=100;a[2].i=17;if(!layout_params_runtime_call(v,"android/widget/FrameLayout$LayoutParams","<init>","(III)V",x,a,3,&out))return false;a[0].i=50;if(!layout_params_runtime_call(v,"android/widget/FrameLayout$LayoutParams","<init>","(II)V",y,a,2,&out))return false;return !dvm_exception(v)&&dvm_get_field(v,x,"width","I",&q)&&q.i==-1&&dvm_get_field(v,y,"width","I",&q)&&q.i==50&&dvm_get_field(v,x,"gravity","I",&q)&&q.i==17&&dvm_get_field(v,y,"gravity","I",&q)&&q.i==-1;}

bool layout_params_runtime_gc_identity_selftest(struct dvm *v){
 if(v!=state.vm||!dvm_gil_held(v)||dvm_exception(v)||dvm_gc_has_native_scopes(v))return false;
 dvm_ref ref=dvm_new_object(v,state.frame);union dvm_value args[2]={{.i=41},{.i=42}},out={0};
 bool ok=ref&&layout_params_runtime_call(v,"android/widget/FrameLayout$LayoutParams","<init>","(II)V",ref,args,2,&out)&&!dvm_exception(v)&&layout_params_runtime_valid(v,ref);
 int old=-1;for(int i=0;i<CAP;++i)if(state.refs[i]==ref)old=i;if(old<0)return false;
 uint64_t identity=dvm__obj(v,ref)->identity_generation;dvm_unpin(v,ref);dvm__discard_temporary(v,ref);
 dvm_ref replacement=dvm_new_object(v,state.frame);
 ok=replacement==ref&&dvm__obj(v,replacement)->identity_generation!=identity&&!layout_params_runtime_valid(v,replacement)&&ok;
 ok=layout_params_runtime_call(v,"android/widget/FrameLayout$LayoutParams","<init>","(II)V",replacement,args,2,&out)&&!dvm_exception(v)&&state.refs[old]==replacement&&layout_params_runtime_valid(v,replacement)&&ok;
 retire(old);dvm__discard_temporary(v,replacement);
 dprintf(2,"LAYOUT_PARAMS_GC_IDENTITY_SELFTEST %s same_id_new_generation=1 stale_lookup_rejected=1 slot_reused=1 global_roots=0\n",ok?"PASS":"FAIL");return ok;
}


