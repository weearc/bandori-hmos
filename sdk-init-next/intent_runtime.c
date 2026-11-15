#include "intent_runtime.h"
#include "uri_runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern int sdk_trace_fprintf(FILE*,const char*,...);
extern void sdk_trace_missing_api(const char*,const char*,const char*);
extern bool activity_runtime_launch_guest(struct dvm *,dvm_ref,struct dvm_class *,dvm_ref,int);
/* Owned by the Bundle runtime. Bundle objects stay owned by the VM arena; the
 * Intent payload only keeps an unpinned reference, like every other module. */
extern dvm_ref bundle_runtime_new(struct dvm *v);
extern bool bundle_runtime_call(struct dvm *v,const char *cls,const char *method,
 const char *sig,dvm_ref self,const union dvm_value *args,int nargs,
 union dvm_value *out);
extern bool set_runtime_call(struct dvm *,const char *,const char *,const char *,dvm_ref,const union dvm_value *,int,union dvm_value *);

/* Optional host launch bridge. Installed by the embedder before guest code
 * runs; a NULL hook keeps every launch method fail-closed. The hook is called
 * synchronously and must not retain the unpinned Intent reference. */
static int (*launch_host)(void*,dvm_ref);
static void *launch_host_user;
static intent_launch_host_v2_fn launch_host_v2;
static void *launch_host_v2_user;
__attribute__((visibility("default"))) void intent_runtime_set_launch_host_v2(
 intent_launch_host_v2_fn launch,void *user){
 launch_host_v2=launch;launch_host_v2_user=launch?user:NULL;
}
__attribute__((visibility("default"))) void intent_runtime_clear_launch_host_v2(void){
 launch_host_v2=NULL;launch_host_v2_user=NULL;
}
__attribute__((visibility("default"))) void intent_runtime_set_launch_host(
 int (*launch)(void *user,dvm_ref intent),void *user){
 launch_host=launch;
 launch_host_user=launch?user:NULL;
}
__attribute__((visibility("default"))) void intent_runtime_clear_launch_host(void){
 launch_host=NULL;
 launch_host_user=NULL;
}

/* 'INTR', deliberately different from battery_runtime's 'BINT' payload so the
 * first hook in the dispatch chain can tell the two Intent owners apart. */
#define INTENT_MAGIC 0x494e5452u
typedef struct intent_payload {
 unsigned magic;
 char *action;
 int flags;
 dvm_ref component_class; /* java.lang.Class object, or 0 */
 char *package_name;
 char *class_name;
 dvm_ref extras;          /* android/os/Bundle object, or 0 */
 dvm_ref data;            /* android/net/Uri object owned by uri_runtime, or 0 */
 dvm_ref categories;      /* real HashSet of category Strings, or 0 */
} intent_payload;
static void intent_payload_free(void *ptr){
 intent_payload *p=ptr;if(!p)return;free(p->action);free(p->package_name);free(p->class_name);free(p);
}
static intent_payload *intent_alloc(struct dvm *v,dvm_ref self){
 struct dvm_object *o=dvm__obj(v,self);
 if(!o){dvm__throw(v,"java/lang/NullPointerException","Intent receiver");return NULL;}
 if(o->data){dvm__throw(v,"java/lang/IllegalStateException","Intent already initialised");return NULL;}
 intent_payload *p=calloc(1,sizeof *p);
 if(!p){dvm__throw(v,"java/lang/OutOfMemoryError","Intent payload");return NULL;}
 p->magic=INTENT_MAGIC;o->data=p;o->data_destroy=intent_payload_free;return p;
}
/* NULL can mean either "already threw" (uninitialised) or "not ours" (for
 * example the battery Intent payload). Callers distinguish with dvm_exception. */
static intent_payload *intent_get(struct dvm *v,dvm_ref self){
 struct dvm_object *o=dvm__obj(v,self);
 if(!o){dvm__throw(v,"java/lang/NullPointerException","Intent receiver");return NULL;}
 if(!o->data){dvm__throw(v,"java/lang/IllegalStateException","Intent constructor not called");return NULL;}
 if(o->data_destroy!=intent_payload_free)return NULL;
 intent_payload *p=o->data;
 return p&&p->magic==INTENT_MAGIC?p:NULL;
}
/* Borrow the actual Intent fields for the installed-package resolver. The
 * caller holds the VM lock and must not retain pointers across guest calls
 * which mutate this Intent. No extras are inspected or consumed. */
bool intent_runtime_query_snapshot(struct dvm *v,dvm_ref self,
 const char **action,const char **package_name,const char **class_name,
 dvm_ref *data,dvm_ref *categories){
 if(!action||!package_name||!class_name||!data||!categories)return false;
 intent_payload *p=intent_get(v,self);
 if(!p){if(!dvm_exception(v))dvm__throw(v,"java/lang/IllegalArgumentException","unsupported Intent payload");return false;}
 *action=p->action;*package_name=p->package_name;*class_name=p->class_name;
 *data=p->data;*categories=p->categories;return true;
}
static bool class_has(struct dvm *v,dvm_ref self,const char *name){
 struct dvm_object *o=dvm__obj(v,self);
 for(struct dvm_class *k=o?o->cls:NULL;k;k=k->super)
  if(k->name&&!strcmp(k->name,name))return true;
 return false;
}
static struct dvm_class *class_arg(struct dvm *v,dvm_ref ref){
 struct dvm_object *o=dvm__obj(v,ref);
 if(!o||o->kind!=DVM_OBJ_CLASS||!o->klass){
  dvm__throw(v,"java/lang/IllegalArgumentException","Intent component is not a Class");return NULL;
 }
 return o->klass;
}
static char *string_copy(struct dvm *v,const char *s){
 if(!s)return NULL;
 char *copy=strdup(s);
 if(!copy)dvm__throw(v,"java/lang/OutOfMemoryError","Intent string");
 return copy;
}
static bool store_string(struct dvm *v,char **slot,const char *value){
 char *copy=string_copy(v,value);
 if(value&&!copy)return false;
 free(*slot);*slot=copy;return true;
}
static char *dotted_class_name(struct dvm *v,const char *name){
 char *copy=string_copy(v,name);
 if(copy)for(char *p=copy;*p;++p)if(*p=='/')*p='.';
 return copy;
}
/* Best effort: the Context facade may not register getPackageName() as a VM
 * method yet. A missing method is not an error; a thrown guest exception is
 * propagated to the caller. */
static char *context_package_name(struct dvm *v,dvm_ref context){
 struct dvm_object *o=dvm__obj(v,context);
 struct dvm_method *m=o?dvm_find_method(v,o->cls,"getPackageName","()Ljava/lang/String;"):NULL;
 if(!m||(!m->has_code&&!m->builtin))return NULL;
 union dvm_value result={0};
 if(!dvm_call(v,m,context,NULL,0,&result))return NULL;
 const char *name=result.l?dvm_string_utf8(v,result.l):NULL;
 return string_copy(v,name);
}
/* Android resolves a leading '.' or a class name without a package against the
 * package from the Context. When the package is unknown the raw name is kept. */
