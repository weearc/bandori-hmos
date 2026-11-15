#include "theme_runtime.h"
#include "dvm/dvm_internal.h"
#include "gc_runtime.h"
#include <stdio.h>
#include <string.h>
#define CAP 64
#define ATTR 0x01010058
typedef struct {dvm_ref ref;dvm_gc_handle owner;int value;int kind;int recycled;} entry;
static struct {struct dvm *vm;entry e[CAP];} state;
static void retire(entry *e){if(e->owner){dvm_ref live=dvm_gc_handle_ref(state.vm,e->owner);if(live)dvm_unpin(state.vm,live);dvm_gc_handle_drop(state.vm,e->owner);}memset(e,0,sizeof *e);}
static entry *find(dvm_ref ref){if(!ref)return NULL;for(int i=0;i<CAP;i++)if(state.e[i].ref==ref&&dvm_gc_handle_ref(state.vm,state.e[i].owner)==ref)return &state.e[i];return NULL;}
static entry *alloc(void){for(int i=0;i<CAP;i++)if(!state.e[i].ref||!dvm_gc_handle_ref(state.vm,state.e[i].owner)){retire(&state.e[i]);return &state.e[i];}return NULL;}
static bool fail(struct dvm *v,const char *type,const char *msg){dvm__throw(v,type,msg);return true;}
bool theme_runtime_vm_start(struct dvm *v){if(!v||(state.vm&&state.vm!=v))return false;state.vm=v;return true;}
void theme_runtime_vm_shutdown(struct dvm *v){if(v!=state.vm)return;for(int i=0;i<CAP;i++)retire(&state.e[i]);memset(&state,0,sizeof state);}
dvm_ref theme_runtime_create(struct dvm *v,int value){if(v!=state.vm||value< -1||value>2)return 0;entry *e=alloc();if(!e)return 0;dvm_ref r=dvm_new_object(v,dvm_find_class_or_external(v,"android/content/res/Resources$Theme"));if(!r)return 0;dvm_gc_handle owner=dvm_gc_handle_new(v,r,DVM_GC_HANDLE_WEAK);if(!owner)return 0;e->ref=r;e->owner=owner;e->kind=1;e->value=value;dvm_pin(v,r);return r;}
dvm_ref theme_runtime_create_unity_theme(struct dvm *v){return theme_runtime_create(v,0);}
bool theme_runtime_call(struct dvm *v,const char *c,const char *m,const char *sig,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 if(!v||v!=state.vm||!c||!m||!sig)return false;entry *e=find(self);if(!e)return false;
 if(e->kind==1&&!strcmp(c,"android/content/res/Resources$Theme")&&!strcmp(m,"obtainStyledAttributes")&&!strcmp(sig,"([I)Landroid/content/res/TypedArray;")){
  if(n!=1||!a||!a[0].l)return fail(v,"java/lang/NullPointerException","attributes");
  struct dvm_object *arr=dvm__obj(v,a[0].l);if(!arr||arr->kind!=DVM_OBJ_ARRAY||arr->elem_kind!='I'||arr->length!=1||!arr->data||((int *)arr->data)[0]!=ATTR)return fail(v,"java/lang/UnsupportedOperationException","theme attributes unsupported");
  if(e->value<0)return fail(v,"java/lang/UnsupportedOperationException","theme attribute unresolved");entry *t=alloc();if(!t)return fail(v,"java/lang/OutOfMemoryError","typed array capacity");
  dvm_ref r=dvm_new_object(v,dvm_find_class_or_external(v,"android/content/res/TypedArray"));if(!r)return fail(v,"java/lang/OutOfMemoryError","typed array");dvm_gc_handle owner=dvm_gc_handle_new(v,r,DVM_GC_HANDLE_WEAK);if(!owner)return true;t->ref=r;t->owner=owner;t->kind=2;t->value=e->value;dvm_pin(v,r);out->l=r;return true;
 }
 if(e->kind!=2||strcmp(c,"android/content/res/TypedArray"))return false;
 if(!strcmp(m,"recycle")&&!strcmp(sig,"()V")){if(e->recycled)return fail(v,"java/lang/IllegalStateException","already recycled");e->recycled=1;return true;}
 if(!strcmp(m,"getBoolean")&&!strcmp(sig,"(IZ)Z")&&n==2&&a&&out){if(e->recycled)return fail(v,"java/lang/IllegalStateException","recycled");if(a[0].i!=0)return fail(v,"java/lang/IndexOutOfBoundsException","attribute index");out->i=e->value==0?!!a[1].i:e->value==2;return true;}
 return false;
}


