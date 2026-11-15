/* Stateful subset, not a GC or Android ReferenceQueue implementation.
 * Upstream Throwable contract reviewed in Lunaria dvm_runtime.c th_init.
 * This implementation does not ignore missing field writes.
 */
#include "throwable_reference.h"
#include "gc_runtime.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <hilog/log.h>
extern int sdk_trace_fprintf(FILE*,const char*,...);
typedef struct {struct dvm* vm;dvm_ref message,cause,referent;dvm_gc_handle referent_key;bool cause_set,weak;int stack_count;struct dvm_method** stack;} state;
static void destroy(void* p){state* s=p;if(s){if(s->message)dvm_unpin(s->vm,s->message);if(s->cause)dvm_unpin(s->vm,s->cause);if(s->referent_key)dvm_gc_handle_drop(s->vm,s->referent_key);free(s->stack);free(s);}}
static state* data(struct dvm_object* o){return o&&o->data_destroy==destroy?o->data:NULL;}
bool throwable_reference_gc_visit_owner(struct dvm *v,dvm_ref owner,dvm_gc_visit_fn visit,void *user){
 if(!v||!visit||!dvm_gil_held(v))return false;state *s=data(dvm__obj(v,owner));if(!s||s->vm!=v)return false;
 if(s->message){if(!dvm__obj(v,s->message))return false;visit(&s->message,user);}
 if(s->cause){if(!dvm__obj(v,s->cause))return false;visit(&s->cause,user);}return true;
}
static bool derives(struct dvm_class* c,const char* name){for(;c;c=c->super)if(!strcmp(c->name,name))return true;return false;}
void throwable_reference_vm_start(struct dvm* v){
 static const char* pairs[][2]={{"java/lang/Throwable","java/lang/Object"},{"java/lang/Exception","java/lang/Throwable"},{"java/lang/RuntimeException","java/lang/Exception"},{"java/lang/Error","java/lang/Throwable"},{"java/lang/IllegalArgumentException","java/lang/RuntimeException"},{"java/lang/IllegalStateException","java/lang/RuntimeException"},{"java/lang/NullPointerException","java/lang/RuntimeException"},{"java/lang/UnsupportedOperationException","java/lang/RuntimeException"},{"java/lang/ref/Reference","java/lang/Object"},{"java/lang/ref/WeakReference","java/lang/ref/Reference"},{"java/lang/ref/SoftReference","java/lang/ref/Reference"}};
 for(size_t i=0;i<sizeof pairs/sizeof pairs[0];i++){
  struct dvm_class* c=dvm_find_class_or_external(v,pairs[i][0]);struct dvm_class* parent=dvm_find_class_or_external(v,pairs[i][1]);
  if(c&&parent&&c->external)c->super=parent;
 }
}
#define IS(n,s) (!strcmp(m,n)&&!strcmp(sig,s))
/* Throwable.getStackTrace() reports the VM frames captured when this runtime
 * constructed the Throwable.  vm->callstack is outermost first, so the array
 * is filled from stack_count-1 down to 0 and element 0 is the innermost frame.
 * Each element is a real java.lang.StackTraceElement with the declaring class
 * in Java's dotted form and the recorded method name.  The DEX carries no
 * source-file or line map, so fileName is null and lineNumber is -1 (Java's
 * "unavailable" values); neither is invented. */