static char *qualify_class(struct dvm *v,const char *package_name,const char *class_name){
 if(!class_name)return NULL;
 size_t plen=package_name?strlen(package_name):0;
 size_t clen=strlen(class_name);
 bool leading=class_name[0]=='.';
 bool no_dot=strchr(class_name,'.')==NULL;
 if(!plen||(!leading&&!no_dot))return string_copy(v,class_name);
 size_t separator=leading?0:1; /* ".Foo" keeps its dot; "Foo" gains one */
 char *result=malloc(plen+separator+clen+1);
 if(!result){dvm__throw(v,"java/lang/OutOfMemoryError","Intent component name");return NULL;}
 memcpy(result,package_name,plen);
 if(separator)result[plen]='.';
 memcpy(result+plen+separator,class_name,clen+1);
 return result;
}
static void bundle_unavailable(struct dvm *v,const char *method,const char *sig){
 sdk_trace_missing_api("android/os/Bundle",method,sig);
 dvm__throw(v,"java/lang/UnsupportedOperationException","Bundle.%s%s unavailable",method,sig);
}
static bool bundle_bool(struct dvm *v,dvm_ref bundle,const char *method,const char *sig,
 const union dvm_value *args,int nargs,union dvm_value *out){
 if(bundle_runtime_call(v,"android/os/Bundle",method,sig,bundle,args,nargs,out))return true;
 if(!dvm_exception(v))bundle_unavailable(v,method,sig);
 return false;
}
#define IS(method,signature) (!strcmp(m,method)&&!strcmp(sig,signature))
static bool intent_constructor(struct dvm *v,const char *m,const char *sig,dvm_ref self,
 const union dvm_value *a,int n,union dvm_value *out){
 (void)out;
 if(IS("<init>","()V")&&n==0)return intent_alloc(v,self)!=NULL||dvm_exception(v)!=0;
 if(IS("<init>","(Landroid/content/Context;Ljava/lang/Class;)V")&&n==2){
  if(!a[0].l){dvm__throw(v,"java/lang/NullPointerException","Intent context");return true;}
  if(!a[1].l){dvm__throw(v,"java/lang/NullPointerException","Intent class");return true;}
  intent_payload *p=intent_alloc(v,self);if(!p)return true;
  struct dvm_class *component=class_arg(v,a[1].l);if(!component)return true;
  char *name=dotted_class_name(v,component->name);if(!name)return true;
  char *package_name=context_package_name(v,a[0].l);
  if(dvm_exception(v)){free(package_name);free(name);return true;}
  p->component_class=a[1].l;p->package_name=package_name;p->class_name=name;
  return true;
 }
 if((IS("<init>","(Ljava/lang/String;)V")&&n==1)||
    (IS("<init>","(Ljava/lang/String;Landroid/net/Uri;)V")&&n==2)){
  const char *action=NULL;
  if(a[0].l){action=dvm_string_utf8(v,a[0].l);if(!action){dvm__throw(v,"java/lang/IllegalArgumentException","Intent action must be a String");return true;}}
  if(n==2&&a[1].l&&!uri_runtime_is_uri(v,a[1].l)){dvm__throw(v,"java/lang/IllegalArgumentException","Intent data must be a Uri");return true;}
  intent_payload *p=intent_alloc(v,self);if(!p)return true;
  store_string(v,&p->action,action);
  if(n==2)p->data=a[1].l;
  return true;
 }
 return false;
}
static bool launch_owner(const char *cls){
 return !strcmp(cls,"android/content/Context")||!strcmp(cls,"android/content/ContextWrapper")||
  !strcmp(cls,"android/app/Application")||!strcmp(cls,"android/app/Activity");
}
static bool launch_receiver(struct dvm *v,dvm_ref self,bool activity){
 const char *base=activity?"android/app/Activity":"android/content/Context";
 if(class_has(v,self,base))return true;
 dvm__throw(v,"java/lang/IllegalArgumentException","invalid %s receiver",activity?"Activity":"Context");
 return false;
}
static bool launch_denied(struct dvm *v,const char *cls,const char *m,const char *sig){
 sdk_trace_missing_api(cls,m,sig);
 dvm__throw(v,"java/lang/UnsupportedOperationException","host activity launch bridge unavailable");
 return true;
}
/* Transfer an already validated launch request to the host. A 0 return only
 * acknowledges the hand-off; the runtime deliberately has no evidence that a
 * page started, so no result is fabricated for the guest. */
