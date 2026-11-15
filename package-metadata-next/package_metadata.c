#include "gc_runtime.h"
#include "package_metadata.h"
#include "guest_identity.h"
#include <stdlib.h>
#include <string.h>
#include "config.h"
typedef struct state {struct dvm *vm; dvm_ref pm; struct state *next;} state;
typedef struct boxed {int value;} boxed;
static state *states;
static void box_free(void *p){free(p);}
static void foreign_free(void *p){free(p);}
static state *lookup(struct dvm *v){for(state *s=states;s;s=s->next)if(s->vm==v)return s;return NULL;}
/* Android BaseBundle throws NullPointerException for a null key.  A non-string
 * receiver cannot occur through a typed invoke; report it as a cast failure
 * instead of silently treating it as a missing key. */
static bool metadata_key(struct dvm *v,const union dvm_value *a,const char **key){
 if(!a[0].l){dvm__throw(v,"java/lang/NullPointerException","metadata key");return false;}
 const char *k=dvm_string_utf8(v,a[0].l);
 if(!k){dvm__throw(v,"java/lang/ClassCastException","metadata key");return false;}
 *key=k;return true;
}
static bool metadata_bundle(struct dvm *v,dvm_ref self){struct dvm_object *o=dvm__obj(v,self);return o&&o->data&&o->data_destroy==box_free;}
static dvm_ref object(struct dvm *v,const char *name){struct dvm_class *c=dvm_find_class_or_external(v,name);return c?dvm_new_object(v,c):0;}
bool package_metadata_vm_start(struct dvm *v){
 if(lookup(v))return true;
 struct dvm_class *c=dvm_find_class_or_external(v,"android/content/pm/ApplicationInfo");
 if(!c||!c->external||c->ifields||c->nifields||c->islots||(c->super&&c->super->islots))return false;
 struct dvm_field *f=calloc(1,sizeof *f);state *s=calloc(1,sizeof *s);if(!f||!s){free(f);free(s);return false;}
 f->cls=c;f->name="metaData";f->type="Landroid/os/Bundle;";f->kind='L';f->width=1;f->access=1;
 c->ifields=f;c->nifields=1;c->islots=1;
 s->vm=v;s->next=states;states=s;return true;
}
#define IS(a,b) (!strcmp(m,a)&&!strcmp(sig,b))
static bool fail(struct dvm *v,const char *message){dvm__throw(v,"java/lang/UnsupportedOperationException",message);return true;}
bool package_metadata_call(struct dvm *v,const char *c,const char *m,const char *sig,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 state *s=lookup(v);if(!s)return false;
 if(!strcmp(c,"android/content/Context")&&IS("getPackageManager","()Landroid/content/pm/PackageManager;")){
  if(!s->pm){s->pm=object(v,"android/content/pm/PackageManager");if(!s->pm)return fail(v,"package manager allocation");dvm_pin(v,s->pm);}out->l=s->pm;return true;
 }
 if(!strcmp(c,"android/content/pm/PackageManager")&&IS("getApplicationInfo","(Ljava/lang/String;I)Landroid/content/pm/ApplicationInfo;")&&n==2){
  if(self!=s->pm)return fail(v,"foreign package manager");const char *name=dvm_string_utf8(v,a[0].l);
  if(!name||strcmp(name,GUEST_EXPECTED_PACKAGE)){dvm__throw(v,"android/content/pm/PackageManager$NameNotFoundException","package not found");return true;}
  if(a[1].i!=128)return fail(v,"unsupported application info flags");
  dvm_ref bundle=object(v,"android/os/Bundle"),info=object(v,"android/content/pm/ApplicationInfo");struct dvm_object *o=dvm__obj(v,info);
  if(!bundle||!o||!o->slots)return fail(v,"application info allocation");struct dvm_object *bo=dvm__obj(v,bundle);bo->data=calloc(1,1);if(!bo->data)return fail(v,"bundle allocation");bo->data_destroy=box_free;o->slots[0].l=bundle;out->l=info;return true;
 }
 if(!strcmp(c,"android/os/Bundle")&&IS("get","(Ljava/lang/String;)Ljava/lang/Object;")&&n==1){
  struct dvm_object *o=dvm__obj(v,self);if(!o||!o->data||o->data_destroy!=box_free)return false;
  const char *key=dvm_string_utf8(v,a[0].l);if(!key)return fail(v,"invalid metadata key");
  int val;const char *type;
  if(!strcmp(key,"BSGameSdk_PaidGame")){val=VALUE_PAID;type="java/lang/Boolean";}
  else if(!strcmp(key,"BILIGAME_CHANNELID")){val=VALUE_CHANNEL;type="java/lang/Integer";}
  else {for(size_t i=0;i<sizeof known_keys/sizeof known_keys[0];i++)if(!strcmp(key,known_keys[i]))return fail(v,"metadata value unsupported");out->l=0;return true;}
  dvm_ref ref=object(v,type);struct dvm_object *b=dvm__obj(v,ref);boxed *p=calloc(1,sizeof *p);if(!b||!p){free(p);return fail(v,"boxed metadata allocation");}p->value=val;b->data=p;b->data_destroy=box_free;out->l=ref;return true;
 }
 /* Default-value getters follow android.os.BaseBundle: a null key throws NPE,
 * a missing key returns the caller's default, and a value whose type does not
 * match the requested type returns the default too.  This metadata Bundle has
 * no String values, so getString/getCharSequence never fabricate one from
 * Boolean/Integer data; getLong does not widen an Integer. */
 if(!strcmp(c,"android/os/Bundle")&&n==2&&IS("getString","(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;")){
  if(!metadata_bundle(v,self))return false;const char *key;if(!metadata_key(v,a,&key))return true;out->l=a[1].l;return true;
 }
 if(!strcmp(c,"android/os/Bundle")&&n==2&&IS("getCharSequence","(Ljava/lang/String;Ljava/lang/CharSequence;)Ljava/lang/CharSequence;")){
  if(!metadata_bundle(v,self))return false;const char *key;if(!metadata_key(v,a,&key))return true;out->l=a[1].l;return true;
 }
 if(!strcmp(c,"android/os/Bundle")&&n==2&&IS("getInt","(Ljava/lang/String;I)I")){
  if(!metadata_bundle(v,self))return false;const char *key;if(!metadata_key(v,a,&key))return true;out->i=!strcmp(key,"BILIGAME_CHANNELID")?VALUE_CHANNEL:a[1].i;return true;
 }
 if(!strcmp(c,"android/os/Bundle")&&n==2&&IS("getLong","(Ljava/lang/String;J)J")){
  if(!metadata_bundle(v,self))return false;const char *key;if(!metadata_key(v,a,&key))return true;out->j=a[1].j;return true;
 }
 if(!strcmp(c,"android/os/Bundle")&&n==2&&IS("getBoolean","(Ljava/lang/String;Z)Z")){
  if(!metadata_bundle(v,self))return false;const char *key;if(!metadata_key(v,a,&key))return true;out->i=!strcmp(key,"BSGameSdk_PaidGame")?(VALUE_PAID!=0):a[1].i;return true;
 }
 /* One-argument overloads match the two-argument getters with Android
  * BaseBundle defaults: null/0/0/false.  This source stores no String or
  * Long values, so getString/getCharSequence return null and getLong never
  * widens the Integer channel id.  Null keys still throw NPE through
  * metadata_key and foreign Bundles are refused by metadata_bundle. */
 if(!strcmp(c,"android/os/Bundle")&&n==1&&IS("getString","(Ljava/lang/String;)Ljava/lang/String;")){
  if(!metadata_bundle(v,self))return false;const char *key;if(!metadata_key(v,a,&key))return true;out->l=0;return true;
 }
 if(!strcmp(c,"android/os/Bundle")&&n==1&&IS("getCharSequence","(Ljava/lang/String;)Ljava/lang/CharSequence;")){
  if(!metadata_bundle(v,self))return false;const char *key;if(!metadata_key(v,a,&key))return true;out->l=0;return true;
 }
 if(!strcmp(c,"android/os/Bundle")&&n==1&&IS("getInt","(Ljava/lang/String;)I")){
  if(!metadata_bundle(v,self))return false;const char *key;if(!metadata_key(v,a,&key))return true;out->i=!strcmp(key,"BILIGAME_CHANNELID")?VALUE_CHANNEL:0;return true;
 }
 if(!strcmp(c,"android/os/Bundle")&&n==1&&IS("getLong","(Ljava/lang/String;)J")){
  if(!metadata_bundle(v,self))return false;const char *key;if(!metadata_key(v,a,&key))return true;out->j=0;return true;
 }
 if(!strcmp(c,"android/os/Bundle")&&n==1&&IS("getBoolean","(Ljava/lang/String;)Z")){
  if(!metadata_bundle(v,self))return false;const char *key;if(!metadata_key(v,a,&key))return true;out->i=!strcmp(key,"BSGameSdk_PaidGame")?(VALUE_PAID!=0):0;return true;
 }
 if((!strcmp(c,"java/lang/Boolean")&&IS("booleanValue","()Z"))||(!strcmp(c,"java/lang/Integer")&&IS("intValue","()I"))){struct dvm_object *o=dvm__obj(v,self);if(!o||o->data_destroy!=box_free||!o->data)return false;out->i=((boxed*)o->data)->value;return true;}
 return false;
}
/* Not wired by this file: the active rebuild42 build copies
 * reviewed_metadata.c over package_metadata.c, where package_metadata_selftest
 * is already wired through java_basics_test.c.  This copy is kept in sync for
 * the standalone package_metadata.c source path. */
