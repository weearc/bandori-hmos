#include "thread_extras.h"
#include "gc_runtime.h"
#include <stdio.h>
#include <string.h>
extern int sdk_trace_fprintf(FILE*,const char*,...);

/* The executor owns failure reporting. This side table only preserves the
 * handler reference a router ThreadFactory installs on its new Thread. */
#define MAX_THREAD_EXTRAS 64
typedef struct {dvm_ref thread,handler;dvm_gc_handle owner;} thread_extra;
static struct dvm *active;
static thread_extra extras[MAX_THREAD_EXTRAS];
static int extras_used;
/* Thread.setDefaultUncaughtExceptionHandler is process-wide in AOSP; this is
 * the VM-lifetime slot for it.  thread_extras_vm_start resets it per VM. */
static dvm_ref default_handler;
static dvm_gc_handle default_handler_root;
void thread_extras_vm_end(struct dvm *v){
 if(active!=v)return;
 if(default_handler_root)dvm_gc_handle_drop(v,default_handler_root);
 for(int i=0;i<extras_used;++i)if(extras[i].owner)dvm_gc_handle_drop(v,extras[i].owner);
 memset(extras,0,sizeof extras);extras_used=0;default_handler=0;default_handler_root=0;active=NULL;
}
void thread_extras_vm_start(struct dvm *v){if(active==v)thread_extras_vm_end(v);active=v;extras_used=0;default_handler=0;default_handler_root=0;memset(extras,0,sizeof extras);}
static int find_extra(struct dvm *v,dvm_ref thread){for(int i=0;i<extras_used;i++)if(extras[i].thread==thread&&dvm_gc_handle_ref(v,extras[i].owner)==thread)return i;return -1;}
static bool store_handler(struct dvm *v,dvm_ref thread,dvm_ref handler){
 int i=find_extra(v,thread);if(i>=0){extras[i].handler=handler;return true;}
 for(i=0;i<extras_used;++i)if(!dvm_gc_handle_ref(v,extras[i].owner)){
  dvm_gc_handle owner=dvm_gc_handle_new(v,thread,DVM_GC_HANDLE_WEAK);if(!owner)return false;
  if(extras[i].owner)dvm_gc_handle_drop(v,extras[i].owner);
  extras[i]=(thread_extra){.thread=thread,.handler=handler,.owner=owner};return true;
 }
 if(extras_used>=MAX_THREAD_EXTRAS){dvm__throw(v,"java/lang/UnsupportedOperationException","Thread handler table full");return false;}
 dvm_gc_handle owner=dvm_gc_handle_new(v,thread,DVM_GC_HANDLE_WEAK);if(!owner)return false;
 extras[extras_used]=(thread_extra){.thread=thread,.handler=handler,.owner=owner};extras_used++;return true;
}
/* Conditional owner edge, deliberately absent from global strong roots. */
bool thread_extras_gc_visit_owner(struct dvm *v,dvm_ref owner,dvm_gc_visit_fn visit,void *user){
 if(v!=active||!visit||!dvm_gil_held(v))return false;int i=find_extra(v,owner);
 if(i>=0&&extras[i].handler){if(!dvm__obj(v,extras[i].handler))return false;visit(&extras[i].handler,user);}return true;
}
#define IS(a,b) (!strcmp(m,a)&&!strcmp(sig,b))
bool thread_extras_call(struct dvm *v,const char *c,const char *m,const char *sig,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 if(v!=active||strcmp(c,"java/lang/Thread"))return false;
 /* Static methods: no receiver.  Bugly asks for the default handler. */
 if(IS("getDefaultUncaughtExceptionHandler","()Ljava/lang/Thread$UncaughtExceptionHandler;")&&n==0){
  out->l=default_handler_root?dvm_gc_handle_ref(v,default_handler_root):0;return true;
 }
 if(IS("setDefaultUncaughtExceptionHandler","(Ljava/lang/Thread$UncaughtExceptionHandler;)V")&&n==1){
  dvm_gc_handle root=a[0].l?dvm_gc_handle_new(v,a[0].l,DVM_GC_HANDLE_STRONG):0;
  if(a[0].l&&!root)return true;if(default_handler_root)dvm_gc_handle_drop(v,default_handler_root);
  default_handler=a[0].l;default_handler_root=root;return true;
 }
 if(!self)return false;
 if(IS("setUncaughtExceptionHandler","(Ljava/lang/Thread$UncaughtExceptionHandler;)V")&&n==1){
  if(!store_handler(v,self,a[0].l))return true;return true;
 }
 if(IS("getUncaughtExceptionHandler","()Ljava/lang/Thread$UncaughtExceptionHandler;")){
  int i=find_extra(v,self);out->l=i>=0?extras[i].handler:0;return true;
 }
 return false;
}
bool thread_extras_selftest(struct dvm *v){
 dvm_ref thread=dvm_new_object(v,dvm_find_class_or_external(v,"java/lang/Thread"));
 dvm_ref handler=dvm_new_object(v,dvm_find_class_or_external(v,"java/lang/Thread$UncaughtExceptionHandler"));
 if(!thread||!handler)return false;
 union dvm_value a={.l=handler},out={0};
 if(!thread_extras_call(v,"java/lang/Thread","setUncaughtExceptionHandler","(Ljava/lang/Thread$UncaughtExceptionHandler;)V",thread,&a,1,&out))return false;
 if(!thread_extras_call(v,"java/lang/Thread","getUncaughtExceptionHandler","()Ljava/lang/Thread$UncaughtExceptionHandler;",thread,NULL,0,&out)||out.l!=handler)return false;
 a.l=0;
 if(!thread_extras_call(v,"java/lang/Thread","setUncaughtExceptionHandler","(Ljava/lang/Thread$UncaughtExceptionHandler;)V",thread,&a,1,&out))return false;
 if(!thread_extras_call(v,"java/lang/Thread","getUncaughtExceptionHandler","()Ljava/lang/Thread$UncaughtExceptionHandler;",thread,NULL,0,&out)||out.l)return false;
 sdk_trace_fprintf(stderr,"THREAD_EXTRAS_SELFTEST PASS uncaught_handler=1 clear=1\n");
 return true;
}
struct thread_extra_gc_test {dvm_ref wanted;unsigned seen;};
static void thread_extra_gc_test_visit(dvm_ref *slot,void *user){struct thread_extra_gc_test *s=user;if(slot&&*slot==s->wanted)++s->seen;}
bool thread_extras_gc_selftest(struct dvm *v){
 if(v!=active||!dvm_gil_held(v)||dvm_exception(v)||dvm_gc_has_native_scopes(v))return false;
 dvm_ref previous=default_handler_root?dvm_gc_handle_ref(v,default_handler_root):0;
 dvm_gc_handle keep=previous?dvm_gc_handle_new(v,previous,DVM_GC_HANDLE_STRONG):0;if(previous&&!keep)return false;
 dvm_ref owner=dvm_new_object(v,dvm_find_class_or_external(v,"java/lang/Thread"));
 dvm_ref handler=dvm_new_object(v,dvm_find_class_or_external(v,"java/lang/Thread$UncaughtExceptionHandler"));
 union dvm_value arg={.l=handler},out={0};struct thread_extra_gc_test seen={.wanted=handler};
 bool ok=owner&&handler&&thread_extras_call(v,"java/lang/Thread","setDefaultUncaughtExceptionHandler","(Ljava/lang/Thread$UncaughtExceptionHandler;)V",0,&arg,1,&out)&&
    dvm_gc_visit_roots(v,DVM_GC_ROOT_STRONG,thread_extra_gc_test_visit,&seen)&&seen.seen;
 arg.l=previous;ok=thread_extras_call(v,"java/lang/Thread","setDefaultUncaughtExceptionHandler","(Ljava/lang/Thread$UncaughtExceptionHandler;)V",0,&arg,1,&out)&&ok;
 if(keep)dvm_gc_handle_drop(v,keep);
 arg.l=handler;ok=store_handler(v,owner,handler)&&ok;seen.seen=0;
 ok=dvm_gc_visit_roots(v,DVM_GC_ROOT_STRONG,thread_extra_gc_test_visit,&seen)&&!seen.seen&&ok;
 ok=thread_extras_gc_visit_owner(v,owner,thread_extra_gc_test_visit,&seen)&&seen.seen==1&&ok;
 dvm__discard_temporary(v,owner);dvm_ref replacement=dvm_new_object(v,dvm_find_class_or_external(v,"java/lang/Thread"));
 ok=replacement==owner&&thread_extras_call(v,"java/lang/Thread","getUncaughtExceptionHandler","()Ljava/lang/Thread$UncaughtExceptionHandler;",replacement,NULL,0,&out)&&!out.l&&ok;
 if(replacement)dvm__discard_temporary(v,replacement);if(handler)dvm__discard_temporary(v,handler);
 sdk_trace_fprintf(stderr,"THREAD_EXTRAS_GC_SELFTEST %s default_strong_root=1 handler_owner_edge=1 key_slot_reuse=1 default_restored=1\n",ok?"PASS":"FAIL");return ok;
}