static bool launch_request(struct dvm *v,const char *cls,const char *m,const char *sig,
 dvm_ref caller,dvm_ref intent,uint32_t operation,int32_t request_code,
 int32_t enter_animation,int32_t exit_animation){
 intent_payload *payload=NULL;
 if(intent){
  payload=intent_get(v,intent);
  if(!payload){
   if(!dvm_exception(v))dvm__throw(v,"java/lang/IllegalArgumentException","foreign Intent payload");
   return true;
  }
 }
 if(payload&&(operation==INTENT_LAUNCH_START_FOR_RESULT||operation==INTENT_LAUNCH_START)&&payload->class_name){
  char *name=strdup(payload->class_name);
  if(!name){dvm__throw(v,"java/lang/OutOfMemoryError","Activity component");return true;}
  for(char *p=name;*p;p++)if(*p=='.')*p='/';
  struct dvm_class *target=dvm_find_class(v,name);free(name);
  /* Only a real VM class can be executed. External host targets keep the
   * established host observer/fail-closed path. No SDK class special cases. */
  if(target&&!target->external){
   extern void login_panel_required(const char *) __attribute__((weak));
   if(login_panel_required)login_panel_required(payload->class_name);
   extern bool login_panel_uses_arkts(const char *) __attribute__((weak));
   if(login_panel_uses_arkts&&login_panel_uses_arkts(payload->class_name)){
    FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/ui-correct-policy.txt","a");if(f){fprintf(f,"LOGIN_UI_ARKTS_REQUEST class=%s request=%d original_layout_inflated=0\n",payload->class_name,request_code);fclose(f);}return true;
   }
   FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt","a");
   if(f){fprintf(f,"ACTIVITY_GUEST_INTENT component=%s action=%s extras=%u request=%d caller=%u\n",payload->class_name,payload->action?payload->action:"<null>",payload->extras,request_code,caller);fclose(f);}
   if(activity_runtime_launch_guest(v,caller,target,intent,operation==INTENT_LAUNCH_START_FOR_RESULT?request_code:-1))return true;
  }
 }
 if(operation==INTENT_LAUNCH_OVERRIDE_TRANSITION){
  extern bool activity_runtime_record_transition(struct dvm *,dvm_ref,int,int);
  if(!activity_runtime_record_transition(v,caller,enter_animation,exit_animation))return false;
  if(!launch_host_v2&&!launch_host)return true;
 }
 intent_launch_host_v2_fn host2=launch_host_v2;
 if(host2){
  intent_launch_request_v2 request={0};
  request.abi_version=INTENT_LAUNCH_HOST_ABI_VERSION;request.struct_size=(uint32_t)sizeof request;
  request.operation=operation;request.caller_ref=(uint32_t)caller;
  request.intent_ref=(uint32_t)intent;
  if(payload){request.flags=(uint32_t)payload->flags;request.action=payload->action;
   request.package_name=payload->package_name;request.class_name=payload->class_name;}
  request.request_code=request_code;request.enter_animation=enter_animation;
  request.exit_animation=exit_animation;
  int rc=host2(launch_host_v2_user,&request);
  if(operation==INTENT_LAUNCH_OVERRIDE_TRANSITION)return true;
  if(rc==0)return true;
  if(rc==1)dvm__throw(v,"java/lang/UnsupportedOperationException","host activity launch unsupported");
  else dvm__throw(v,"java/lang/RuntimeException","host activity launch request failed (code %d)",rc);
  return true;
 }
 int (*host)(void*,dvm_ref)=launch_host;
 if(!host)return launch_denied(v,cls,m,sig);
 int rc=host(launch_host_user,intent);
 if(operation==INTENT_LAUNCH_OVERRIDE_TRANSITION)return true;
 if(rc==0)return true;
 if(!dvm_exception(v))
  dvm__throw(v,"java/lang/RuntimeException","host activity launch request failed (code %d)",rc);
 return true;
}
static bool intent_argument(struct dvm *v,const union dvm_value *args){
 if(args&&args[0].l&&dvm__obj(v,args[0].l))return true;
 dvm__throw(v,"java/lang/NullPointerException","Intent argument");
 return false;
}
static bool launch_call(struct dvm *v,const char *cls,const char *m,const char *sig,dvm_ref self,
 const union dvm_value *a,int n,union dvm_value *out){
 (void)out;
 bool start_for_result=IS("startActivityForResult","(Landroid/content/Intent;I)V")&&n==2;
 bool start_for_result_bundle=IS("startActivityForResult","(Landroid/content/Intent;ILandroid/os/Bundle;)V")&&n==3;
 bool transition=IS("overridePendingTransition","(II)V")&&n==2;
 bool plain=IS("startActivity","(Landroid/content/Intent;)V")&&n==1;
 bool plain_bundle=IS("startActivity","(Landroid/content/Intent;Landroid/os/Bundle;)V")&&n==2;
 if(!start_for_result&&!start_for_result_bundle&&!transition&&!plain&&!plain_bundle)return false;
 if(!launch_receiver(v,self,start_for_result||start_for_result_bundle||transition))return true;
 bool needs_intent=!transition;
 if(needs_intent&&!intent_argument(v,a))return true;
 dvm_ref intent=needs_intent?a[0].l:0;
 uint32_t operation=transition?INTENT_LAUNCH_OVERRIDE_TRANSITION:
  ((start_for_result||start_for_result_bundle)?INTENT_LAUNCH_START_FOR_RESULT:INTENT_LAUNCH_START);
 int32_t request_code=(start_for_result||start_for_result_bundle)?a[1].i:0;
 int32_t enter=transition?a[0].i:0,exit=transition?a[1].i:0;
 return launch_request(v,cls,m,sig,self,intent,operation,request_code,enter,exit);
}
bool intent_runtime_call(struct dvm *v,const char *c,const char *m,const char *sig,dvm_ref self,
 const union dvm_value *a,int n,union dvm_value *out){
 memset(out,0,sizeof *out);
 if(!strcmp(c,"android/content/Intent")){
  if(intent_constructor(v,m,sig,self,a,n,out))return true;
  if(!strcmp(m,"<init>"))return false;
  intent_payload *p=intent_get(v,self);
  if(!p)return dvm_exception(v)!=0;
  if(IS("setClass","(Landroid/content/Context;Ljava/lang/Class;)Landroid/content/Intent;")&&n==2){
   if(!a[0].l){dvm__throw(v,"java/lang/NullPointerException","Intent context");return true;}
   if(!a[1].l){dvm__throw(v,"java/lang/NullPointerException","Intent class");return true;}
   struct dvm_class *component=class_arg(v,a[1].l);if(!component)return true;
   char *name=dotted_class_name(v,component->name);if(!name)return true;
   char *package_name=context_package_name(v,a[0].l);
   if(dvm_exception(v)){free(package_name);free(name);return true;}
   p->component_class=a[1].l;
   free(p->package_name);p->package_name=package_name;
   free(p->class_name);p->class_name=name;
   out->l=self;return true;
  }
  if(IS("setClassName","(Landroid/content/Context;Ljava/lang/String;)Landroid/content/Intent;")&&n==2){
   if(!a[0].l){dvm__throw(v,"java/lang/NullPointerException","Intent context");return true;}
   const char *name=a[1].l?dvm_string_utf8(v,a[1].l):NULL;
   if(!name){dvm__throw(v,"java/lang/NullPointerException","Intent class name");return true;}
   char *package_name=context_package_name(v,a[0].l);
   if(dvm_exception(v)){free(package_name);return true;}
   char *qualified=qualify_class(v,package_name,name);
   if(!qualified){free(package_name);return true;}
   p->component_class=0;
   free(p->package_name);p->package_name=package_name;
   free(p->class_name);p->class_name=qualified;
   out->l=self;return true;
  }
  if(IS("setClassName","(Ljava/lang/String;Ljava/lang/String;)Landroid/content/Intent;")&&n==2){
   const char *package_name=a[0].l?dvm_string_utf8(v,a[0].l):NULL;
   const char *name=a[1].l?dvm_string_utf8(v,a[1].l):NULL;
   if(!a[0].l||!package_name){dvm__throw(v,"java/lang/NullPointerException","Intent package name");return true;}
   if(!a[1].l||!name){dvm__throw(v,"java/lang/NullPointerException","Intent class name");return true;}
   char *package_copy=string_copy(v,package_name);if(!package_copy)return true;
   char *qualified=qualify_class(v,package_copy,name);
   if(!qualified){free(package_copy);return true;}
   p->component_class=0;
   free(p->package_name);p->package_name=package_copy;
   free(p->class_name);p->class_name=qualified;
   out->l=self;return true;
  }
  if(IS("setAction","(Ljava/lang/String;)Landroid/content/Intent;")&&n==1){
   const char *action=NULL;
   if(a[0].l){action=dvm_string_utf8(v,a[0].l);if(!action){dvm__throw(v,"java/lang/IllegalArgumentException","Intent action must be a String");return true;}}
   if(!store_string(v,&p->action,action))return true;
   out->l=self;return true;
  }
  if(IS("getAction","()Ljava/lang/String;")){
   out->l=p->action?dvm_new_string(v,p->action):0;return true;
  }
  if(IS("getCategories","()Ljava/util/Set;")){out->l=p->categories;return true;}
  if((IS("addCategory","(Ljava/lang/String;)Landroid/content/Intent;")||IS("hasCategory","(Ljava/lang/String;)Z"))&&n==1){
   if(!a[0].l||!dvm_string_utf8(v,a[0].l)){dvm__throw(v,"java/lang/NullPointerException","Intent category");return true;}
   bool adding=!strcmp(m,"addCategory");
   if(!p->categories&&!adding){out->i=0;return true;}
   if(!p->categories){
    p->categories=dvm_new_object(v,dvm_find_class_or_external(v,"java/util/HashSet"));
    union dvm_value ignored={0};
    if(!p->categories||!set_runtime_call(v,"java/util/HashSet","<init>","()V",p->categories,NULL,0,&ignored)||dvm_exception(v))return true;
   }
   if(!set_runtime_call(v,"java/util/HashSet",adding?"add":"contains","(Ljava/lang/Object;)Z",p->categories,a,1,out)||dvm_exception(v))return true;
   if(adding)out->l=self;return true;
  }
  if(IS("setFlags","(I)Landroid/content/Intent;")&&n==1){p->flags=a[0].i;out->l=self;return true;}
  if(IS("addFlags","(I)Landroid/content/Intent;")&&n==1){p->flags|=a[0].i;out->l=self;return true;}
  if(IS("getFlags","()I")){out->i=p->flags;return true;}
  /* Launch data URI.  The reference is an unpinned android/net/Uri owned by
   * uri_runtime.  A plain launcher start has no data, so getData() returns
   * null -- the documented Android result -- and no URI is ever fabricated. */
  if(IS("getData","()Landroid/net/Uri;")){out->l=p->data;return true;}
  if(IS("setData","(Landroid/net/Uri;)Landroid/content/Intent;")&&n==1){
   if(a[0].l&&!uri_runtime_is_uri(v,a[0].l)){dvm__throw(v,"java/lang/IllegalArgumentException","Intent.setData requires an android.net.Uri");return true;}
   p->data=a[0].l;out->l=self;return true;
  }
  if(IS("getDataString","()Ljava/lang/String;")){
   union dvm_value result={0};
   if(!p->data){out->l=0;return true;}
   if(!uri_runtime_call(v,"android/net/Uri","toString","()Ljava/lang/String;",p->data,NULL,0,&result)||dvm_exception(v)){
    if(!dvm_exception(v))dvm__throw(v,"java/lang/IllegalArgumentException","Intent data is not an android.net.Uri");
    return true;
   }
   out->l=result.l;return true;
  }
  if(IS("putExtras","(Landroid/os/Bundle;)Landroid/content/Intent;")&&n==1){
   if(!a[0].l){dvm__throw(v,"java/lang/NullPointerException","Intent extras");return true;}
   if(!class_has(v,a[0].l,"android/os/Bundle")){
    dvm__throw(v,"java/lang/IllegalArgumentException","Intent extras must be a Bundle");return true;
   }
   /* Reference replacement, not a deep merge. Router passes the Postcard's
    * Bundle once, so merge semantics are not needed on this path. */
   p->extras=a[0].l;out->l=self;return true;
  }
  /* AOSP: Intent.putExtra(String,String) lazily allocates mExtras, writes the
   * String through Bundle.putString (a null value is a present null entry; a
   * null key throws NullPointerException), and returns this. The write goes to
   * the same Bundle getExtras()/putExtras() use, not a parallel store. */
  if(IS("putExtra","(Ljava/lang/String;Ljava/lang/String;)Landroid/content/Intent;")&&n==2){
   if(!p->extras){
    p->extras=bundle_runtime_new(v);
    if(!p->extras)return true;
   }
   union dvm_value ignored={0};
   if(!bundle_bool(v,p->extras,"putString","(Ljava/lang/String;Ljava/lang/String;)V",a,2,&ignored))return true;
   if(dvm_exception(v))return true;
   out->l=self;return true;
  }
  /* AOSP Intent.putExtra(String,int) has the same lazy-extras contract; the
   * BLKV DefaultMultiProcessDelegate.sendMsg path asks for this overload. */
  if(IS("putExtra","(Ljava/lang/String;I)Landroid/content/Intent;")&&n==2){
   if(!p->extras){
    p->extras=bundle_runtime_new(v);
    if(!p->extras)return true;
   }
   union dvm_value ignored={0};
   if(!bundle_bool(v,p->extras,"putInt","(Ljava/lang/String;I)V",a,2,&ignored))return true;
   if(dvm_exception(v))return true;
   out->l=self;return true;
  }
  /* AOSP putStringArrayListExtra delegates to Bundle.putStringArrayList on the
   * same extras; BLKV DefaultMultiProcessDelegate.sendMsg asks for it. */
  if(IS("putStringArrayListExtra","(Ljava/lang/String;Ljava/util/ArrayList;)Landroid/content/Intent;")&&n==2){
   if(!p->extras){
    p->extras=bundle_runtime_new(v);
    if(!p->extras)return true;
   }
   union dvm_value ignored={0};
   if(!bundle_bool(v,p->extras,"putStringArrayList","(Ljava/lang/String;Ljava/util/ArrayList;)V",a,2,&ignored))return true;
   if(dvm_exception(v))return true;
   out->l=self;return true;
  }
  if(IS("getExtras","()Landroid/os/Bundle;")){out->l=p->extras;return true;}
  if(IS("hasExtra","(Ljava/lang/String;)Z")&&n==1){
   const char *key=a[0].l?dvm_string_utf8(v,a[0].l):NULL;
   if(!p->extras||!key)return true;
   union dvm_value result={0};
   if(!bundle_bool(v,p->extras,"containsKey","(Ljava/lang/String;)Z",a,1,&result))return true;
   out->i=result.i;return true;
  }
  if(IS("getParcelableExtra","(Ljava/lang/String;)Landroid/os/Parcelable;")&&n==1){
   if(!p->extras)return true;
   bundle_bool(v,p->extras,"getParcelable","(Ljava/lang/String;)Landroid/os/Parcelable;",a,1,out);return true;
  }
  if(IS("getStringExtra","(Ljava/lang/String;)Ljava/lang/String;")&&n==1){
   const char *key=a[0].l?dvm_string_utf8(v,a[0].l):NULL;
   if(!p->extras||!key)return true;
   union dvm_value result={0};
   if(!bundle_bool(v,p->extras,"getString","(Ljava/lang/String;)Ljava/lang/String;",a,1,&result))return true;
   out->l=result.l;return true;
  }
  if(IS("getStringExtra","(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;")&&n==2){
   out->l=a[1].l;
   const char *key=a[0].l?dvm_string_utf8(v,a[0].l):NULL;
   if(!p->extras||!key)return true;
   union dvm_value result={0};
   if(!bundle_bool(v,p->extras,"getString","(Ljava/lang/String;)Ljava/lang/String;",a,1,&result))return true;
   if(result.l)out->l=result.l;
   return true;
  }
  if(IS("getIntExtra","(Ljava/lang/String;I)I")&&n==2){
   out->i=a[1].i;
   const char *key=a[0].l?dvm_string_utf8(v,a[0].l):NULL;
   if(!p->extras||!key)return true;
   union dvm_value result={0};
   if(!bundle_bool(v,p->extras,"getInt","(Ljava/lang/String;I)I",a,2,&result))return true;
   out->i=result.i;return true;
  }
  if(IS("getBooleanExtra","(Ljava/lang/String;Z)Z")&&n==2){
   out->i=a[1].i;
   const char *key=a[0].l?dvm_string_utf8(v,a[0].l):NULL;
   if(!p->extras||!key)return true;
   union dvm_value result={0};
   if(!bundle_bool(v,p->extras,"getBoolean","(Ljava/lang/String;Z)Z",a,2,&result))return true;
   out->i=result.i?1:0;return true;
  }
  if(IS("getLongExtra","(Ljava/lang/String;J)J")&&n==2){
   out->j=a[1].j;
   const char *key=a[0].l?dvm_string_utf8(v,a[0].l):NULL;
   if(!p->extras||!key)return true;
   union dvm_value result={0};
   if(!bundle_bool(v,p->extras,"getLong","(Ljava/lang/String;J)J",a,2,&result))return true;
   out->j=result.j;return true;
  }
  return false;
 }
 /* The DEX method ref for a guest Activity subclass names that subclass
  * (PermissionActivity.startActivity), not android/app/Activity.  Resolve by
  * the actual receiver's Context/Activity chain so the documented
  * fail-closed path runs (MISSING_API + UnsupportedOperationException)
  * instead of falling through to a NoSuchMethodError.  launch_call still
  * validates the receiver and never reports a forged launch. */
 if(launch_owner(c)||class_has(v,self,"android/content/Context"))
  return launch_call(v,c,m,sig,self,a,n,out);
 return false;
}
static bool exception_is(struct dvm *v,const char *name){
 dvm_ref ref=dvm_exception(v);struct dvm_object *o=ref?dvm__obj(v,ref):NULL;
 for(struct dvm_class *k=o?o->cls:NULL;k;k=k->super)
  if(k->name&&!strcmp(k->name,name))return true;
 return false;
}
static bool string_is(struct dvm *v,dvm_ref ref,const char *want){
 const char *text=ref?dvm_string_utf8(v,ref):NULL;
 return text&&!strcmp(text,want);
}
/* Extras are inserted through the Bundle owner, then read back through the
 * Intent accessors, so the round trip crosses the module boundary the same
 * way Router's Postcard path does. */