static dvm_ref throwable_stack_element(struct dvm *v,struct dvm_method *frame){
 const char *class_name=frame->cls->name;size_t length=strlen(class_name);
 char *dotted=malloc(length+1);
 if(!dotted){dvm__throw(v,"java/lang/OutOfMemoryError","stack frame class name");return 0;}
 for(size_t i=0;i<length;i++)dotted[i]=class_name[i]=='/'?'.':class_name[i];
 dotted[length]='\0';
 dvm_ref declaring=dvm_new_string(v,dotted);
 free(dotted);
 if(!declaring){dvm__throw(v,"java/lang/OutOfMemoryError","stack frame class name string");return 0;}
 dvm_ref method=dvm_new_string(v,frame->name);
 if(!method){dvm__discard_temporary(v,declaring);dvm__throw(v,"java/lang/OutOfMemoryError","stack frame method name");return 0;}
 struct dvm_class *cls=dvm__class_by_desc(v,"Ljava/lang/StackTraceElement;");
 if(!cls){dvm__discard_temporary(v,declaring);dvm__discard_temporary(v,method);dvm__throw(v,"java/lang/IllegalStateException","StackTraceElement class unavailable");return 0;}
 dvm_ref element=dvm_new_object(v,cls);
 if(!element){dvm__discard_temporary(v,declaring);dvm__discard_temporary(v,method);dvm__throw(v,"java/lang/OutOfMemoryError","StackTraceElement");return 0;}
 union dvm_value value;bool ok=true;
 value.l=declaring;ok=ok&&dvm_set_field(v,element,"declaringClass","Ljava/lang/String;",value);
 value.l=method;ok=ok&&dvm_set_field(v,element,"methodName","Ljava/lang/String;",value);
 value.l=0;ok=ok&&dvm_set_field(v,element,"fileName","Ljava/lang/String;",value);
 value.i=-1;ok=ok&&dvm_set_field(v,element,"lineNumber","I",value);
 if(!ok){
  dvm__discard_temporary(v,element);dvm__discard_temporary(v,method);dvm__discard_temporary(v,declaring);
  dvm__throw(v,"java/lang/IllegalStateException","StackTraceElement fields unavailable");return 0;
 }
 return element;
}
/* Release one built element on the failure path, including the two Strings its
 * slots reference: heap_free() releases the object and slot array only, not
 * the referenced String objects. */
