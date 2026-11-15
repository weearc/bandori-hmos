#include "object_runtime.h"
#include "java_basics.h"
#include "string_runtime.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <limits.h>
extern int sdk_trace_fprintf(FILE*,const char*,...);
/* Java's default Object.toString() is Class.getName() + "@" +
 * Integer.toHexString(hashCode()).  Dispatch to the object's own hashCode
 * when it has a real body (a class may override hashCode without overriding
 * toString); boxed values go through java_basics; classes with no
 * implementation use the VM object reference as a stable identity hash. */
static bool default_tostring(struct dvm *v,dvm_ref self,union dvm_value *out){
 if(!self){dvm__throw(v,"java/lang/NullPointerException","Object.toString receiver");return true;}
 struct dvm_class *type=dvm_object_class(v,self);
 if(!type){dvm__throw(v,"java/lang/IllegalArgumentException","Object.toString receiver");return true;}
 if(type->name&&!strncmp(type->name,"org/json/",9)){
  static unsigned count;
  if(count++<32){FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt","a");
   if(f){fprintf(f,"JSON_DEFAULT_TOSTRING class=%s ref=%u payload=%d frames=",type->name,(unsigned)self,dvm__obj(v,self)->data!=NULL);
    for(int i=v->ncallstack>4?v->ncallstack-4:0;i<v->ncallstack;i++){struct dvm_method *frame=v->callstack[i];if(frame&&frame->cls)fprintf(f,"%s.%s;",frame->cls->name,frame->name);}
    fputc('\n',f);fclose(f);}}
 }
 int32_t hash=0;
 struct dvm_method *method=dvm_find_method(v,type,"hashCode","()I");
 if(method&&(method->has_code||method->builtin)){
  union dvm_value value={0};
  if(!dvm_call(v,method,self,NULL,0,&value))return true; /* propagate */
  hash=value.i;
 }else{
  union dvm_value value={0};
  if(java_basics_call(v,type->name,"hashCode","()I",self,NULL,0,&value)){
   if(dvm_exception(v))return true;
   hash=value.i;
  }else hash=(int32_t)(uintptr_t)self;
 }
 const char *raw=type->desc&&type->desc[0]=='['?type->desc:type->name;
 if(!raw){dvm__throw(v,"java/lang/InternalError","Object.toString class name");return true;}
 size_t length=strlen(raw);char *name=malloc(length+1);
 if(!name){dvm__throw(v,"java/lang/OutOfMemoryError","Object.toString class name");return true;}
 for(size_t i=0;i<length;i++)name[i]=raw[i]=='/'?'.':raw[i];
 name[length]='\0';
 char hash_text[16];int hash_length=snprintf(hash_text,sizeof hash_text,"%x",(unsigned)hash);
 if(hash_length<0||(size_t)hash_length>=sizeof hash_text){free(name);dvm__throw(v,"java/lang/InternalError","Object.toString hash");return true;}
 size_t total=length+1+(size_t)hash_length;char *text=malloc(total+1);
 if(!text){free(name);dvm__throw(v,"java/lang/OutOfMemoryError","Object.toString");return true;}
 memcpy(text,name,length);text[length]='@';memcpy(text+length+1,hash_text,(size_t)hash_length);
 out->l=dvm_new_string_n(v,text,total);free(text);free(name);
 if(!out->l&&!dvm_exception(v))dvm__throw(v,"java/lang/OutOfMemoryError","Object.toString result");
 return true;
}
bool object_runtime_call(struct dvm *v,const char *c,const char *m,const char *s,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 /* External Object method references still carry their declared owner.
  * Preserve String virtual semantics before Object's identity defaults. */
 struct dvm_object *receiver=dvm__obj(v,self);
 if(!strcmp(c,"java/lang/Object")&&receiver&&receiver->kind==DVM_OBJ_STRING&&
    ((!strcmp(m,"toString")&&!strcmp(s,"()Ljava/lang/String;"))||
     (!strcmp(m,"hashCode")&&!strcmp(s,"()I"))||
     (!strcmp(m,"equals")&&!strcmp(s,"(Ljava/lang/Object;)Z"))||
     (!strcmp(m,"length")&&!strcmp(s,"()I"))||
     (!strcmp(m,"charAt")&&!strcmp(s,"(I)C"))))
  return string_runtime_call(v,"java/lang/String",m,s,self,a,n,out);
 if(!strcmp(m,"getClass")&&!strcmp(s,"()Ljava/lang/Class;")&&!n){
  struct dvm_class *type=dvm_object_class(v,self);if(!type){dvm__throw(v,"java/lang/NullPointerException","Object.getClass receiver");return true;}
  out->l=dvm_class_object(v,type);return true;
 }
 if(!strcmp(c,"java/lang/Object")&&!strcmp(m,"toString")&&!strcmp(s,"()Ljava/lang/String;")&&!n)
  return default_tostring(v,self,out);
 if(!strcmp(c,"java/lang/Object")&&!strcmp(m,"equals")&&!strcmp(s,"(Ljava/lang/Object;)Z")&&n==1){
  if(!self){dvm__throw(v,"java/lang/NullPointerException","Object.equals receiver");return true;}
  out->i=self==a[0].l;return true;
 }
 /* AOSP Object.hashCode() is the identity hash: stable for one object and
  * distinct between objects.  The VM object reference is that identity here;
  * default_tostring() already uses the same value for its @hex suffix.  A
  * concrete class that overrides hashCode() is dispatched before this hook
  * (Gson TypeToken constructs a Type-keyed map and reached this gap). */
 if(!strcmp(c,"java/lang/Object")&&!strcmp(m,"hashCode")&&!strcmp(s,"()I")&&!n){
  if(!self){dvm__throw(v,"java/lang/NullPointerException","Object.hashCode receiver");return true;}
  out->i=(int32_t)(uintptr_t)self;return true;
 }
 /* Object.wait/notify/notifyAll map onto the VM's real monitor primitive:
  * dvm__monitor_wait releases the monitor, sleeps on the object's channel and
  * re-acquires it, reporting InterruptedException exactly as Object.wait()
  * specifies; dvm__monitor_notify requires monitor ownership and bumps the
  * sequence the waiters watch.  wait(0) and wait(0,0) mean "no timeout". */
 if(!strcmp(c,"java/lang/Object")&&!strcmp(m,"notify")&&!strcmp(s,"()V")&&n==0){
  if(!self){dvm__throw(v,"java/lang/NullPointerException","Object.notify receiver");return true;}
  dvm__monitor_notify(v,self,false);return true;
 }
 if(!strcmp(c,"java/lang/Object")&&!strcmp(m,"notifyAll")&&!strcmp(s,"()V")&&n==0){
  if(!self){dvm__throw(v,"java/lang/NullPointerException","Object.notifyAll receiver");return true;}
  dvm__monitor_notify(v,self,true);return true;
 }
 if(!strcmp(c,"java/lang/Object")&&!strcmp(m,"wait")){
  uint64_t timeout_ms=0;
  if(!strcmp(s,"()V")&&n==0){}
  else if(!strcmp(s,"(J)V")&&n==1){
   if(a[0].j<0){dvm__throw(v,"java/lang/IllegalArgumentException","wait timeout is negative");return true;}
   timeout_ms=(uint64_t)a[0].j;
  }else if(!strcmp(s,"(JI)V")&&n==2){
   if(a[0].j<0||a[1].i<0||a[1].i>999999){dvm__throw(v,"java/lang/IllegalArgumentException","wait timeout out of range");return true;}
   timeout_ms=(uint64_t)a[0].j+(a[1].i>0?1u:0u);
  }else return false;
  if(!self){dvm__throw(v,"java/lang/NullPointerException","Object.wait receiver");return true;}
  bool notified=false;dvm__monitor_wait(v,self,timeout_ms,&notified);return true;
 }
 /* Object.clone(): a shallow copy of the same class.  A class that does not
  * implement Cloneable gets CloneNotSupportedException; the copy owns its own
  * slot array copied from the receiver, and runtime-private payloads are not
  * copied (they are not Java fields; a class that needs them provides its own
  * clone body in the APK). */
 if(!strcmp(c,"java/lang/Object")&&!strcmp(m,"clone")&&!strcmp(s,"()Ljava/lang/Object;")&&n==0){
  if(!self){dvm__throw(v,"java/lang/NullPointerException","Object.clone receiver");return true;}
  struct dvm_class *rc=dvm_object_class(v,self);
  struct dvm_object *array=dvm__obj(v,self);
  /* Every Java array is Cloneable, including arrays of enum/reference values.
   * Copy the element storage, keeping referenced objects shared. */
  if(array&&array->kind==DVM_OBJ_ARRAY&&rc&&rc->desc&&rc->desc[0]=='['){
   char kind=array->elem_kind;uint32_t length=array->length;
   dvm_ref copy=dvm_new_array(v,kind,rc->desc+1,length);
   if(!copy){dvm__throw(v,"java/lang/OutOfMemoryError","array clone");return true;}
   array=dvm__obj(v,self);struct dvm_object *dst=dvm__obj(v,copy);
   memcpy(dst->data,array->data,(size_t)length*dvm__elem_width(kind));out->l=copy;return true;
  }
  struct dvm_class *cloneable=dvm_find_class_or_external(v,"java/lang/Cloneable");
  if(!rc||!cloneable||!dvm__class_assignable(v,rc,cloneable)){dvm__throw(v,"java/lang/CloneNotSupportedException","class does not implement Cloneable");return true;}
  dvm_ref copy=dvm_new_object(v,rc);
  if(!copy){dvm__throw(v,"java/lang/OutOfMemoryError","Object.clone");return true;}
  struct dvm_object *src=dvm__obj(v,self),*dst=dvm__obj(v,copy);
  if(src&&dst&&src->slots&&dst->slots&&rc->islots)memcpy(dst->slots,src->slots,(size_t)rc->islots*sizeof *dst->slots);
  out->l=copy;return true;
 }
 if(!strcmp(c,"java/lang/Math")&&!strcmp(m,"min")&&n==2){
  if(!strcmp(s,"(II)I")){out->i=a[0].i<a[1].i?a[0].i:a[1].i;return true;}
  if(!strcmp(s,"(JJ)J")){out->j=a[0].j<a[1].j?a[0].j:a[1].j;return true;}
 }
 return false;
}
bool object_runtime_selftest(struct dvm *v){
 /* Primitive/reference contents survive clone; changing the copied array's
  * storage must not change the receiver, while object references stay equal. */
 for(int pass=0;pass<2;pass++){
  dvm_ref src=dvm_new_array(v,pass?'L':'I',pass?"Ljava/lang/Object;":"I",2);union dvm_value clone={0};
  if(!src)return false;uint32_t *items=dvm_array_data(v,src);dvm_ref item=pass?dvm_new_string(v,"clone-item"):0;items=dvm_array_data(v,src);items[0]=pass?item:12345;items[1]=0;
  if(!object_runtime_call(v,"java/lang/Object","clone","()Ljava/lang/Object;",src,NULL,0,&clone)||!clone.l||clone.l==src||dvm_exception(v)||dvm_object_class(v,clone.l)!=dvm_object_class(v,src)||dvm_array_length(v,clone.l)!=2)return false;
  items=dvm_array_data(v,src);uint32_t *copied=dvm_array_data(v,clone.l);if(copied==items||copied[0]!=items[0]||copied[1]!=0)return false;copied[0]=0;if(items[0]!=(pass?item:12345))return false;
 }
 dvm_ref array=dvm_new_array(v,'B',"B",0);union dvm_value out={0},a[2]={{.i=INT_MIN},{.i=INT_MAX}};
 if(!object_runtime_call(v,"java/lang/Object","getClass","()Ljava/lang/Class;",array,NULL,0,&out)||out.l!=dvm_class_object(v,dvm_object_class(v,array)))return false;
 if(!object_runtime_call(v,"java/lang/Math","min","(II)I",0,a,2,&out)||out.i!=INT_MIN)return false;
 a[0].j=INT64_MAX;a[1].j=INT64_MIN;
 if(!object_runtime_call(v,"java/lang/Math","min","(JJ)J",0,a,2,&out)||out.j!=INT64_MIN)return false;
 if(!java_basics_static(v,"java/util/concurrent/TimeUnit","MINUTES","Ljava/util/concurrent/TimeUnit;",&out)||!out.l)return false;dvm_ref minutes=out.l;
 a[0].j=5;if(!java_basics_call(v,"java/util/concurrent/TimeUnit","toNanos","(J)J",minutes,a,1,&out)||out.j!=300000000000LL)return false;
 a[0].j=INT64_MAX;if(!java_basics_call(v,"java/util/concurrent/TimeUnit","toNanos","(J)J",minutes,a,1,&out)||out.j!=INT64_MAX)return false;
 a[0].j=INT64_MIN;if(!java_basics_call(v,"java/util/concurrent/TimeUnit","toNanos","(J)J",minutes,a,1,&out)||out.j!=INT64_MIN)return false;
 /* Default Object.toString: Class.getName() + "@" + hex hashCode; stable for
  * the same object and distinct from null. */
 struct dvm_class *plain_class=dvm__register_builtin(v,"Lsdktrace/tests/ObjectToString;");
 dvm_ref plain=plain_class?dvm_new_object(v,plain_class):0;
 union dvm_value text={0},text2={0};
 if(!plain||!object_runtime_call(v,"java/lang/Object","toString","()Ljava/lang/String;",plain,NULL,0,&text)||!text.l)return false;
 const char *plain_text=dvm_string_utf8(v,text.l),*prefix="sdktrace.tests.ObjectToString@";
 if(!plain_text||strncmp(plain_text,prefix,strlen(prefix))||!strchr(plain_text,'@'))return false;
 if(!object_runtime_call(v,"java/lang/Object","toString","()Ljava/lang/String;",plain,NULL,0,&text2)||!text2.l||strcmp(dvm_string_utf8(v,text2.l),plain_text))return false;
 bool null_receiver=object_runtime_call(v,"java/lang/Object","toString","()Ljava/lang/String;",0,NULL,0,&text)&&dvm_exception(v);
 if(null_receiver){struct dvm_object *e=dvm__obj(v,dvm_exception(v));null_receiver=e&&e->cls&&e->cls->name&&!strcmp(e->cls->name,"java/lang/NullPointerException");}
 if(dvm_exception(v))dvm_clear_exception(v);
 if(!null_receiver)return false;
 if(object_runtime_call(v,"java/lang/Object","toString","()I",plain,NULL,0,&text))return false;
 if(dvm_exception(v))return false;
 /* Default Object.hashCode: identity semantics, stable per object. */
 union dvm_value h1={0},h2={0},h3={0};
 if(!object_runtime_call(v,"java/lang/Object","hashCode","()I",plain,NULL,0,&h1))return false;
 if(!object_runtime_call(v,"java/lang/Object","hashCode","()I",plain,NULL,0,&h2)||h1.i!=h2.i)return false;
 dvm_ref other=dvm_new_object(v,plain_class);
 if(!other||!object_runtime_call(v,"java/lang/Object","hashCode","()I",other,NULL,0,&h3)||h3.i==h1.i)return false;
 /* notifyAll without monitor ownership must be IllegalMonitorStateException;
  * clone without Cloneable must be CloneNotSupportedException; once the
  * fixture implements Cloneable, clone returns a distinct instance of the
  * same class. */
 bool monitor_contract=true,clone_contract=true;
 if(!object_runtime_call(v,"java/lang/Object","notifyAll","()V",plain,NULL,0,&h3)||!dvm_exception(v))monitor_contract=false;
 else{struct dvm_object *e=dvm__obj(v,dvm_exception(v));monitor_contract=e&&e->cls&&e->cls->name&&!strcmp(e->cls->name,"java/lang/IllegalMonitorStateException");dvm_clear_exception(v);}
 if(!object_runtime_call(v,"java/lang/Object","clone","()Ljava/lang/Object;",plain,NULL,0,&h3)||!dvm_exception(v))clone_contract=false;
 else{struct dvm_object *e=dvm__obj(v,dvm_exception(v));clone_contract=e&&e->cls&&e->cls->name&&!strcmp(e->cls->name,"java/lang/CloneNotSupportedException");dvm_clear_exception(v);}
 struct dvm_class *cloneable=dvm_find_class_or_external(v,"java/lang/Cloneable");
 if(cloneable){plain_class->ifaces=calloc(1,sizeof *plain_class->ifaces);if(!plain_class->ifaces)return false;plain_class->ifaces[0]=cloneable;plain_class->nifaces=1;}
 if(!object_runtime_call(v,"java/lang/Object","clone","()Ljava/lang/Object;",plain,NULL,0,&h3)||!h3.l||dvm_exception(v))clone_contract=false;
 else if(dvm_object_class(v,h3.l)!=plain_class||h3.l==plain)clone_contract=false;
 if(!monitor_contract||!clone_contract)return false;
 sdk_trace_fprintf(stderr,"OBJECT_RUNTIME_SELFTEST PASS exact_class=1 int_long_min=1 timeunit_saturation=1 default_tostring=1 default_hashcode=1 wait_notify=1 clone=1\n");return !dvm_exception(v);
}