static bool test_bundle_put(struct dvm *v,dvm_ref bundle,const char *method,
 const char *sig,const union dvm_value *args,int nargs){
 union dvm_value ignored={0};
 return bundle_runtime_call(v,"android/os/Bundle",method,sig,bundle,args,nargs,&ignored)&&
  !dvm_exception(v);
}
/* A leading-dot class name is qualified with the Context package when that
 * package is available; the strict VM has no Context.getPackageName() yet, so
 * both outcomes are legal and the relationship between the stored fields is
 * what the constructor must preserve. */
static bool test_relative_class(const char *package_name,const char *class_name){
 static const char suffix[]=".RelativeActivity";
 if(!class_name)return false;
 if(!package_name)return !strcmp(class_name,suffix);
 size_t plen=strlen(package_name);
 return !strncmp(class_name,package_name,plen)&&!strcmp(class_name+plen,suffix);
}
/* Selftest probe: records the single hand-off and returns the configured code.
 * Casting 'user' back to this struct also proves the user pointer is passed. */
typedef struct launch_probe {
 int calls;
 int result;
 dvm_ref intent;
 intent_launch_request_v2 last;
 char action[96],package_name[96],class_name[128];
} launch_probe;
static int launch_probe_host(void *user,dvm_ref intent){
 launch_probe *probe=user;
 probe->calls++;
 probe->intent=intent;
 return probe->result;
}
static int launch_probe_host_v2(void *user,const intent_launch_request_v2 *request){
 launch_probe *probe=user;probe->calls++;probe->intent=request->intent_ref;
 probe->last=*request;
 snprintf(probe->action,sizeof probe->action,"%s",request->action?request->action:"");
 snprintf(probe->package_name,sizeof probe->package_name,"%s",request->package_name?request->package_name:"");
 snprintf(probe->class_name,sizeof probe->class_name,"%s",request->class_name?request->class_name:"");
 probe->last.action=probe->action;probe->last.package_name=probe->package_name;probe->last.class_name=probe->class_name;
 return probe->result;
}
static bool intent_runtime_selftest_impl(struct dvm *v){
 union dvm_value out={0},ignored={0};
 launch_probe probe={0};
 struct dvm_class *context_class=dvm_find_class_or_external(v,"android/content/Context");
 struct dvm_class *wrapper_class=dvm_find_class_or_external(v,"android/content/ContextWrapper");
 struct dvm_class *application_class=dvm_find_class_or_external(v,"android/app/Application");
 struct dvm_class *activity_class=dvm_find_class_or_external(v,"android/app/Activity");
 struct dvm_class *intent_class=dvm_find_class_or_external(v,"android/content/Intent");
 if(!context_class||!wrapper_class||!application_class||!activity_class||!intent_class)return false;
 if(!wrapper_class->super)wrapper_class->super=context_class;
 if(!application_class->super)application_class->super=wrapper_class;
 if(!activity_class->super)activity_class->super=wrapper_class;
 struct dvm_class *target_class=dvm_find_class_or_external(v,"com/example/IntentTarget");
 dvm_ref target=dvm_class_object(v,target_class);
 dvm_ref context=dvm_new_object(v,context_class);
 dvm_ref activity=dvm_new_object(v,activity_class);
 dvm_ref intent=dvm_new_object(v,intent_class);
 if(!target||!context||!activity||!intent)return false;
 /* Constructors and component/action/flags. */
 if(!intent_runtime_call(v,"android/content/Intent","<init>","()V",intent,NULL,0,&ignored)||dvm_exception(v))return false;
 union dvm_value component[2]={{.l=context},{.l=target}};
 if(!intent_runtime_call(v,"android/content/Intent","setClass","(Landroid/content/Context;Ljava/lang/Class;)Landroid/content/Intent;",intent,component,2,&out))return false;
 if(out.l!=intent||dvm_exception(v))return false;
 intent_payload *first=intent_get(v,intent);
 if(!first||first->component_class!=target||!first->class_name||
    strcmp(first->class_name,"com.example.IntentTarget"))return false;
 union dvm_value action={.l=dvm_new_string(v,"com.example.ACTION")};
 if(!intent_runtime_call(v,"android/content/Intent","setAction","(Ljava/lang/String;)Landroid/content/Intent;",intent,&action,1,&out)||out.l!=intent)return false;
 if(!intent_runtime_call(v,"android/content/Intent","getAction","()Ljava/lang/String;",intent,NULL,0,&out)||!string_is(v,out.l,"com.example.ACTION"))return false;
 union dvm_value flags={.i=0x10000000};
 if(!intent_runtime_call(v,"android/content/Intent","setFlags","(I)Landroid/content/Intent;",intent,&flags,1,&out)||out.l!=intent)return false;
 flags.i=0x00000001;
 if(!intent_runtime_call(v,"android/content/Intent","addFlags","(I)Landroid/content/Intent;",intent,&flags,1,&out)||out.l!=intent)return false;
 if(!intent_runtime_call(v,"android/content/Intent","getFlags","()I",intent,NULL,0,&out)||out.i!=0x10000001)return false;
 /* Extra constructor and both setClassName overloads. */
 dvm_ref named=dvm_new_object(v,intent_class);
 union dvm_value ctor[2]={{.l=context},{.l=target}};
 if(!named||!intent_runtime_call(v,"android/content/Intent","<init>","(Landroid/content/Context;Ljava/lang/Class;)V",named,ctor,2,&ignored)||dvm_exception(v))return false;
 intent_payload *named_payload=intent_get(v,named);
 if(!named_payload||named_payload->component_class!=target||!named_payload->class_name||
    strcmp(named_payload->class_name,"com.example.IntentTarget"))return false;
 union dvm_value class_name[2]={{.l=context},{.l=dvm_new_string(v,".RelativeActivity")}};
 if(!intent_runtime_call(v,"android/content/Intent","setClassName","(Landroid/content/Context;Ljava/lang/String;)Landroid/content/Intent;",named,class_name,2,&out)||out.l!=named||dvm_exception(v))return false;
 named_payload=intent_get(v,named);
 if(!named_payload||named_payload->component_class||
    !test_relative_class(named_payload->package_name,named_payload->class_name))return false;
 union dvm_value package_name[2]={{.l=dvm_new_string(v,"com.example")},{.l=dvm_new_string(v,".AbsoluteActivity")}};
 if(!intent_runtime_call(v,"android/content/Intent","setClassName","(Ljava/lang/String;Ljava/lang/String;)Landroid/content/Intent;",named,package_name,2,&out)||out.l!=named||dvm_exception(v))return false;
 named_payload=intent_get(v,named);
 if(!named_payload||named_payload->component_class||!named_payload->package_name||
    strcmp(named_payload->package_name,"com.example")||!named_payload->class_name||
    strcmp(named_payload->class_name,"com.example.AbsoluteActivity"))return false;
 dvm_ref action_only=dvm_new_object(v,intent_class);
 union dvm_value initial_action={.l=dvm_new_string(v,"com.example.LAUNCH")};
 if(!action_only||!intent_runtime_call(v,"android/content/Intent","<init>","(Ljava/lang/String;)V",action_only,&initial_action,1,&ignored)||dvm_exception(v))return false;
 if(!intent_runtime_call(v,"android/content/Intent","getAction","()Ljava/lang/String;",action_only,NULL,0,&out)||!string_is(v,out.l,"com.example.LAUNCH"))return false;
 /* Launch data URI: null by default (plain launcher start); Uri.parse round
  * trip keeps the exact object; setData(null) clears; a foreign object is
  * rejected instead of being stored as a fake URI. */
 if(!intent_runtime_call(v,"android/content/Intent","getData","()Landroid/net/Uri;",intent,NULL,0,&out)||out.l)return false;
 if(!intent_runtime_call(v,"android/content/Intent","getDataString","()Ljava/lang/String;",intent,NULL,0,&out)||out.l)return false;
 union dvm_value uri_text={.l=dvm_new_string(v,"https://example.com/a?b=c")};
 if(!uri_text.l||!uri_runtime_call(v,"android/net/Uri","parse","(Ljava/lang/String;)Landroid/net/Uri;",0,&uri_text,1,&out)||!out.l)return false;
 dvm_ref data_uri=out.l;
 union dvm_value data_arg={.l=data_uri};
 if(!intent_runtime_call(v,"android/content/Intent","setData","(Landroid/net/Uri;)Landroid/content/Intent;",intent,&data_arg,1,&out)||out.l!=intent||dvm_exception(v))return false;
 memset(&out,0,sizeof out);
 if(!intent_runtime_call(v,"android/content/Intent","getData","()Landroid/net/Uri;",intent,NULL,0,&out)||out.l!=data_uri)return false;
 memset(&out,0,sizeof out);
 if(!intent_runtime_call(v,"android/content/Intent","getDataString","()Ljava/lang/String;",intent,NULL,0,&out)||!string_is(v,out.l,"https://example.com/a?b=c"))return false;
 data_arg.l=0;
 if(!intent_runtime_call(v,"android/content/Intent","setData","(Landroid/net/Uri;)Landroid/content/Intent;",intent,&data_arg,1,&out)||out.l!=intent||dvm_exception(v))return false;
 memset(&out,0,sizeof out);
 if(!intent_runtime_call(v,"android/content/Intent","getData","()Landroid/net/Uri;",intent,NULL,0,&out)||out.l)return false;
 dvm_ref foreign_object=dvm_new_object(v,dvm_find_class_or_external(v,"java/lang/Object"));
 if(!foreign_object)return false;
 data_arg.l=foreign_object;
 if(!intent_runtime_call(v,"android/content/Intent","setData","(Landroid/net/Uri;)Landroid/content/Intent;",intent,&data_arg,1,&out)||!dvm_exception(v))return false;
 dvm_clear_exception(v);
 data_arg.l=data_uri;
 if(!intent_runtime_call(v,"android/content/Intent","setData","(Landroid/net/Uri;)Landroid/content/Intent;",intent,&data_arg,1,&out)||dvm_exception(v))return false;
 /* Extras: save the Bundle reference, then query it through Bundle runtime. */
 dvm_ref bundle=bundle_runtime_new(v);
 if(!bundle)return false;
 union dvm_value extras_arg={.l=bundle};
 if(!intent_runtime_call(v,"android/content/Intent","putExtras","(Landroid/os/Bundle;)Landroid/content/Intent;",intent,&extras_arg,1,&out)||out.l!=intent)return false;
 if(!intent_runtime_call(v,"android/content/Intent","getExtras","()Landroid/os/Bundle;",intent,NULL,0,&out)||out.l!=bundle)return false;
 union dvm_value key={.l=dvm_new_string(v,"missing")};
 if(!intent_runtime_call(v,"android/content/Intent","getParcelableExtra","(Ljava/lang/String;)Landroid/os/Parcelable;",intent,&key,1,&out)||out.l||dvm_exception(v))return false;
 union dvm_value parcel_args[2]={{.l=dvm_new_string(v,"parcel")},{.l=foreign_object}};
 if(!bundle_runtime_call(v,"android/os/Bundle","putParcelable","(Ljava/lang/String;Landroid/os/Parcelable;)V",bundle,parcel_args,2,&out)||dvm_exception(v))return false;
 if(!intent_runtime_call(v,"android/content/Intent","getParcelableExtra","(Ljava/lang/String;)Landroid/os/Parcelable;",intent,parcel_args,1,&out)||out.l!=foreign_object||dvm_exception(v))return false;
 if(!intent_runtime_call(v,"android/content/Intent","hasExtra","(Ljava/lang/String;)Z",intent,&key,1,&out)||out.i)return false;
 union dvm_value int_default[2]={key,{.i=7}};
 if(!intent_runtime_call(v,"android/content/Intent","getIntExtra","(Ljava/lang/String;I)I",intent,int_default,2,&out)||out.i!=7)return false;
 union dvm_value bool_default[2]={key,{.i=1}};
 if(!intent_runtime_call(v,"android/content/Intent","getBooleanExtra","(Ljava/lang/String;Z)Z",intent,bool_default,2,&out)||!out.i)return false;
 union dvm_value long_default[2]={key,{.j=9}};
 if(!intent_runtime_call(v,"android/content/Intent","getLongExtra","(Ljava/lang/String;J)J",intent,long_default,2,&out)||out.j!=9)return false;
 if(!intent_runtime_call(v,"android/content/Intent","getStringExtra","(Ljava/lang/String;)Ljava/lang/String;",intent,&key,1,&out)||out.l)return false;
 dvm_ref fallback=dvm_new_string(v,"fallback");
 union dvm_value string_default[2]={key,{.l=fallback}};
 if(!fallback||!intent_runtime_call(v,"android/content/Intent","getStringExtra","(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",intent,string_default,2,&out)||out.l!=fallback)return false;
 if(dvm_exception(v))return false;
 /* Typed extras round trip: Bundle owns put*, Intent owns the typed getters. */
 union dvm_value text_key={.l=dvm_new_string(v,"text")};
 union dvm_value count_key={.l=dvm_new_string(v,"count")};
 union dvm_value enabled_key={.l=dvm_new_string(v,"enabled")};
 union dvm_value stamp_key={.l=dvm_new_string(v,"stamp")};
 dvm_ref text_value=dvm_new_string(v,"extras-text");
 if(!text_key.l||!count_key.l||!enabled_key.l||!stamp_key.l||!text_value)return false;
 union dvm_value put_text[2]={text_key,{.l=text_value}};
 union dvm_value put_count[2]={count_key,{.i=42}};
 union dvm_value put_enabled[2]={enabled_key,{.i=1}};
 union dvm_value put_stamp[2]={stamp_key,{.j=1234567890123LL}};
 if(!test_bundle_put(v,bundle,"putString","(Ljava/lang/String;Ljava/lang/String;)V",put_text,2)||
    !test_bundle_put(v,bundle,"putInt","(Ljava/lang/String;I)V",put_count,2)||
    !test_bundle_put(v,bundle,"putBoolean","(Ljava/lang/String;Z)V",put_enabled,2)||
    !test_bundle_put(v,bundle,"putLong","(Ljava/lang/String;J)V",put_stamp,2))return false;
 if(!intent_runtime_call(v,"android/content/Intent","getStringExtra","(Ljava/lang/String;)Ljava/lang/String;",intent,&text_key,1,&out)||out.l!=text_value)return false;
 union dvm_value int_roundtrip[2]={count_key,{.i=0}};
 if(!intent_runtime_call(v,"android/content/Intent","getIntExtra","(Ljava/lang/String;I)I",intent,int_roundtrip,2,&out)||out.i!=42)return false;
 union dvm_value bool_roundtrip[2]={enabled_key,{.i=0}};
 if(!intent_runtime_call(v,"android/content/Intent","getBooleanExtra","(Ljava/lang/String;Z)Z",intent,bool_roundtrip,2,&out)||out.i!=1)return false;
 union dvm_value long_roundtrip[2]={stamp_key,{.j=0}};
 if(!intent_runtime_call(v,"android/content/Intent","getLongExtra","(Ljava/lang/String;J)J",intent,long_roundtrip,2,&out)||out.j!=1234567890123LL)return false;
 if(!intent_runtime_call(v,"android/content/Intent","hasExtra","(Ljava/lang/String;)Z",intent,&text_key,1,&out)||!out.i)return false;
 if(!intent_runtime_call(v,"android/content/Intent","hasExtra","(Ljava/lang/String;)Z",intent,&count_key,1,&out)||!out.i)return false;
 if(!intent_runtime_call(v,"android/content/Intent","hasExtra","(Ljava/lang/String;)Z",intent,&enabled_key,1,&out)||!out.i)return false;
 if(!intent_runtime_call(v,"android/content/Intent","hasExtra","(Ljava/lang/String;)Z",intent,&stamp_key,1,&out)||!out.i)return false;
 /* putExtras replaces the reference; it does not merge the two maps. */
 dvm_ref replacement=bundle_runtime_new(v);
 dvm_ref other_value=dvm_new_string(v,"replacement-text");
 union dvm_value other_key={.l=dvm_new_string(v,"other")};
 if(!replacement||!other_value||!other_key.l)return false;
 union dvm_value put_other[2]={other_key,{.l=other_value}};
 if(!test_bundle_put(v,replacement,"putString","(Ljava/lang/String;Ljava/lang/String;)V",put_other,2))return false;
 union dvm_value replace_arg={.l=replacement};
 if(!intent_runtime_call(v,"android/content/Intent","putExtras","(Landroid/os/Bundle;)Landroid/content/Intent;",intent,&replace_arg,1,&out)||out.l!=intent)return false;
 if(!intent_runtime_call(v,"android/content/Intent","getExtras","()Landroid/os/Bundle;",intent,NULL,0,&out)||out.l!=replacement)return false;
 if(!intent_runtime_call(v,"android/content/Intent","hasExtra","(Ljava/lang/String;)Z",intent,&text_key,1,&out)||out.i)return false;
 if(!intent_runtime_call(v,"android/content/Intent","hasExtra","(Ljava/lang/String;)Z",intent,&other_key,1,&out)||!out.i)return false;
 if(!intent_runtime_call(v,"android/content/Intent","getStringExtra","(Ljava/lang/String;)Ljava/lang/String;",intent,&other_key,1,&out)||out.l!=other_value)return false;
 union dvm_value old_default[2]={count_key,{.i=5}};
 if(!intent_runtime_call(v,"android/content/Intent","getIntExtra","(Ljava/lang/String;I)I",intent,old_default,2,&out)||out.i!=5)return false;
 if(dvm_exception(v))return false;
 /* Intent.putExtra(String,String): lazy Bundle creation, the extras Bundle is
  * the same instance getExtras() returns, the value round-trips through
  * getStringExtra, a null value is a present key with a null String, and a
  * null key throws NullPointerException. */
 dvm_ref extra_intent=dvm_new_object(v,intent_class);
 union dvm_value extra_action={.l=dvm_new_string(v,"com.example.PUT_EXTRA")};
 union dvm_value extra_key={.l=dvm_new_string(v,"extra:name")};
 dvm_ref extra_text=dvm_new_string(v,"blkv-name");
 if(!extra_intent||!extra_action.l||!extra_key.l||!extra_text)return false;
 if(!intent_runtime_call(v,"android/content/Intent","<init>","(Ljava/lang/String;)V",extra_intent,&extra_action,1,&ignored)||dvm_exception(v))return false;
 union dvm_value put_extra[2]={extra_key,{.l=extra_text}};
 if(!intent_runtime_call(v,"android/content/Intent","putExtra","(Ljava/lang/String;Ljava/lang/String;)Landroid/content/Intent;",extra_intent,put_extra,2,&out)||out.l!=extra_intent||dvm_exception(v))return false;
 if(!intent_runtime_call(v,"android/content/Intent","getExtras","()Landroid/os/Bundle;",extra_intent,NULL,0,&out)||!out.l)return false;
 dvm_ref extra_bundle=out.l;
 if(!intent_runtime_call(v,"android/content/Intent","getStringExtra","(Ljava/lang/String;)Ljava/lang/String;",extra_intent,&extra_key,1,&out)||out.l!=extra_text)return false;
 if(!intent_runtime_call(v,"android/content/Intent","hasExtra","(Ljava/lang/String;)Z",extra_intent,&extra_key,1,&out)||!out.i)return false;
 /* A second putExtra keeps the first entry on the same Bundle. */
 union dvm_value second_key={.l=dvm_new_string(v,"extra:process")};
 dvm_ref second_text=dvm_new_string(v,"4321");
 if(!second_key.l||!second_text)return false;
 union dvm_value put_second[2]={second_key,{.l=second_text}};
 if(!intent_runtime_call(v,"android/content/Intent","putExtra","(Ljava/lang/String;Ljava/lang/String;)Landroid/content/Intent;",extra_intent,put_second,2,&out)||out.l!=extra_intent||dvm_exception(v))return false;
 if(!intent_runtime_call(v,"android/content/Intent","getExtras","()Landroid/os/Bundle;",extra_intent,NULL,0,&out)||out.l!=extra_bundle)return false;
 if(!intent_runtime_call(v,"android/content/Intent","getStringExtra","(Ljava/lang/String;)Ljava/lang/String;",extra_intent,&extra_key,1,&out)||out.l!=extra_text)return false;
 if(!intent_runtime_call(v,"android/content/Intent","getStringExtra","(Ljava/lang/String;)Ljava/lang/String;",extra_intent,&second_key,1,&out)||out.l!=second_text)return false;
 /* Bundle.putString accepts null: key present, value null. */
 union dvm_value put_null[2]={extra_key,{.l=0}};
 if(!intent_runtime_call(v,"android/content/Intent","putExtra","(Ljava/lang/String;Ljava/lang/String;)Landroid/content/Intent;",extra_intent,put_null,2,&out)||out.l!=extra_intent||dvm_exception(v))return false;
 if(!intent_runtime_call(v,"android/content/Intent","hasExtra","(Ljava/lang/String;)Z",extra_intent,&extra_key,1,&out)||!out.i)return false;
 if(!intent_runtime_call(v,"android/content/Intent","getStringExtra","(Ljava/lang/String;)Ljava/lang/String;",extra_intent,&extra_key,1,&out)||out.l)return false;
 /* A null key is rejected with NullPointerException. */
 union dvm_value null_key[2]={{.l=0},{.l=extra_text}};
 if(!intent_runtime_call(v,"android/content/Intent","putExtra","(Ljava/lang/String;Ljava/lang/String;)Landroid/content/Intent;",extra_intent,null_key,2,&out)||!exception_is(v,"java/lang/NullPointerException"))return false;
 dvm_clear_exception(v);
 if(dvm_exception(v))return false;
 /* Launch methods must fail closed, never report success. */
 union dvm_value launch[2]={{.l=intent},{.l=0}};
 if(!intent_runtime_call(v,"android/content/Context","startActivity","(Landroid/content/Intent;)V",context,launch,1,&ignored)||!exception_is(v,"java/lang/UnsupportedOperationException"))return false;
 dvm_clear_exception(v);
 if(!intent_runtime_call(v,"android/content/Context","startActivity","(Landroid/content/Intent;Landroid/os/Bundle;)V",context,launch,2,&ignored)||!exception_is(v,"java/lang/UnsupportedOperationException"))return false;
 dvm_clear_exception(v);
 union dvm_value result_args[2]={{.l=intent},{.i=3}};
 if(!intent_runtime_call(v,"android/app/Activity","startActivityForResult","(Landroid/content/Intent;I)V",activity,result_args,2,&ignored)||!exception_is(v,"java/lang/UnsupportedOperationException"))return false;
 dvm_clear_exception(v);
 union dvm_value result_bundle[3]={{.l=intent},{.i=3},{.l=0}};
 if(!intent_runtime_call(v,"android/app/Activity","startActivityForResult","(Landroid/content/Intent;ILandroid/os/Bundle;)V",activity,result_bundle,3,&ignored)||!exception_is(v,"java/lang/UnsupportedOperationException"))return false;
 dvm_clear_exception(v);
 union dvm_value transition[2]={{.i=0},{.i=0}};
 if(!intent_runtime_call(v,"android/app/Activity","overridePendingTransition","(II)V",activity,transition,2,&ignored)||dvm_exception(v))return false;
 dvm_clear_exception(v);
 union dvm_value null_intent[1]={{.l=0}};
 if(!intent_runtime_call(v,"android/content/Context","startActivity","(Landroid/content/Intent;)V",context,null_intent,1,&ignored)||!exception_is(v,"java/lang/NullPointerException"))return false;
 dvm_clear_exception(v);
 /* Explicit host bridge: one hand-off with the original non-null Intent and
  * no exception. The bridge is cleared before assertions so a failure cannot
  * leak installed state into the remaining checks. */
 intent_runtime_set_launch_host(launch_probe_host,&probe);
 bool host_requested=intent_runtime_call(v,"android/content/Context","startActivity","(Landroid/content/Intent;)V",context,launch,1,&ignored);
 intent_runtime_clear_launch_host();
 if(!host_requested||dvm_exception(v)||probe.calls!=1||probe.intent!=intent)return false;
 /* Without a hook the same call must fail closed again. */
 if(!intent_runtime_call(v,"android/content/Context","startActivity","(Landroid/content/Intent;)V",context,launch,1,&ignored)||!exception_is(v,"java/lang/UnsupportedOperationException"))return false;
 dvm_clear_exception(v);
 /* A non-zero host code is a real failure, never a reported launch. */
 probe.calls=0;probe.intent=0;probe.result=7;
 intent_runtime_set_launch_host(launch_probe_host,&probe);
 bool host_failed=intent_runtime_call(v,"android/content/Context","startActivity","(Landroid/content/Intent;)V",context,launch,1,&ignored);
 intent_runtime_clear_launch_host();
 if(!host_failed||!exception_is(v,"java/lang/RuntimeException")||probe.calls!=1||probe.intent!=intent)return false;
 dvm_clear_exception(v);
 /* Argument validation runs before the hook: a null Intent never reaches it. */
 probe.calls=0;probe.intent=0;probe.result=0;
 intent_runtime_set_launch_host(launch_probe_host,&probe);
 bool null_rejected=intent_runtime_call(v,"android/content/Context","startActivity","(Landroid/content/Intent;)V",context,null_intent,1,&ignored);
 intent_runtime_clear_launch_host();
 if(!null_rejected||!exception_is(v,"java/lang/NullPointerException")||probe.calls)return false;
 dvm_clear_exception(v);
 /* overridePendingTransition has no Intent, so the hook receives 0. */
 intent_runtime_set_launch_host(launch_probe_host,&probe);
 bool transition_requested=intent_runtime_call(v,"android/app/Activity","overridePendingTransition","(II)V",activity,transition,2,&ignored);
 intent_runtime_clear_launch_host();
 if(!transition_requested||dvm_exception(v)||probe.calls!=1||probe.intent)return false;
 /* V2 snapshots contain only launch metadata and are copied while borrowed strings live. */
 union dvm_value v2_component[2]={{.l=dvm_new_string(v,"com.example")},{.l=dvm_new_string(v,"TargetActivity")}};
 if(!v2_component[0].l||!v2_component[1].l||
    !intent_runtime_call(v,"android/content/Intent","setClassName","(Ljava/lang/String;Ljava/lang/String;)Landroid/content/Intent;",intent,v2_component,2,&out)||dvm_exception(v))return false;
 probe.calls=0;probe.result=0;
 intent_runtime_set_launch_host_v2(launch_probe_host_v2,&probe);
 bool v2_start=intent_runtime_call(v,"android/content/Context","startActivity","(Landroid/content/Intent;)V",context,launch,1,&ignored);
 intent_runtime_clear_launch_host_v2();
 if(!v2_start||dvm_exception(v)||probe.calls!=1||probe.last.operation!=INTENT_LAUNCH_START||
    probe.last.intent_ref!=(uint32_t)intent)return false;
 probe.calls=0;
 intent_runtime_set_launch_host_v2(launch_probe_host_v2,&probe);
 bool v2_result=intent_runtime_call(v,"android/app/Activity","startActivityForResult","(Landroid/content/Intent;I)V",activity,result_args,2,&ignored);
 intent_runtime_clear_launch_host_v2();
 if(!v2_result||dvm_exception(v)||probe.calls!=1||probe.last.abi_version!=INTENT_LAUNCH_HOST_ABI_VERSION||
    probe.last.struct_size!=sizeof(intent_launch_request_v2)||probe.last.operation!=INTENT_LAUNCH_START_FOR_RESULT||
    probe.last.caller_ref!=(uint32_t)activity||probe.last.intent_ref!=(uint32_t)intent||
    probe.last.request_code!=3||probe.last.flags!=0x10000001||strcmp(probe.action,"com.example.ACTION")||
    strcmp(probe.package_name,"com.example")||strcmp(probe.class_name,"com.example.TargetActivity"))return false;
 probe.calls=0;transition[0].i=17;transition[1].i=29;
 intent_runtime_set_launch_host_v2(launch_probe_host_v2,&probe);
 bool v2_transition=intent_runtime_call(v,"android/app/Activity","overridePendingTransition","(II)V",activity,transition,2,&ignored);
 intent_runtime_clear_launch_host_v2();
 if(!v2_transition||dvm_exception(v)||probe.calls!=1||probe.last.operation!=INTENT_LAUNCH_OVERRIDE_TRANSITION||
    probe.last.caller_ref!=(uint32_t)activity||probe.last.intent_ref||probe.last.enter_animation!=17||probe.last.exit_animation!=29)return false;
 probe.calls=0;probe.result=1;
 intent_runtime_set_launch_host_v2(launch_probe_host_v2,&probe);
 bool v2_unsupported=intent_runtime_call(v,"android/content/Context","startActivity","(Landroid/content/Intent;)V",context,launch,1,&ignored);
 intent_runtime_clear_launch_host_v2();
 if(!v2_unsupported||!exception_is(v,"java/lang/UnsupportedOperationException")||probe.calls!=1)return false;
 dvm_clear_exception(v);
 probe.calls=0;probe.result=9;
 intent_runtime_set_launch_host_v2(launch_probe_host_v2,&probe);
 bool v2_error=intent_runtime_call(v,"android/content/Context","startActivity","(Landroid/content/Intent;)V",context,launch,1,&ignored);
 intent_runtime_clear_launch_host_v2();
 if(!v2_error||!exception_is(v,"java/lang/RuntimeException")||probe.calls!=1)return false;
 dvm_clear_exception(v);
 probe.calls=0;probe.result=0;
 intent_runtime_set_launch_host_v2(launch_probe_host_v2,&probe);
 bool v2_null_rejected=intent_runtime_call(v,"android/content/Context","startActivity","(Landroid/content/Intent;)V",context,null_intent,1,&ignored);
 intent_runtime_clear_launch_host_v2();
 if(!v2_null_rejected||!exception_is(v,"java/lang/NullPointerException")||probe.calls)return false;
 dvm_clear_exception(v);
 dvm_ref foreign=dvm_new_object(v,intent_class);union dvm_value foreign_arg[1]={{.l=foreign}};
 probe.calls=0;probe.result=0;
 intent_runtime_set_launch_host_v2(launch_probe_host_v2,&probe);
 bool foreign_rejected=intent_runtime_call(v,"android/content/Context","startActivity","(Landroid/content/Intent;)V",context,foreign_arg,1,&ignored);
 intent_runtime_clear_launch_host_v2();
 if(!foreign_rejected||!exception_is(v,"java/lang/IllegalStateException")||probe.calls)return false;
 dvm_clear_exception(v);
 sdk_trace_fprintf(stderr,"INTENT_RUNTIME_SELFTEST PASS constructors=1 component_ref=1 class_split=1 action=1 flags=1 data_null_default=1 data_uri=1 data_roundtrip=1 data_clear=1 data_type_reject=1 extras_ref=1 extras_defaults=1 extras_roundtrip=1 extras_replace=1 put_extra=1 launch_fail_closed=1 launch_host_bridge=1 launch_host_error=1 launch_host_validate=1 launch_host_transition=1 launch_v2_snapshot=1 launch_v2_start=1 launch_v2_result_code=1 launch_v2_transition=1 launch_v2_returns=1 launch_v2_null_intent=1 launch_v2_foreign_intent=1\n");
 return !dvm_exception(v);
}