static void throwable_stack_element_discard(struct dvm *v,dvm_ref element){
 union dvm_value value={0};
 if(dvm_get_field(v,element,"declaringClass","Ljava/lang/String;",&value)&&value.l)dvm__discard_temporary(v,value.l);
 value.l=0;
 if(dvm_get_field(v,element,"methodName","Ljava/lang/String;",&value)&&value.l)dvm__discard_temporary(v,value.l);
 dvm__discard_temporary(v,element);
}
static bool throwable_get_stack_trace(struct dvm *v,state *s,union dvm_value *out){
 uint32_t count=0;
 if(s&&s->stack)for(int i=0;i<s->stack_count;i++){struct dvm_method *frame=s->stack[i];if(frame&&frame->cls&&frame->cls->name&&frame->name)++count;}
 /* Java allows a Throwable whose stack is unavailable: an absent state or an
  * empty snapshot yields a real zero-length array, never null. */
 dvm_ref array=dvm_new_array(v,'L',"Ljava/lang/StackTraceElement;",count);
 if(!array){dvm__throw(v,"java/lang/OutOfMemoryError","Throwable.getStackTrace array");return true;}
 if(!count){out->l=array;return true;}
 dvm_ref *items=(dvm_ref*)dvm_array_data(v,array);
 if(!items){dvm__discard_temporary(v,array);dvm__throw(v,"java/lang/OutOfMemoryError","Throwable.getStackTrace array data");return true;}
 uint32_t at=0;
 for(int i=s->stack_count-1;i>=0;--i){
  struct dvm_method *frame=s->stack[i];
  if(!frame||!frame->cls||!frame->cls->name||!frame->name)continue;
  dvm_ref element=throwable_stack_element(v,frame);
  if(!element){
   for(uint32_t j=0;j<at;j++)throwable_stack_element_discard(v,items[j]);
   dvm__discard_temporary(v,array);
   if(!dvm_exception(v))dvm__throw(v,"java/lang/OutOfMemoryError","Throwable.getStackTrace element");
   return true;
  }
  items[at++]=element;
 }
 out->l=array;return true;
}
bool throwable_reference_call(struct dvm* v,const char* c,const char* m,const char* sig,dvm_ref self,const union dvm_value* a,int nargs,union dvm_value* out){
 struct dvm_object* o=dvm__obj(v,self);
 if(!o)return false;
 /* SoftReference 与 WeakReference 共用同一宿主实现：本 VM 没有内存压力
  * 信号，软引用在宿主对象被回收后同样返回 null（明确的实现选择，不伪造
  * 保留语义）。B 站渠道悬浮球 SDK 使用 SoftReference 缓存 Activity。 */
 if((!strcmp(c,"java/lang/ref/WeakReference")||!strcmp(c,"java/lang/ref/SoftReference")||!strcmp(c,"java/lang/ref/Reference"))&&derives(o->cls,"java/lang/ref/Reference")){
  state* s=data(o);
  if(IS("<init>","(Ljava/lang/Object;)V")&&nargs==1){
   if(o->data){dvm__throw(v,"java/lang/IllegalStateException","reference already initialized");return true;}
   s=calloc(1,sizeof *s);if(!s){dvm__throw(v,"java/lang/OutOfMemoryError","weak reference");return true;}
   s->vm=v;s->weak=true;s->referent=a[0].l;s->referent_key=a[0].l?dvm_gc_handle_new(v,a[0].l,DVM_GC_HANDLE_WEAK):0;
   if(a[0].l&&!s->referent_key){free(s);return true;}o->data=s;o->data_destroy=destroy;return true;
  }
  if(!s||!s->weak)return false;
  if(IS("get","()Ljava/lang/Object;")){s->referent=s->referent_key?dvm_gc_handle_ref(v,s->referent_key):0;out->l=s->referent;return true;}
  if(IS("clear","()V")){if(s->referent_key)dvm_gc_handle_drop(v,s->referent_key);s->referent_key=0;s->referent=0;return true;}
  if(IS("refersTo","(Ljava/lang/Object;)Z")){s->referent=s->referent_key?dvm_gc_handle_ref(v,s->referent_key):0;out->i=s->referent==a[0].l;return true;}
  return false;
 }
 struct dvm_class* target=dvm_find_class_or_external(v,c);
 if(!target||!derives(target,"java/lang/Throwable")||!derives(o->cls,c)){
  /* The same corruption that overwrites class name buffers also shows up here
    * as a missing Throwable in the RuntimeException chain (72ce: six
    * RouteException.<init> failures with no CLASS_CORRUPTION report).  Print
    * the actual chain and the field values the decision used, so a broken
    * super link is distinguishable from a damaged name. */
  if(!strcmp(c,"java/lang/RuntimeException")||!strcmp(c,"java/lang/Exception")){
   char chain[256];size_t at=0;
   for(struct dvm_class* k=target;k&&at<sizeof chain-1;k=k->super){
    int n=snprintf(chain+at,sizeof chain-at,"%s->",k->name?k->name:"?");
    if(n<=0)break;at+=(size_t)n;
   }
   chain[at]=0;
   sdk_trace_fprintf(stderr,"THROWABLE_ENTRY_REJECT c=%s target=%p external=%d super_ptr=%p super_name=%s chain=%s receiver=%s derives_target=%d derives_receiver=%d\n",
    c,(void*)target,target?target->external:0,(void*)(target?target->super:0),
    target&&target->super&&target->super->name?target->super->name:"?",
    chain,o->cls&&o->cls->name?o->cls->name:"?",
    target?derives(target,"java/lang/Throwable"):0,derives(o->cls,c));
  }
  return false;
 }
 state* s=data(o);
 if(!strcmp(m,"<init>")){
  dvm_ref message=0,cause=0;bool explicit_cause=false;
  if(!strcmp(sig,"()V")&&nargs==0){}
  else if(!strcmp(sig,"(Ljava/lang/String;)V")&&nargs==1)message=a[0].l;
  else if(!strcmp(sig,"(Ljava/lang/Throwable;)V")&&nargs==1){cause=a[0].l;explicit_cause=true;if(cause){char text[2048];dvm_describe_exception(v,cause,text,sizeof text);message=dvm_new_string(v,text);}}
  else if(!strcmp(sig,"(Ljava/lang/String;Ljava/lang/Throwable;)V")&&nargs==2){message=a[0].l;cause=a[1].l;explicit_cause=true;}
  else return false;
  if(o->data){dvm__throw(v,"java/lang/IllegalStateException","throwable already initialized");return true;}
  if(message&&!dvm_string_utf8(v,message)){dvm__throw(v,"java/lang/IllegalArgumentException","throwable message type");return true;}
  if(cause&&!derives(dvm_object_class(v,cause),"java/lang/Throwable")){dvm__throw(v,"java/lang/IllegalArgumentException","throwable cause type");return true;}
  s=calloc(1,sizeof *s);if(!s){dvm__throw(v,"java/lang/OutOfMemoryError","throwable state");return true;}
  s->stack_count=v->ncallstack;
  if(s->stack_count){s->stack=malloc((size_t)s->stack_count*sizeof *s->stack);if(!s->stack){free(s);dvm__throw(v,"java/lang/OutOfMemoryError","throwable stack");return true;}memcpy(s->stack,v->callstack,(size_t)s->stack_count*sizeof *s->stack);}
  s->vm=v;s->message=message;s->cause=cause;s->cause_set=explicit_cause;
  if(message){const char* text=dvm_string_utf8(v,message);char* copy=strdup(text);if(!copy){free(s->stack);free(s);dvm__throw(v,"java/lang/OutOfMemoryError","throwable message");return true;}free(o->utf8);o->utf8=copy;dvm_pin(v,message);}
  if(cause)dvm_pin(v,cause);
  o->data=s;o->data_destroy=destroy;return true;
 }
 if(IS("getMessage","()Ljava/lang/String;")||IS("getLocalizedMessage","()Ljava/lang/String;")){out->l=s?s->message:o->utf8?dvm_new_string(v,o->utf8):0;return true;}
 if(IS("getCause","()Ljava/lang/Throwable;")){out->l=s?s->cause:0;return true;}
 if(IS("getStackTrace","()[Ljava/lang/StackTraceElement;"))return throwable_get_stack_trace(v,s,out);
 if(IS("printStackTrace","()V")){
  char text[4096];dvm_describe_exception(v,self,text,sizeof text);
  OH_LOG_Print(LOG_APP,LOG_ERROR,0xB011,"GuestThrowable","%{private}s",text);
  if(s)for(int i=s->stack_count-1;i>=0;i--){struct dvm_method* frame=s->stack[i];if(frame)OH_LOG_Print(LOG_APP,LOG_ERROR,0xB011,"GuestThrowable","at %{private}s.%{private}s",frame->cls?frame->cls->name:"?",frame->name);}
  return true;
 }
 if(IS("toString","()Ljava/lang/String;")){char text[4096];dvm_describe_exception(v,self,text,sizeof text);out->l=dvm_new_string(v,text);if(!out->l)dvm__throw(v,"java/lang/OutOfMemoryError","Throwable.toString result");return true;}
 if(IS("initCause","(Ljava/lang/Throwable;)Ljava/lang/Throwable;")&&nargs==1){
  if(!s||s->cause_set){dvm__throw(v,"java/lang/IllegalStateException","cause already initialized or foreign throwable");return true;}
  if(a[0].l==self){dvm__throw(v,"java/lang/IllegalArgumentException","self cause");return true;}
  if(a[0].l&&!derives(dvm_object_class(v,a[0].l),"java/lang/Throwable")){dvm__throw(v,"java/lang/IllegalArgumentException","cause type");return true;}
  s->cause=a[0].l;s->cause_set=true;if(s->cause)dvm_pin(v,s->cause);out->l=self;return true;
 }
 return false;
}
static bool throwable_selftest_stack_array(struct dvm *v,dvm_ref error,uint32_t want_count,dvm_ref *array_out){
 union dvm_value out={0};
 if(!throwable_reference_call(v,"java/lang/Throwable","getStackTrace","()[Ljava/lang/StackTraceElement;",error,NULL,0,&out)||dvm_exception(v)){
  sdk_trace_fprintf(stderr,"THROWABLE_REFERENCE_SELFTEST_FAIL getStackTrace_call=1\n");return false;
 }
 struct dvm_object *array=out.l?dvm__obj(v,out.l):NULL;
 if(!out.l||!array||array->kind!=DVM_OBJ_ARRAY||array->elem_kind!='L'||array->length!=want_count){
  sdk_trace_fprintf(stderr,"THROWABLE_REFERENCE_SELFTEST_FAIL getStackTrace_array nonnull=%d want=%u got=%u\n",out.l!=0,(unsigned)want_count,array?array->length:0);return false;
 }
 if(array_out)*array_out=out.l;
 return true;
}
static bool throwable_selftest_element(struct dvm *v,dvm_ref element,const char *want_class,const char *want_method){
 union dvm_value field={0};
 const char *text;
 if(!dvm_get_field(v,element,"declaringClass","Ljava/lang/String;",&field)||!field.l)return false;
 text=dvm_string_utf8(v,field.l);
 if(!text||strcmp(text,want_class))return false;
 field.l=0;
 if(!dvm_get_field(v,element,"methodName","Ljava/lang/String;",&field)||!field.l)return false;
 text=dvm_string_utf8(v,field.l);
 if(!text||strcmp(text,want_method))return false;
 field.l=0;
 if(!dvm_get_field(v,element,"fileName","Ljava/lang/String;",&field)||field.l)return false;
 field.l=0;
 return dvm_get_field(v,element,"lineNumber","I",&field)&&field.i==-1;
}
static bool throwable_selftest_element_readable(struct dvm *v,dvm_ref element){
 union dvm_value field={0};
 if(!dvm_get_field(v,element,"declaringClass","Ljava/lang/String;",&field)||!field.l)return false;
 const char *name=dvm_string_utf8(v,field.l);
 if(!name||!*name||strchr(name,'/'))return false;
 field.l=0;
 if(!dvm_get_field(v,element,"methodName","Ljava/lang/String;",&field)||!field.l)return false;
 if(!dvm_string_utf8(v,field.l))return false;
 field.l=0;
 if(!dvm_get_field(v,element,"fileName","Ljava/lang/String;",&field)||field.l)return false;
 field.l=0;
 return dvm_get_field(v,element,"lineNumber","I",&field)&&field.i==-1;
}
bool throwable_reference_selftest(struct dvm* v){
 union dvm_value out={0},args[2]={{.l=dvm_new_string(v,"fixture-message")},{0}};
 struct dvm_class* type=dvm_find_class_or_external(v,"java/lang/RuntimeException");dvm_ref error=type?dvm_new_object(v,type):0;
 if(!error||!args[0].l||!throwable_reference_call(v,"java/lang/RuntimeException","<init>","(Ljava/lang/String;)V",error,args,1,&out))return false;
 if(!throwable_reference_call(v,"java/lang/Throwable","getMessage","()Ljava/lang/String;",error,NULL,0,&out)||out.l!=args[0].l)return false;
 /* getStackTrace contract for a Throwable built through this runtime: a real
  * non-null array whose length is the snapshot length (possibly zero outside
  * dvm_call(), where the VM has no guest frames), with readable fields and no
  * invented file/line information. */
 struct dvm_object *error_object=dvm__obj(v,error);
 state *error_state=data(error_object);
 uint32_t snapshot_count=0;
 if(error_state&&error_state->stack)for(int i=0;i<error_state->stack_count;i++){struct dvm_method *frame=error_state->stack[i];if(frame&&frame->cls&&frame->cls->name&&frame->name)++snapshot_count;}
 dvm_ref trace=0;
 if(!throwable_selftest_stack_array(v,error,snapshot_count,&trace))return false;
 if(snapshot_count){
  dvm_ref *items=(dvm_ref*)dvm_array_data(v,trace);
  if(!items){sdk_trace_fprintf(stderr,"THROWABLE_REFERENCE_SELFTEST_FAIL getStackTrace_storage=1\n");return false;}
  for(uint32_t i=0;i<snapshot_count;i++)if(!items[i]||!throwable_selftest_element_readable(v,items[i])){sdk_trace_fprintf(stderr,"THROWABLE_REFERENCE_SELFTEST_FAIL getStackTrace_element=%u\n",i);return false;}
 }
 /* A Throwable created by dvm__throw() has no state snapshot; Java permits an
  * unavailable stack, but the result is still a zero-length array, not null. */
 dvm__throw(v,"java/lang/IllegalStateException","stackless fixture");
 dvm_ref stackless=dvm_exception(v);dvm_clear_exception(v);
 if(!stackless||!throwable_selftest_stack_array(v,stackless,0,&trace)){
  sdk_trace_fprintf(stderr,"THROWABLE_REFERENCE_SELFTEST_FAIL getStackTrace_stackless=1\n");return false;
 }
 /* Synthetic VM frames make ordering deterministic without guest bytecode:
  * callstack[0] is outermost and callstack[1] innermost, so the array must be
  * [Inner.run, Outer.outer] with dotted declaring-class names. */
 static struct dvm_method frames[2];
 struct dvm_class *outer_class=dvm_find_class_or_external(v,"com/example/Outer");
 struct dvm_class *inner_class=dvm_find_class_or_external(v,"com/example/Inner");
 if(!outer_class||!inner_class)return false;
 memset(frames,0,sizeof frames);
 frames[0].cls=outer_class;frames[0].name="outer";
 frames[1].cls=inner_class;frames[1].name="run";
 struct dvm_method *saved[128];
 int saved_count=v->ncallstack;
 if(saved_count<0||saved_count>128)return false;
 for(int i=0;i<saved_count;i++)saved[i]=v->callstack[i];
 v->callstack[0]=&frames[0];v->callstack[1]=&frames[1];v->ncallstack=2;
 struct dvm_class *nested_type=dvm_find_class_or_external(v,"java/lang/RuntimeException");
 dvm_ref nested=nested_type?dvm_new_object(v,nested_type):0;
 bool constructed=nested&&throwable_reference_call(v,"java/lang/RuntimeException","<init>","()V",nested,NULL,0,&out)&&!dvm_exception(v);
 for(int i=0;i<saved_count;i++)v->callstack[i]=saved[i];
 v->ncallstack=saved_count;
 if(!constructed){sdk_trace_fprintf(stderr,"THROWABLE_REFERENCE_SELFTEST_FAIL getStackTrace_fixture_construct=1\n");return false;}
 if(!throwable_selftest_stack_array(v,nested,2,&trace))return false;
 dvm_ref *trace_items=(dvm_ref*)dvm_array_data(v,trace);
 if(!trace_items||!throwable_selftest_element(v,trace_items[0],"com.example.Inner","run")||!throwable_selftest_element(v,trace_items[1],"com.example.Outer","outer")){
  sdk_trace_fprintf(stderr,"THROWABLE_REFERENCE_SELFTEST_FAIL getStackTrace_innermost_first=1\n");return false;
 }
 /* Wrong signature and a non-Throwable receiver stay unhandled and leave no
  * exception: the VM must not fabricate a stack for them. */
 union dvm_value int_arg={.i=0},ignored={0};
 if(throwable_reference_call(v,"java/lang/Throwable","getStackTrace","(I)[Ljava/lang/StackTraceElement;",nested,&int_arg,1,&ignored)||dvm_exception(v)){
  sdk_trace_fprintf(stderr,"THROWABLE_REFERENCE_SELFTEST_FAIL getStackTrace_wrong_signature=1\n");return false;
 }
 dvm_ref foreign=dvm_new_string(v,"not-a-throwable");
 if(!foreign||throwable_reference_call(v,"java/lang/Throwable","getStackTrace","()[Ljava/lang/StackTraceElement;",foreign,NULL,0,&ignored)||dvm_exception(v)){
  sdk_trace_fprintf(stderr,"THROWABLE_REFERENCE_SELFTEST_FAIL getStackTrace_foreign_receiver=1\n");return false;
 }
 type=dvm_find_class_or_external(v,"java/lang/ref/WeakReference");dvm_ref weak=type?dvm_new_object(v,type):0;
 struct dvm_object* referent=dvm__obj(v,error);uint32_t pins=referent->pins;args[0].l=error;
 if(!weak||!throwable_reference_call(v,"java/lang/ref/WeakReference","<init>","(Ljava/lang/Object;)V",weak,args,1,&out)||referent->pins!=pins)return false;
 if(!throwable_reference_call(v,"java/lang/ref/WeakReference","get","()Ljava/lang/Object;",weak,NULL,0,&out)||out.l!=error)return false;
 if(!throwable_reference_call(v,"java/lang/ref/WeakReference","clear","()V",weak,NULL,0,&out))return false;
 if(!(throwable_reference_call(v,"java/lang/ref/WeakReference","get","()Ljava/lang/Object;",weak,NULL,0,&out)&&!out.l&&!dvm_exception(v)))return false;
 sdk_trace_fprintf(stderr,"THROWABLE_REFERENCE_SELFTEST PASS stack_trace=1 stack_trace_empty=1 stack_trace_order=1 stack_trace_fields=1 stack_trace_signature=1 weak_reference=1\n");
 return true;
}

int throwable_gc_payload(struct dvm *v,struct dvm_object *o,dvm_gc_visit_fn visit,void *u){
 if(o->data_destroy!=destroy)return DVM_GC_PAYLOAD_UNKNOWN;state *s=o->data;if(!s||s->vm!=v)return DVM_GC_PAYLOAD_INVALID;
 if(visit){visit(&s->message,u);visit(&s->cause,u);if(!s->weak)visit(&s->referent,u);}return DVM_GC_PAYLOAD_REFS;
}