bool theme_runtime_selftest(struct dvm *v){
 if(!v||v!=state.vm||dvm_exception(v))return false;
 dvm_ref theme=theme_runtime_create_unity_theme(v),arr=dvm_new_array(v,'I',NULL,1);
 struct dvm_object *o=dvm__obj(v,arr);if(!theme||!o||!o->data)return false;
 ((int *)o->data)[0]=ATTR;union dvm_value a[2]={{0}},out={0};a[0].l=arr;
 if(!theme_runtime_call(v,"android/content/res/Resources$Theme","obtainStyledAttributes","([I)Landroid/content/res/TypedArray;",theme,a,1,&out)||!out.l||dvm_exception(v))return false;
 dvm_ref typed=out.l; a[0].i=0;a[1].i=0;
 if(!theme_runtime_call(v,"android/content/res/TypedArray","getBoolean","(IZ)Z",typed,a,2,&out)||out.i||dvm_exception(v))return false;
 a[1].i=1;if(!theme_runtime_call(v,"android/content/res/TypedArray","getBoolean","(IZ)Z",typed,a,2,&out)||out.i!=1||dvm_exception(v))return false;
 if(!theme_runtime_call(v,"android/content/res/TypedArray","recycle","()V",typed,NULL,0,&out)||dvm_exception(v))return false;
 if(!theme_runtime_call(v,"android/content/res/TypedArray","getBoolean","(IZ)Z",typed,a,2,&out)||!dvm_exception(v))return false;dvm_clear_exception(v);
 ((int *)o->data)[0]=ATTR+1;a[0].l=arr;
 if(!theme_runtime_call(v,"android/content/res/Resources$Theme","obtainStyledAttributes","([I)Landroid/content/res/TypedArray;",theme,a,1,&out)||!dvm_exception(v))return false;dvm_clear_exception(v);
 /* Existing theme remains usable after the rejected query. */
 ((int *)o->data)[0]=ATTR;
 if(!theme_runtime_call(v,"android/content/res/Resources$Theme","obtainStyledAttributes","([I)Landroid/content/res/TypedArray;",theme,a,1,&out)||!out.l||dvm_exception(v))return false;
 return true;
}

bool theme_runtime_gc_identity_selftest(struct dvm *v){
 if(v!=state.vm||!dvm_gil_held(v)||dvm_exception(v)||dvm_gc_has_native_scopes(v))return false;
 dvm_ref ref=theme_runtime_create(v,1);entry *old=find(ref);if(!old)return false;
 struct dvm_class *cls=dvm_object_class(v,ref);uint64_t identity=dvm__obj(v,ref)->identity_generation;
 dvm_unpin(v,ref);dvm__discard_temporary(v,ref);dvm_ref replacement=dvm_new_object(v,cls);
 bool ok=replacement==ref&&dvm__obj(v,replacement)->identity_generation!=identity&&!find(replacement);
 entry *reused=alloc();ok=reused==old&&ok;if(replacement)dvm__discard_temporary(v,replacement);
 dprintf(2,"THEME_GC_IDENTITY_SELFTEST %s same_id_new_generation=1 stale_lookup_rejected=1 slot_reused=1 global_roots=0\n",ok?"PASS":"FAIL");return ok;
}