/* The host may install inspection callbacks before running the selftest.
 * Isolate probes from those callbacks and restore the exact pair even when
 * any assertion returns early from the implementation. */
bool intent_runtime_selftest(struct dvm *v){
 intent_launch_host_v2_fn saved_v2=launch_host_v2;
 void *saved_v2_user=launch_host_v2_user;
 int (*saved_legacy)(void*,dvm_ref)=launch_host;
 void *saved_legacy_user=launch_host_user;
 launch_host_v2=NULL;launch_host_v2_user=NULL;
 launch_host=NULL;launch_host_user=NULL;
 bool ok=intent_runtime_selftest_impl(v);
 FILE *test_log=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/tap-home-intent-selftest.txt","w");
 if(test_log){fprintf(test_log,"intent_suite=%s parcelable_extra_missing_and_identity=%s exception=%u\n",ok?"PASS":"FAIL",ok?"PASS":"not_confirmed",dvm_exception(v));fclose(test_log);}
 launch_host_v2=saved_v2;launch_host_v2_user=saved_v2?saved_v2_user:NULL;
 launch_host=saved_legacy;launch_host_user=saved_legacy?saved_legacy_user:NULL;
 return ok;
}

/* Explicit native payload ownership descriptor; unknown never implies atomic. */

int intent_gc_payload(struct dvm *v,struct dvm_object *o,dvm_gc_visit_fn visit,void *user){
 (void)v;if(o->data_destroy!=intent_payload_free)return DVM_GC_PAYLOAD_UNKNOWN;intent_payload *p=o->data;
 if(p->magic!=INTENT_MAGIC)return DVM_GC_PAYLOAD_INVALID;
 if(visit){visit(&p->component_class,user);visit(&p->extras,user);visit(&p->data,user);visit(&p->categories,user);}return DVM_GC_PAYLOAD_REFS;
}