bool package_metadata_selftest(struct dvm *v){
 union dvm_value out={0},a[2]={{0},{0}};
 extern dvm_ref android_basics_application(struct dvm *);
 dvm_ref context=android_basics_application(v);
 if(!context)return false;
 if(!package_metadata_call(v,"android/content/Context","getPackageManager","()Landroid/content/pm/PackageManager;",context,NULL,0,&out)||!out.l)return false;
 dvm_ref pm=out.l;a[0].l=dvm_new_string(v,GUEST_EXPECTED_PACKAGE);a[1].i=128;
 if(!package_metadata_call(v,"android/content/pm/PackageManager","getApplicationInfo","(Ljava/lang/String;I)Landroid/content/pm/ApplicationInfo;",pm,a,2,&out)||!out.l)return false;
 if(!dvm_get_field(v,out.l,"metaData","Landroid/os/Bundle;",&out)||!out.l)return false;
 dvm_ref bundle=out.l,fallback=dvm_new_string(v,"fallback");
 if(!fallback)return false;
 /* Boolean value: real value wins over a deliberately opposing default, and
  * getString must return the supplied fallback instead of "true"/"false". */
 a[0].l=dvm_new_string(v,"BSGameSdk_PaidGame");a[1].i=1;
 if(!package_metadata_call(v,"android/os/Bundle","getBoolean","(Ljava/lang/String;Z)Z",bundle,a,2,&out))return false;
 if(out.i!=(VALUE_PAID!=0))return false;
 a[1].l=fallback;
 if(!package_metadata_call(v,"android/os/Bundle","getString","(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",bundle,a,2,&out)||out.l!=fallback)return false;
 if(!package_metadata_call(v,"android/os/Bundle","getCharSequence","(Ljava/lang/String;Ljava/lang/CharSequence;)Ljava/lang/CharSequence;",bundle,a,2,&out)||out.l!=fallback)return false;
 a[1].i=-7;
 if(!package_metadata_call(v,"android/os/Bundle","getInt","(Ljava/lang/String;I)I",bundle,a,2,&out)||out.i!=-7)return false;
 /* Integer value: getInt returns it; getString/getBoolean/getLong return the
  * default and never stringify or widen it. */
 a[0].l=dvm_new_string(v,"BILIGAME_CHANNELID");a[1].i=999;
 if(!package_metadata_call(v,"android/os/Bundle","getInt","(Ljava/lang/String;I)I",bundle,a,2,&out)||out.i!=VALUE_CHANNEL)return false;
 a[1].l=fallback;
 if(!package_metadata_call(v,"android/os/Bundle","getString","(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",bundle,a,2,&out)||out.l!=fallback)return false;
 a[1].i=0;
 if(!package_metadata_call(v,"android/os/Bundle","getBoolean","(Ljava/lang/String;Z)Z",bundle,a,2,&out)||out.i!=0)return false;
 a[1].j=1234567890123LL;
 if(!package_metadata_call(v,"android/os/Bundle","getLong","(Ljava/lang/String;J)J",bundle,a,2,&out)||out.j!=1234567890123LL)return false;
 /* Unknown key: every getter returns its default. */
 a[0].l=dvm_new_string(v,"sdktrace.absent.metadata.key");
 a[1].l=fallback;
 if(!package_metadata_call(v,"android/os/Bundle","getString","(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",bundle,a,2,&out)||out.l!=fallback)return false;
 a[1].i=77;
 if(!package_metadata_call(v,"android/os/Bundle","getInt","(Ljava/lang/String;I)I",bundle,a,2,&out)||out.i!=77)return false;
 a[1].i=1;
 if(!package_metadata_call(v,"android/os/Bundle","getBoolean","(Ljava/lang/String;Z)Z",bundle,a,2,&out)||out.i!=1)return false;
 /* Null key: NullPointerException, not a default value or MISSING_API. */
 a[0].l=0;a[1].l=fallback;
 bool handled=package_metadata_call(v,"android/os/Bundle","getString","(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",bundle,a,2,&out);
 dvm_ref exc=dvm_exception(v);char text[128]={0};
 if(exc)dvm_describe_exception(v,exc,text,sizeof text);
 bool npe=handled&&exc&&!strncmp(text,"java/lang/NullPointerException",30);
 if(exc)dvm_clear_exception(v);
 if(!npe)return false;
 /* A Bundle whose payload marker does not belong to package_metadata must be
  * refused, so dispatch can fall through instead of fabricating a value. */
 dvm_ref foreign=object(v,"android/os/Bundle");struct dvm_object *fo=dvm__obj(v,foreign);
 if(!fo)return false;fo->data=calloc(1,1);if(!fo->data)return false;fo->data_destroy=foreign_free;
 a[0].l=dvm_new_string(v,"BSGameSdk_PaidGame");a[1].l=fallback;
 bool claimed=package_metadata_call(v,"android/os/Bundle","getString","(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",foreign,a,2,&out);
 bool refused=!claimed&&!dvm_exception(v);
 if(dvm_exception(v))dvm_clear_exception(v);
 return refused;
}


int metadata_gc_payload(struct dvm *v,struct dvm_object *o,dvm_gc_visit_fn visit,void *u){
 (void)v;(void)visit;(void)u;return o->data_destroy==box_free?DVM_GC_PAYLOAD_ATOMIC:DVM_GC_PAYLOAD_UNKNOWN;
}
