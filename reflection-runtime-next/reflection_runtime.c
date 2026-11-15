#include "reflection_runtime.h"
#include "prefs_runtime.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
extern int sdk_trace_fprintf(FILE*,const char*,...);

static struct dvm *active;
typedef struct suppressed_entry {dvm_ref owner; dvm_ref *items; size_t count,capacity; struct suppressed_entry *next;} suppressed_entry;
static suppressed_entry *suppressed_head;
typedef struct { struct dvm_class *owner; int kind; } reflected;
static void reflected_free(void *p) { free(p); }
void reflection_runtime_vm_end(struct dvm *v) {
 if(v!=active)return;
 while(suppressed_head){suppressed_entry *next=suppressed_head->next;
  dvm_unpin(v,suppressed_head->owner);
  for(size_t i=0;i<suppressed_head->count;i++)dvm_unpin(v,suppressed_head->items[i]);
  free(suppressed_head->items);free(suppressed_head);suppressed_head=next;}
 active=NULL;
}
void reflection_runtime_vm_start(struct dvm *v) {
 active=v;struct dvm_class *base=dvm_find_class_or_external(v,"java/lang/ReflectiveOperationException");
 struct dvm_class *exception=dvm_find_class_or_external(v,"java/lang/Exception");if(base&&base->external)base->super=exception;
 struct dvm_class *missing=dvm_find_class_or_external(v,"java/lang/NoSuchMethodException");if(missing&&missing->external)missing->super=base;
}
static bool fail(struct dvm *v,const char *type,const char *message) { dvm__throw(v,type,"%s",message); return true; }
static bool throwable(struct dvm *v,dvm_ref ref){struct dvm_class *base=dvm_find_class_or_external(v,"java/lang/Throwable");struct dvm_object *o=dvm__obj(v,ref);return o&&base&&dvm__class_assignable(v,o->cls,base);}
static suppressed_entry *suppressed_for(dvm_ref owner){for(suppressed_entry *p=suppressed_head;p;p=p->next)if(p->owner==owner)return p;return NULL;}
static bool add_suppressed(struct dvm *v,dvm_ref owner,dvm_ref value){
 if(!throwable(v,owner))return fail(v,"java/lang/IllegalArgumentException","addSuppressed target");
 if(!value)return fail(v,"java/lang/NullPointerException","suppressed exception");
 if(value==owner)return fail(v,"java/lang/IllegalArgumentException","self suppression");
 if(!throwable(v,value))return fail(v,"java/lang/IllegalArgumentException","suppressed exception type");
 suppressed_entry *p=suppressed_for(owner);
 if(!p){p=calloc(1,sizeof *p);if(!p)return fail(v,"java/lang/OutOfMemoryError","suppressed list");p->owner=owner;p->next=suppressed_head;suppressed_head=p;dvm_pin(v,owner);}
 if(p->count==p->capacity){size_t cap=p->capacity?p->capacity*2:4;dvm_ref *items=realloc(p->items,cap*sizeof *items);if(!items)return fail(v,"java/lang/OutOfMemoryError","suppressed list");p->items=items;p->capacity=cap;}
 p->items[p->count++]=value;dvm_pin(v,value);return true;
}
static bool get_suppressed(struct dvm *v,dvm_ref owner,union dvm_value *out){
 if(!throwable(v,owner))return fail(v,"java/lang/IllegalArgumentException","getSuppressed target");
 suppressed_entry *p=suppressed_for(owner);size_t count=p?p->count:0;
 out->l=dvm_new_array(v,'L',"Ljava/lang/Throwable;",(uint32_t)count);
 if(!out->l)return fail(v,"java/lang/OutOfMemoryError","suppressed array");
 if(count)memcpy(dvm_array_data(v,out->l),p->items,count*sizeof(dvm_ref));return true;
}
static bool is_class_array(struct dvm *v,dvm_ref r) {
 if(!r)return true;
 struct dvm_object *o=dvm__obj(v,r);
 return o&&o->kind==DVM_OBJ_ARRAY&&o->cls&&o->cls->desc&&!strcmp(o->cls->desc,"[Ljava/lang/Class;");
}
static bool is_object_array(struct dvm *v,dvm_ref r) {
 if(!r)return true;
 struct dvm_object *o=dvm__obj(v,r);
 return o&&o->kind==DVM_OBJ_ARRAY&&o->cls&&o->cls->desc&&!strcmp(o->cls->desc,"[Ljava/lang/Object;");
}
static bool has_public_zero_method(struct dvm_class *c,const char *name) {
 for(struct dvm_class *k=c;k;k=k->super)for(int i=0;i<k->nmethods;i++) {
  struct dvm_method *m=&k->methods[i];
  if((m->access&1)&&!strcmp(m->name,name)&&m->sig&&m->sig[0]=='('&&m->sig[1]==')')return true;
 }
 return false;
}
bool reflection_runtime_call(struct dvm *v,const char *c,const char *m,const char *s,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out) {
 if(v!=active)return false;
 if(!strcmp(c,"java/lang/Throwable")&&!strcmp(m,"addSuppressed")&&!strcmp(s,"(Ljava/lang/Throwable;)V")&&n==1)return add_suppressed(v,self,a[0].l);
 if(!strcmp(c,"java/lang/Throwable")&&!strcmp(m,"getSuppressed")&&!strcmp(s,"()[Ljava/lang/Throwable;")&&n==0)return get_suppressed(v,self,out);
 if(!strcmp(c,"java/lang/Class")&&(!strcmp(m,"getMethod")||!strcmp(m,"getDeclaredMethod"))&&!strcmp(s,"(Ljava/lang/String;[Ljava/lang/Class;)Ljava/lang/reflect/Method;")&&n==2) {
  struct dvm_object *co=dvm__obj(v,self);
  if(!co||co->kind!=DVM_OBJ_CLASS||!co->klass||!a[0].l)return fail(v,"java/lang/NullPointerException","Class.getMethod");
  if(!is_class_array(v,a[1].l))return fail(v,"java/lang/IllegalArgumentException","getMethod parameter types");
  const char *name=dvm_string_utf8(v,a[0].l);if(!name||memchr(name,0,dvm_string_utf8_length(v,a[0].l)))return fail(v,"java/lang/IllegalArgumentException","method name");
  bool declared=!strcmp(m,"getDeclaredMethod");uint32_t count=a[1].l?dvm_array_length(v,a[1].l):0;
  struct dvm_class *owner=co->klass;
  int kind=0;
  if(!declared&&!count&&!strcmp(owner->name,"android/content/SharedPreferences$Editor")&&!strcmp(name,"apply"))kind=1;
  if(declared&&count==1&&!strcmp(owner->name,"java/lang/Throwable")&&!strcmp(name,"addSuppressed")){
   dvm_ref item=((dvm_ref*)dvm_array_data(v,a[1].l))[0];struct dvm_object *type=dvm__obj(v,item);
   if(type&&type->kind==DVM_OBJ_CLASS&&type->klass==owner)kind=2;
  }
  if(!kind&&((!strcmp(owner->name,"javax/net/ssl/SSLParameters")&&!strcmp(name,"setApplicationProtocols"))||(!strcmp(owner->name,"javax/net/ssl/SSLSocket")&&!strcmp(name,"getApplicationProtocol"))))return fail(v,"java/lang/NoSuchMethodException",name);
  if(!kind)return fail(v,!count&&has_public_zero_method(owner,name)?"java/lang/UnsupportedOperationException":"java/lang/NoSuchMethodException",name);
  reflected *r=calloc(1,sizeof *r);if(!r)return fail(v,"java/lang/OutOfMemoryError","Method");
  r->owner=owner;r->kind=kind;
  out->l=dvm_new_object(v,dvm_find_class_or_external(v,"java/lang/reflect/Method"));
  struct dvm_object *mo=dvm__obj(v,out->l);if(!mo){free(r);return fail(v,"java/lang/OutOfMemoryError","Method");}
  mo->data=r;mo->data_destroy=reflected_free;return true;
 }
 if(!strcmp(c,"java/lang/reflect/Method")&&!strcmp(m,"invoke")&&!strcmp(s,"(Ljava/lang/Object;[Ljava/lang/Object;)Ljava/lang/Object;")&&n==2) {
  struct dvm_object *mo=dvm__obj(v,self);reflected *r=mo&&mo->data_destroy==reflected_free?mo->data:NULL;
  if(!r)return fail(v,"java/lang/IllegalArgumentException","Method.invoke receiver");
  uint32_t count=a[1].l?dvm_array_length(v,a[1].l):0;
  if(!is_object_array(v,a[1].l)||count!=(r->kind==2?1u:0u))return fail(v,"java/lang/IllegalArgumentException","Method.invoke arguments");
  struct dvm_object *target=dvm__obj(v,a[0].l);
  if(!target||!dvm__class_assignable(v,target->cls,r->owner))return fail(v,"java/lang/IllegalArgumentException","Method.invoke target");
  if(r->kind==1) {union dvm_value ignored={0};if(!prefs_runtime_call(v,"android/content/SharedPreferences$Editor","apply","()V",a[0].l,NULL,0,&ignored))return fail(v,"java/lang/IllegalArgumentException","Method.invoke editor");out->l=0;return true;}
  if(r->kind==2){dvm_ref value=((dvm_ref*)dvm_array_data(v,a[1].l))[0];out->l=0;return add_suppressed(v,a[0].l,value);}
  return false;
 }
 return false;
}
bool reflection_runtime_selftest(struct dvm *v) {
 if(v!=active)return false;
 struct dvm_class *editor=dvm_find_class_or_external(v,"android/content/SharedPreferences$Editor");
 dvm_ref klass=dvm_class_object(v,editor),name=dvm_new_string(v,"apply");
 union dvm_value args[2]={{.l=name},{.l=dvm_new_array(v,'L',"Ljava/lang/Class;",0)}},out={0};
 if(!reflection_runtime_call(v,"java/lang/Class","getMethod","(Ljava/lang/String;[Ljava/lang/Class;)Ljava/lang/reflect/Method;",klass,args,2,&out)||!out.l)return false;
 dvm_ref method=out.l;
 dvm_ref context=dvm_new_object(v,dvm_find_class_or_external(v,"android/content/Context"));
 union dvm_value pa[2]={{.l=dvm_new_string(v,"__reflection_runtime_selftest_v1__")},{.i=0}};
 if(!prefs_runtime_call(v,"android/content/Context","getSharedPreferences","(Ljava/lang/String;I)Landroid/content/SharedPreferences;",context,pa,2,&out)||!out.l)return false;
 dvm_ref pref=out.l;
 if(!prefs_runtime_call(v,"android/content/SharedPreferences","edit","()Landroid/content/SharedPreferences$Editor;",pref,NULL,0,&out)||!out.l)return false;
 dvm_ref edit=out.l;
 pa[0].l=dvm_new_string(v,"key");pa[1].l=dvm_new_string(v,"value");
 if(!prefs_runtime_call(v,"android/content/SharedPreferences$Editor","putString","(Ljava/lang/String;Ljava/lang/String;)Landroid/content/SharedPreferences$Editor;",edit,pa,2,&out))return false;
 union dvm_value invoke[2]={{.l=edit},{.l=dvm_new_array(v,'L',"Ljava/lang/Object;",0)}};
 if(!reflection_runtime_call(v,"java/lang/reflect/Method","invoke","(Ljava/lang/Object;[Ljava/lang/Object;)Ljava/lang/Object;",method,invoke,2,&out)||out.l||v->exception)return false;
 pa[1].l=0;
 if(!prefs_runtime_call(v,"android/content/SharedPreferences","getString","(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",pref,pa,2,&out)||!out.l||strcmp(dvm_string_utf8(v,out.l),"value"))return false;
 args[0].l=dvm_new_string(v,"doesNotExist");out.l=0;
 if(!reflection_runtime_call(v,"java/lang/Class","getMethod","(Ljava/lang/String;[Ljava/lang/Class;)Ljava/lang/reflect/Method;",klass,args,2,&out)||out.l||!v->exception)return false;
 dvm_clear_exception(v);
 struct dvm_class *base=dvm_find_class_or_external(v,"java/lang/Throwable");
 struct dvm_class *runtime=dvm_find_class_or_external(v,"java/lang/RuntimeException");
 dvm_ref owner=dvm_new_object(v,runtime),first=dvm_new_object(v,runtime),second=dvm_new_object(v,runtime);
 if(!owner||!first||!second)return false;
 union dvm_value added={.l=first};
 if(!reflection_runtime_call(v,"java/lang/Throwable","addSuppressed","(Ljava/lang/Throwable;)V",owner,&added,1,&out))return false;
 added.l=second;
 if(!reflection_runtime_call(v,"java/lang/Throwable","addSuppressed","(Ljava/lang/Throwable;)V",owner,&added,1,&out))return false;
 if(!reflection_runtime_call(v,"java/lang/Throwable","getSuppressed","()[Ljava/lang/Throwable;",owner,NULL,0,&out)||dvm_array_length(v,out.l)!=2)return false;
 dvm_ref *list=dvm_array_data(v,out.l);if(!list||list[0]!=first||list[1]!=second)return false;
 added.l=owner;if(!reflection_runtime_call(v,"java/lang/Throwable","addSuppressed","(Ljava/lang/Throwable;)V",owner,&added,1,&out)||!v->exception)return false;dvm_clear_exception(v);
 added.l=0;if(!reflection_runtime_call(v,"java/lang/Throwable","addSuppressed","(Ljava/lang/Throwable;)V",owner,&added,1,&out)||!v->exception)return false;dvm_clear_exception(v);
 dvm_ref params=dvm_new_array(v,'L',"Ljava/lang/Class;",1);((dvm_ref*)dvm_array_data(v,params))[0]=dvm_class_object(v,base);
 args[0].l=dvm_new_string(v,"addSuppressed");args[1].l=params;
 if(!reflection_runtime_call(v,"java/lang/Class","getDeclaredMethod","(Ljava/lang/String;[Ljava/lang/Class;)Ljava/lang/reflect/Method;",dvm_class_object(v,base),args,2,&out)||!out.l)return false;
 dvm_ref add_method=out.l,values=dvm_new_array(v,'L',"Ljava/lang/Object;",1);((dvm_ref*)dvm_array_data(v,values))[0]=first;
 invoke[0].l=owner;invoke[1].l=values;
 if(!reflection_runtime_call(v,"java/lang/reflect/Method","invoke","(Ljava/lang/Object;[Ljava/lang/Object;)Ljava/lang/Object;",add_method,invoke,2,&out)||out.l||v->exception)return false;
 if(!reflection_runtime_call(v,"java/lang/Throwable","getSuppressed","()[Ljava/lang/Throwable;",owner,NULL,0,&out)||dvm_array_length(v,out.l)!=3)return false;
 sdk_trace_fprintf(stderr,"REFLECTION_SELFTEST PASS apply_lookup=1 invoke_persistent_write=1 missing_method=1 suppressed=1 declared_invoke=1\n");
 return method!=0;
}
