#include "native_scratch.h"
#include "gc_runtime.h"
#include "reflection_runtime.h"
#include "gc_operation.h"
#include "prefs_runtime.h"
#include "java_basics.h"
#include "small_boxes.h"
#include "perf_attr.h"
#include "leak_scope.h"
#include "throwable_reference.h"
#include "discovery_runtime.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
extern int sdk_trace_fprintf(FILE*,const char*,...);
extern const char *sdk_display_source(void);

static struct dvm *active;
/* Class.getPackage() must return a real java.lang.Package carrying the
 * package name; DEX callers immediately invoke Package.getName(). */
static struct dvm *package_vm;
static struct dvm_class *package_class;
static void install_package_class(struct dvm *v);
static void install_fixture_method(struct dvm_class *c,int index,const char *name,const char *sig,uint32_t access,dvm_builtin_fn fn,char ret_kind);
/* A real constructor-free allocation primitive. Gson discovers this through
 * ordinary Class/Field/Method reflection; no application types are special. */
static bool unsafe_allocate_instance(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 (void)self;out->l=0;
 const dvm_ref gc_ref_co_0=((n==1)?(a[0].l):0); if(!gc_operation_admit(v,gc_ref_co_0))return false;
 if(!dvm__obj(v,gc_ref_co_0)){dvm__throw(v,"java/lang/NullPointerException","allocateInstance class");return false;}
 if(dvm__obj(v,gc_ref_co_0)->kind!=DVM_OBJ_CLASS||!dvm__obj(v,gc_ref_co_0)->klass){dvm__throw(v,"java/lang/IllegalArgumentException","allocateInstance requires Class");return false;}
 struct dvm_class *k=dvm__obj(v,gc_ref_co_0)->klass;
 if(k->is_primitive||k->is_array||k->is_interface||(k->access&(DEX_ACC_INTERFACE|DEX_ACC_ABSTRACT))){dvm__throw(v,"java/lang/InstantiationException","%s",k->name);return false;}
 if(!dvm_init_class(v,k))return false;
 out->l=dvm_new_object(v,k);
 if(!out->l){dvm__throw(v,"java/lang/OutOfMemoryError","allocateInstance %s",k->name);return false;}
 return true;
}
static bool install_unsafe_class(struct dvm *v){
 struct dvm_class *k=dvm_find_class_or_external(v,"sun/misc/Unsafe");
 if(!k)return false;
 if(k->nmethods)return true;
 k->access=DEX_ACC_PUBLIC|DEX_ACC_FINAL;k->synthesized=false;
 k->methods=calloc(1,sizeof *k->methods);k->sfields=calloc(1,sizeof *k->sfields);k->sslots=calloc(1,sizeof *k->sslots);
 if(!k->methods||!k->sfields||!k->sslots){dvm__throw(v,"java/lang/OutOfMemoryError","Unsafe metadata");return false;}
 k->nmethods=1;k->nsfields=1;k->nsslots=1;
 install_fixture_method(k,0,"allocateInstance","(Ljava/lang/Class;)Ljava/lang/Object;",DEX_ACC_PUBLIC|DEX_ACC_NATIVE,unsafe_allocate_instance,'L');
 k->methods[0].arg_count=1;k->methods[0].arg_slots=1;
 if(!k->methods[0].sig){dvm__throw(v,"java/lang/OutOfMemoryError","Unsafe signature");return false;}
 k->sfields[0]=(struct dvm_field){.name="theUnsafe",.type="Lsun/misc/Unsafe;",.access=DEX_ACC_PRIVATE|DEX_ACC_STATIC|DEX_ACC_FINAL,.cls=k,.slot=0,.width=1,.kind='L'};
 k->sslots[0].l=dvm_new_object(v,k);
 return k->sslots[0].l!=0;
}
/* Bound the real-dex lookup evidence: the guest often swallows a reflection
 * exception and silently drops a field, so a device run needs to show that
 * the getter name built by params2BodyMap actually matched a dex method. */
static int real_getmethod_log;
typedef struct {struct dvm *vm;dvm_ref raw,owner,args;} generic_type;
typedef struct {struct dvm_class *declaration;char *name;} generic_variable;
static void variable_free(void *data){generic_variable *t=data;if(t){free(t->name);free(t);}}
static bool variable_equals_builtin(struct dvm *v,dvm_ref self,const union dvm_value *args,int n,union dvm_value *out){return reflection_runtime_call(v,"sdktrace/reflect/TypeVariable","equals","(Ljava/lang/Object;)Z",self,args,n,out);}
static bool variable_hash_builtin(struct dvm *v,dvm_ref self,const union dvm_value *args,int n,union dvm_value *out){return reflection_runtime_call(v,"sdktrace/reflect/TypeVariable","hashCode","()I",self,args,n,out);}
static bool variable_text_builtin(struct dvm *v,dvm_ref self,const union dvm_value *args,int n,union dvm_value *out){return reflection_runtime_call(v,"sdktrace/reflect/TypeVariable","toString","()Ljava/lang/String;",self,args,n,out);}
static bool install_variable_methods(struct dvm *v,struct dvm_class *impl){if(impl->nmethods)return true;impl->methods=calloc(3,sizeof *impl->methods);if(!impl->methods){dvm__throw(v,"java/lang/OutOfMemoryError","type variable methods");return false;}impl->nmethods=3;install_fixture_method(impl,0,"equals","(Ljava/lang/Object;)Z",DEX_ACC_PUBLIC,variable_equals_builtin,'Z');install_fixture_method(impl,1,"hashCode","()I",DEX_ACC_PUBLIC,variable_hash_builtin,'I');install_fixture_method(impl,2,"toString","()Ljava/lang/String;",DEX_ACC_PUBLIC,variable_text_builtin,'L');return true;}
static const char *class_signature(struct dvm_class *k,char *buffer,size_t size){if(k->runtime_signature)return k->runtime_signature;return k->dex&&dex_class_signature(k->dex,k->class_def_idx,buffer,size)?buffer:NULL;}
static const char *skip_signature_type(const char *p){if(!p)return NULL;while(*p=='[')p++;if(*p=='L'){int nesting=0;do{if(*p=='<')nesting++;else if(*p=='>')nesting--;else if(*p==';'&&!nesting)return p+1;p++;}while(*p);return NULL;}if(*p=='T'){const char *end=strchr(p,';');return end?end+1:NULL;}return NULL;}
static bool formal_names(struct dvm_class *k,const char *wanted,char names[32][128],int *count){char buffer[4096];const char *p=class_signature(k,buffer,sizeof buffer);*count=0;if(!p||*p!='<')return !wanted;p++;while(*p&&*p!='>'){const char *colon=strchr(p,':');if(!colon||colon==p||colon-p>=128||*count>=32)return false;size_t n=(size_t)(colon-p);memcpy(names[*count],p,n);names[(*count)++][n]=0;p=colon;do{p++;if(*p!=':'){p=skip_signature_type(p);if(!p)return false;}}while(*p==':');}if(*p!='>')return false;if(wanted){for(int i=0;i<*count;i++)if(!strcmp(names[i],wanted))return true;return false;}return true;}
static dvm_ref new_variable(struct dvm *v,struct dvm_class *declaration,const char *name){char names[32][128];int count;if(!declaration||!formal_names(declaration,name,names,&count)){dvm__throw(v,"java/lang/reflect/GenericSignatureFormatError","undeclared type variable %s",name);return 0;}struct dvm_class *iface=dvm_find_class_or_external(v,"java/lang/reflect/TypeVariable"),*impl=dvm_find_class_or_external(v,"sdktrace/reflect/TypeVariable");iface->is_interface=true;iface->access|=DEX_ACC_INTERFACE|DEX_ACC_ABSTRACT;if(!iface->ifaces){iface->ifaces=calloc(1,sizeof *iface->ifaces);if(!iface->ifaces)return 0;iface->ifaces[0]=dvm_find_class_or_external(v,"java/lang/reflect/Type");iface->nifaces=1;}if(!impl->ifaces){impl->ifaces=calloc(1,sizeof *impl->ifaces);if(!impl->ifaces)return 0;impl->ifaces[0]=iface;impl->nifaces=1;}if(!install_variable_methods(v,impl))return 0;dvm_ref result=dvm_new_object(v,impl);const dvm_ref gc_ref_o_1=(result); if(!gc_operation_admit(v,gc_ref_o_1))return false;generic_variable *t=calloc(1,sizeof *t);if(!dvm__obj(v,gc_ref_o_1)||!t){free(t);dvm__throw(v,"java/lang/OutOfMemoryError","type variable");return 0;}t->declaration=declaration;t->name=strdup(name);if(!t->name){free(t);dvm__throw(v,"java/lang/OutOfMemoryError","type variable name");return 0;}dvm__obj(v,gc_ref_o_1)->data=t;dvm__obj(v,gc_ref_o_1)->data_destroy=variable_free;return result;}
static void generic_free(void *data){generic_type *t=data;if(t){dvm_unpin(t->vm,t->raw);if(t->owner)dvm_unpin(t->vm,t->owner);dvm_unpin(t->vm,t->args);free(t);}}
/* Parse class signatures and class-declared type variables from DEX Signature.
 * Wildcards and generic arrays remain explicit unsupported cases. */
static dvm_ref generic_parse(struct dvm *v,const char **cursor,int depth,struct dvm_class *declaration){
 const char *p=*cursor;if(depth<=16&&*p=='T'){const char *end=strchr(p,';');if(!end||end-p<2||end-p>128){dvm__throw(v,"java/lang/reflect/GenericSignatureFormatError","type variable signature");return 0;}char name[128];memcpy(name,p+1,(size_t)(end-p-1));name[end-p-1]=0;dvm_ref result=new_variable(v,declaration,name);if(result)*cursor=end+1;return result;}if(depth>16||*p!='L'){dvm__throw(v,"java/lang/UnsupportedOperationException","generic signature requires class or declared variable");return 0;}
 const char *start=++p;while(*p&&*p!='<'&&*p!=';')p++;
 size_t len=(size_t)(p-start);if(!len||len>1022||!*p){dvm__throw(v,"java/lang/reflect/GenericSignatureFormatError","class signature");return 0;}
 char desc[1025];desc[0]='L';memcpy(desc+1,start,len);desc[len+1]=';';desc[len+2]=0;
 struct dvm_class *raw=dvm__class_by_desc(v,desc);if(!raw)return 0;
 dvm_ref raw_ref=dvm_class_object(v,raw);if(*p==';'){*cursor=p+1;return raw_ref;}
 p++;dvm_ref args[32];uint32_t count=0;
 while(*p&&*p!='>'){if(count==32){dvm__throw(v,"java/lang/UnsupportedOperationException","generic argument limit");return 0;}args[count]=generic_parse(v,&p,depth+1,declaration);if(!args[count++])return 0;}
 if(!count||*p++!='>'||*p++!=';'){dvm__throw(v,"java/lang/reflect/GenericSignatureFormatError","parameterized signature");return 0;}
 dvm_ref array=dvm_new_array(v,'L',"Ljava/lang/reflect/Type;",count);if(!array)return 0;memcpy(({const dvm_ref gc_payload_ref_0=(array);if(!gc_operation_admit(v,gc_payload_ref_0))return 0;dvm_array_data(v,gc_payload_ref_0);}),args,count*sizeof *args);
 struct dvm_class *impl=dvm_find_class_or_external(v,"sdktrace/reflect/ParameterizedType");
 struct dvm_class *iface=dvm_find_class_or_external(v,"java/lang/reflect/ParameterizedType");iface->is_interface=true;iface->access|=DEX_ACC_INTERFACE|DEX_ACC_ABSTRACT;
 if(!iface->ifaces){iface->ifaces=calloc(1,sizeof *iface->ifaces);if(!iface->ifaces){dvm__throw(v,"java/lang/OutOfMemoryError","Type interface");return 0;}iface->ifaces[0]=dvm_find_class_or_external(v,"java/lang/reflect/Type");iface->nifaces=1;iface->ifaces[0]->is_interface=true;}
 if(!impl->ifaces){impl->ifaces=calloc(1,sizeof *impl->ifaces);if(!impl->ifaces){dvm__throw(v,"java/lang/OutOfMemoryError","parameterized interfaces");return 0;}impl->ifaces[0]=dvm_find_class_or_external(v,"java/lang/reflect/ParameterizedType");impl->nifaces=1;}
 dvm_ref result=dvm_new_object(v,impl);const dvm_ref gc_ref_o_2=(result); if(!gc_operation_admit(v,gc_ref_o_2))return false;generic_type *t=calloc(1,sizeof *t);
 if(!dvm__obj(v,gc_ref_o_2)||!t){free(t);dvm__throw(v,"java/lang/OutOfMemoryError","parameterized type");return 0;}
 t->vm=v;t->raw=raw_ref;t->args=array;
 const char *owner=raw->dex?dex_class_enclosing_type(raw->dex,raw->class_def_idx):NULL;
 if(owner){struct dvm_class *k=dvm__class_by_desc(v,owner);if(k)t->owner=dvm_class_object(v,k);}
 dvm_pin(v,t->raw);dvm_pin(v,t->args);if(t->owner)dvm_pin(v,t->owner);dvm__obj(v,gc_ref_o_2)->data=t;dvm__obj(v,gc_ref_o_2)->data_destroy=generic_free;*cursor=p;return result;
}
static bool generic_superclass(struct dvm *v,dvm_ref self,bool interfaces,union dvm_value *out){
 const dvm_ref gc_ref_co_3=(self); if(!gc_operation_admit(v,gc_ref_co_3))return false;if(!dvm__obj(v,gc_ref_co_3)||dvm__obj(v,gc_ref_co_3)->kind!=DVM_OBJ_CLASS||!dvm__obj(v,gc_ref_co_3)->klass){dvm__throw(v,"java/lang/NullPointerException","generic superclass receiver");return true;}
 struct dvm_class *k=dvm__obj(v,gc_ref_co_3)->klass;if(!interfaces&&(k->is_primitive||k->is_interface)){out->l=0;return true;}
 char signature[4096];const char *p=k->runtime_signature;
 if(!p&&k->dex&&dex_class_signature(k->dex,k->class_def_idx,signature,sizeof signature))p=signature;
 if(!p||!*p){
  if(interfaces){uint32_t count=k->is_primitive?0:k->is_array?2:(uint32_t)k->nifaces;out->l=dvm_new_array(v,'L',"Ljava/lang/reflect/Type;",count);if(!out->l)return true;for(uint32_t i=0;i<count;i++){struct dvm_class *iface=k->is_array?dvm_find_class_or_external(v,i?"java/io/Serializable":"java/lang/Cloneable"):k->ifaces[i];((dvm_ref*)({const dvm_ref gc_payload_ref_1=(out->l);if(!gc_operation_admit(v,gc_payload_ref_1))return 0;dvm_array_data(v,gc_payload_ref_1);}))[i]=dvm_class_object(v,iface);}return true;}
  struct dvm_class *parent=k->is_array?dvm_find_class_or_external(v,"java/lang/Object"):k->super;out->l=parent?dvm_class_object(v,parent):0;return true;
 }
 if(*p=='<'){int nesting=0;do{if(*p=='<')nesting++;else if(*p=='>')nesting--;p++;}while(*p&&nesting);if(nesting){dvm__throw(v,"java/lang/reflect/GenericSignatureFormatError","formal type parameters");return true;}}
 dvm_ref parent=generic_parse(v,&p,0,k);if(!parent||dvm_exception(v))return true;
 if(!interfaces){out->l=parent;return true;}
 dvm_ref types[64];uint32_t count=0;while(*p){if(count==64){dvm__throw(v,"java/lang/UnsupportedOperationException","generic interface limit");return true;}types[count]=generic_parse(v,&p,0,k);if(!types[count++]||dvm_exception(v))return true;}
 out->l=dvm_new_array(v,'L',"Ljava/lang/reflect/Type;",count);if(out->l&&count)memcpy(({const dvm_ref gc_payload_ref_2=(out->l);if(!gc_operation_admit(v,gc_payload_ref_2))return 0;dvm_array_data(v,gc_payload_ref_2);}),types,count*sizeof *types);return true;
}
typedef struct suppressed_entry {dvm_ref owner; dvm_gc_handle owner_key; dvm_ref *items; size_t count,capacity; struct suppressed_entry *next;} suppressed_entry;
static suppressed_entry *suppressed_head;
/* kind: 1 SharedPreferences.Editor.apply, 2 Throwable.addSuppressed target,
 * 3 Constructor, 4 Field, 5 a Method declared by a class whose metadata the
 * interpreter loaded from dex.  A Field keeps the declaring class and a
 * pointer into that class's field table; a Method keeps the dvm_method
 * pointer.  Both tables are allocated once when the class is loaded and live
 * until dvm_destroy(), so the pointers cannot dangle while this VM runs.
 * reflection_runtime_call() rejects every VM except the one this module
 * started for, so neither object can be used across VMs either.
 * `accessible` records Method.setAccessible: this VM has no SecurityManager,
 * so the flag only decides whether Method.invoke() honours Java's
 * non-public-method access check. */
typedef struct { struct dvm_class *owner; int kind; struct dvm_method *ctor; struct dvm_method *method; struct dvm_field *field; bool statics; bool accessible; char *sig; } reflected;
static void reflected_free(void *p) { reflected *r=p; if(r){free(r->sig);free(r);} }
/* Like generated java.lang.reflect.Proxy classes, keep one Method constant
 * per proxy class/interface method. Ordinary getMethod/ToReflectedMethod keep
 * returning independent accessible objects. Argument arrays are NOT cached:
 * an InvocationHandler may retain them or reenter the proxy. */
typedef struct proxy_method_cache {struct dvm_class *proxy;struct dvm_method *method;dvm_ref object;dvm_gc_handle root;struct proxy_method_cache *next;} proxy_method_cache;
static proxy_method_cache *proxy_methods;
static uint64_t proxy_method_hits,proxy_method_misses;
static bool reflection_cache_gc_roots(struct dvm *v,dvm_gc_visit_fn visit,void *user,void *context){
 (void)context;if(!dvm_gil_held(v)||active!=v)return false;
 for(proxy_method_cache *p=proxy_methods;p;p=p->next)if(p->object)visit(&p->object,user);return true;
}

void reflection_runtime_vm_end(struct dvm *v) {
 if(v!=active)return;
 dvm_gc_root_provider_remove(v,"reflection/proxy-methods",&active);
 while(proxy_methods){proxy_method_cache *p=proxy_methods;proxy_methods=p->next;dvm_unpin(v,p->object);if(p->root)dvm_gc_handle_drop(v,p->root);free(p);}
 while(suppressed_head){suppressed_entry *next=suppressed_head->next;
  dvm_unpin(v,suppressed_head->owner);
  if(suppressed_head->owner_key)dvm_gc_handle_drop(v,suppressed_head->owner_key);
  for(size_t i=0;i<suppressed_head->count;i++)dvm_unpin(v,suppressed_head->items[i]);
  free(suppressed_head->items);free(suppressed_head);suppressed_head=next;}
 package_vm=NULL;package_class=NULL;
 active=NULL;
}
void reflection_runtime_vm_start(struct dvm *v) {
 if(!install_unsafe_class(v))return;
 active=v;if(!dvm_gc_root_provider_add(v,"reflection/proxy-methods",DVM_GC_ROOT_STRONG,reflection_cache_gc_roots,&active)){dvm__throw(v,"java/lang/OutOfMemoryError","reflection GC roots");return;}real_getmethod_log=0;struct dvm_class *base=dvm_find_class_or_external(v,"java/lang/ReflectiveOperationException");
 proxy_method_hits=proxy_method_misses=0;
 /* ReflectionHelper enumerates Object methods before JNI fallback. These
  * declarations describe existing object_runtime/java_basics bodies; do not
  * change getClass's return type to satisfy a guessed Object signature. */
 struct dvm_class *object=dvm_find_class_or_external(v,"java/lang/Object");
 if(object&&!object->nmethods){
  static const struct {const char *name,*sig;uint32_t access;} members[]={
   {"<init>","()V",0x1},{"getClass","()Ljava/lang/Class;",0x11},
   {"toString","()Ljava/lang/String;",0x1},{"hashCode","()I",0x1},
   {"equals","(Ljava/lang/Object;)Z",0x1},
   {"notify","()V",0x11},{"notifyAll","()V",0x11},
   {"wait","()V",0x11},{"wait","(J)V",0x11},{"wait","(JI)V",0x11},
   {"clone","()Ljava/lang/Object;",0x4}
  };
  int count=(int)(sizeof members/sizeof members[0]);
  struct dvm_method *methods=calloc((size_t)count,sizeof *methods);bool ok=methods!=NULL;
  for(int i=0;methods&&i<count;++i){
   struct dvm_method *m=&methods[i];m->cls=object;m->name=members[i].name;
   m->sig=strdup(members[i].sig);m->access=members[i].access;
   if(!m->sig){ok=false;break;}
   m->arg_slots=dvm_sig_arg_slots(m->sig);m->arg_count=dvm_sig_arg_count(m->sig);
   m->ret_kind=dvm_sig_return_kind(m->sig);
  }
  if(ok){object->methods=methods;object->nmethods=count;free(object->mcache);object->mcache=NULL;object->mcache_cap=object->mcache_len=0;}
  else{if(methods){for(int i=0;i<count;++i)free(methods[i].sig);free(methods);}dvm__throw(v,"java/lang/OutOfMemoryError","Object method declarations");return;}
 }
 struct dvm_class *exception=dvm_find_class_or_external(v,"java/lang/Exception");if(base&&base->external)base->super=exception;
 struct dvm_class *missing=dvm_find_class_or_external(v,"java/lang/NoSuchMethodException");if(missing&&missing->external)missing->super=base;
 /* java.lang.reflect.Method 的方法体由 reflection_runtime_call 分派；
  * 72qa 实测 native 侧 FindClass(Method) 通过后 GetMethodID 仍需类方法表
  * （ToReflectedMethod 返回的 Method 对象被 native 侧继续调
  * getName/getParameterTypes 等）。只声明已实现的公开方法。 */
 {
  static const struct{const char *name;const char *sig;int access;} m_methods[]={
   {"getName","()Ljava/lang/String;",0x1},
   {"getDeclaringClass","()Ljava/lang/Class;",0x1},
   {"getReturnType","()Ljava/lang/Class;",0x1},
   {"getParameterTypes","()[Ljava/lang/Class;",0x1},
   {"getModifiers","()I",0x1},
   {"setAccessible","(Z)V",0x1},
   {"invoke","(Ljava/lang/Object;[Ljava/lang/Object;)Ljava/lang/Object;",0x1},
   {"isAnnotationPresent","(Ljava/lang/Class;)Z",0x1},
  };
  enum {M_N=(int)(sizeof m_methods/sizeof m_methods[0])};
  struct dvm_class *mc=dvm_find_class_or_external(v,"java/lang/reflect/Method");
  if(mc&&mc->nmethods==0){
   struct dvm_method *ms=calloc(M_N,sizeof *ms);
   if(ms){
    int ok=1;
    for(int i=0;i<M_N;i++){
     struct dvm_method *m=&ms[i];
     m->cls=mc;m->name=m_methods[i].name;m->sig=strdup(m_methods[i].sig);
     m->access=(uint32_t)m_methods[i].access;
     m->arg_slots=dvm_sig_arg_slots(m->sig);
     m->arg_count=dvm_sig_arg_count(m->sig);
     m->ret_kind=dvm_sig_return_kind(m->sig);
     if(!m->sig)ok=0;
    }
    if(ok){mc->methods=ms;mc->nmethods=M_N;}
    else{for(int i=0;i<M_N;i++)free(ms[i].sig);free(ms);}
   }
  }
 }
 /* Constructor.newInstance wraps a constructor's throwable in this type.
  * Its Throwable ancestry is what lets throwable_reference_call() attach the
  * original cause (and what makes catch (ReflectiveOperationException)
  * behave as Java specifies). */
 struct dvm_class *invocation=dvm_find_class_or_external(v,"java/lang/reflect/InvocationTargetException");if(invocation&&invocation->external)invocation->super=base;
 /* AOSP: java.lang.Class implements java.lang.reflect.Type (plus
  * GenericDeclaration, AnnotatedElement and Serializable).  dvm_runtime_define
  * is a stub in this build, so the synthesized Class class carries no interface
  * edges and a guest `(Type) clazz` raised ClassCastException.  The interface
  * classes must be marked as interfaces so check-cast walks ifaces. */
 struct dvm_class *class_cls=dvm_find_class_or_external(v,"java/lang/Class");
 /* CLICK-UP: publish declarations for Class bodies already implemented by
  * discovery_runtime/reflection_runtime. Keep unknown external classes closed. */
 if(class_cls&&class_cls->external&&!class_cls->nmethods){
  static const struct {const char *name,*sig;uint32_t access;} members[]={
   {"getName","()Ljava/lang/String;",0x1},
   {"getSuperclass","()Ljava/lang/Class;",0x1},
   {"getClassLoader","()Ljava/lang/ClassLoader;",0x1},
   {"getDeclaredMethods","()[Ljava/lang/reflect/Method;",0x1},
   {"getDeclaredFields","()[Ljava/lang/reflect/Field;",0x1},
   {"getDeclaredConstructors","()[Ljava/lang/reflect/Constructor;",0x1},
   {"getConstructors","()[Ljava/lang/reflect/Constructor;",0x1},
   {"getMethod","(Ljava/lang/String;[Ljava/lang/Class;)Ljava/lang/reflect/Method;",0x1},
   {"getDeclaredMethod","(Ljava/lang/String;[Ljava/lang/Class;)Ljava/lang/reflect/Method;",0x1},
   {"isInterface","()Z",0x1},{"isPrimitive","()Z",0x1},
   {"getModifiers","()I",0x1},{"getComponentType","()Ljava/lang/Class;",0x1},
   {"isInstance","(Ljava/lang/Object;)Z",0x1},
   {"forName","(Ljava/lang/String;)Ljava/lang/Class;",0x9},
   {"forName","(Ljava/lang/String;ZLjava/lang/ClassLoader;)Ljava/lang/Class;",0x9}
  };
  int count=sizeof members/sizeof members[0];
  struct dvm_method *ms=calloc(count,sizeof *ms);if(!ms){dvm__throw(v,"java/lang/OutOfMemoryError","Class declarations");return;}
  for(int i=0;i<count;i++){ms[i].cls=class_cls;ms[i].name=members[i].name;ms[i].sig=strdup(members[i].sig);ms[i].access=members[i].access;
   if(!ms[i].sig){for(int j=0;j<i;j++)free(ms[j].sig);free(ms);dvm__throw(v,"java/lang/OutOfMemoryError","Class declaration signature");return;}
   ms[i].arg_slots=dvm_sig_arg_slots(ms[i].sig);ms[i].arg_count=dvm_sig_arg_count(ms[i].sig);ms[i].ret_kind=dvm_sig_return_kind(ms[i].sig);
  }
  class_cls->methods=ms;class_cls->nmethods=count;free(class_cls->mcache);class_cls->mcache=NULL;class_cls->mcache_cap=class_cls->mcache_len=0;
 }
 /* JNI ReflectionHelper enumerates System before looking up identityHashCode.
  * These are declarations of real existing dispatch bodies, not fake methods. */
 struct dvm_class *system=dvm_find_class_or_external(v,"java/lang/System");
 if(system&&system->external&&!system->nmethods){
  static const struct {const char *name,*sig;} members[]={
   {"identityHashCode","(Ljava/lang/Object;)I"},
   {"arraycopy","(Ljava/lang/Object;ILjava/lang/Object;II)V"},
   {"currentTimeMillis","()J"},{"nanoTime","()J"},
   {"getProperties","()Ljava/util/Properties;"},
   {"getProperty","(Ljava/lang/String;)Ljava/lang/String;"},
   {"getProperty","(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;"},
   {"load","(Ljava/lang/String;)V"},{"loadLibrary","(Ljava/lang/String;)V"}
  };
  int count=sizeof members/sizeof members[0];
  struct dvm_method *ms=calloc(count,sizeof *ms);
  if(!ms){dvm__throw(v,"java/lang/OutOfMemoryError","System declarations");return;}
  for(int i=0;i<count;i++){
   ms[i].cls=system;ms[i].name=members[i].name;ms[i].sig=strdup(members[i].sig);ms[i].access=0x9;
   if(!ms[i].sig){for(int j=0;j<i;j++)free(ms[j].sig);free(ms);dvm__throw(v,"java/lang/OutOfMemoryError","System declaration signature");return;}
   ms[i].arg_slots=dvm_sig_arg_slots(ms[i].sig);ms[i].arg_count=dvm_sig_arg_count(ms[i].sig);ms[i].ret_kind=dvm_sig_return_kind(ms[i].sig);
  }
  system->methods=ms;system->nmethods=count;free(system->mcache);system->mcache=NULL;system->mcache_cap=system->mcache_len=0;
 }
 /* A platform abstract declaration. Concrete DEX receivers implement it;
  * no fabricated body and no empty-array exemption for external classes. */
 struct dvm_class *receiver=dvm_find_class_or_external(v,"android/content/BroadcastReceiver");
 if(receiver&&receiver->external&&!receiver->nmethods){
  struct dvm_method *ms=calloc(1,sizeof *ms);
  if(!ms){dvm__throw(v,"java/lang/OutOfMemoryError","BroadcastReceiver declaration");return;}
  ms->cls=receiver;ms->name="onReceive";ms->sig=strdup("(Landroid/content/Context;Landroid/content/Intent;)V");ms->access=0x401;
  if(!ms->sig){free(ms);dvm__throw(v,"java/lang/OutOfMemoryError","BroadcastReceiver signature");return;}
  ms->arg_count=2;ms->arg_slots=2;ms->ret_kind='V';receiver->methods=ms;receiver->nmethods=1;
  free(receiver->mcache);receiver->mcache=NULL;receiver->mcache_cap=receiver->mcache_len=0;
 }
 struct dvm_class *transfer=dvm_find_class_or_external(v,"java/util/concurrent/LinkedTransferQueue");
 if(transfer&&transfer->external&&!transfer->nmethods){
  static const struct {const char *name,*sig;} entries[]={
   {"<init>","()V"},{"add","(Ljava/lang/Object;)Z"},{"take","()Ljava/lang/Object;"},{"size","()I"},{"isEmpty","()Z"}
  };
  struct dvm_method *ms=calloc(5,sizeof *ms);if(!ms){dvm__throw(v,"java/lang/OutOfMemoryError","transfer queue declarations");return;}
  for(int i=0;i<5;i++){
   ms[i].cls=transfer;ms[i].name=entries[i].name;ms[i].sig=strdup(entries[i].sig);ms[i].access=0x1;
   if(!ms[i].sig){for(int j=0;j<i;j++)free(ms[j].sig);free(ms);dvm__throw(v,"java/lang/OutOfMemoryError","transfer queue signature");return;}
   ms[i].arg_count=dvm_sig_arg_count(ms[i].sig);ms[i].arg_slots=dvm_sig_arg_slots(ms[i].sig);ms[i].ret_kind=dvm_sig_return_kind(ms[i].sig);
  }
  transfer->methods=ms;transfer->nmethods=5;free(transfer->mcache);transfer->mcache=NULL;transfer->mcache_cap=transfer->mcache_len=0;
 }
 struct dvm_class *random=dvm_find_class_or_external(v,"java/util/Random");
 if(random&&random->external&&!random->nmethods){
  static const struct {const char *name,*sig;} entries[]={{"<init>","()V"},{"<init>","(J)V"},{"setSeed","(J)V"},{"nextInt","()I"},{"nextInt","(I)I"}};
  struct dvm_method *ms=calloc(5,sizeof *ms);if(!ms){dvm__throw(v,"java/lang/OutOfMemoryError","Random declarations");return;}
  for(int i=0;i<5;i++){ms[i].cls=random;ms[i].name=entries[i].name;ms[i].sig=strdup(entries[i].sig);ms[i].access=1;if(!ms[i].sig){for(int j=0;j<i;j++)free(ms[j].sig);free(ms);dvm__throw(v,"java/lang/OutOfMemoryError","Random signature");return;}ms[i].arg_count=dvm_sig_arg_count(ms[i].sig);ms[i].arg_slots=dvm_sig_arg_slots(ms[i].sig);ms[i].ret_kind=dvm_sig_return_kind(ms[i].sig);}
  random->methods=ms;random->nmethods=5;free(random->mcache);random->mcache=NULL;random->mcache_cap=random->mcache_len=0;
 }
 struct dvm_class *type_iface=dvm_find_class_or_external(v,"java/lang/reflect/Type");
 struct dvm_class *generic_decl=dvm_find_class_or_external(v,"java/lang/reflect/GenericDeclaration");
 struct dvm_class *annotated=dvm_find_class_or_external(v,"java/lang/reflect/AnnotatedElement");
 if(type_iface)type_iface->is_interface=true;
 if(generic_decl)generic_decl->is_interface=true;
 if(annotated)annotated->is_interface=true;
 if(class_cls&&!class_cls->ifaces&&!class_cls->nifaces&&type_iface&&generic_decl&&annotated){
  struct dvm_class **ifaces=calloc(4,sizeof *ifaces);
  if(ifaces){
   ifaces[0]=type_iface;ifaces[1]=generic_decl;ifaces[2]=annotated;
   ifaces[3]=dvm_find_class_or_external(v,"java/io/Serializable");
   class_cls->ifaces=ifaces;class_cls->nifaces=ifaces[3]?4:3;
   sdk_trace_fprintf(stderr,"RUNTIME_CONTRACT Class->Type installed nifaces=%d\n",class_cls->nifaces);
  }
 }
 install_package_class(v);
}
static bool fail(struct dvm *v,const char *type,const char *message) { dvm__throw(v,type,"%s",message); return true; }

/* java.lang.Package: the only member DEX uses is getName().  The payload is
 * the dotted package name observed from the Class descriptor, never a
 * synthesized placeholder: Class.getPackage() returns null for primitives,
 * primitive arrays and the default package, exactly like Android. */
struct package_box { char *name; };
static void package_box_destroy(void *p) { struct package_box *b=p; if(b){free(b->name);free(b);} }
static struct package_box *package_box_of(struct dvm *v,dvm_ref ref) {
 const dvm_ref gc_ref_o_4=(ref); if(!gc_operation_admit(v,gc_ref_o_4))return false;
 if(!dvm__obj(v,gc_ref_o_4)||!dvm__obj(v,gc_ref_o_4)->cls||strcmp(dvm__obj(v,gc_ref_o_4)->cls->name,"java/lang/Package")||dvm__obj(v,gc_ref_o_4)->data_destroy!=package_box_destroy||!dvm__obj(v,gc_ref_o_4)->data)return NULL;
 return dvm__obj(v,gc_ref_o_4)->data;
}
static dvm_ref package_create(struct dvm *v,const char *name) {
 if(!package_class)install_package_class(v);
 struct dvm_class *c=package_class?package_class:dvm_find_class_or_external(v,"java/lang/Package");
 if(!c)return 0;
 dvm_ref ref=dvm_new_object(v,c);
 if(!ref)return 0;
 const dvm_ref gc_ref_o_5=(ref); if(!gc_operation_admit(v,gc_ref_o_5))return false;
 if(!dvm__obj(v,gc_ref_o_5))return 0;
 struct package_box *b=calloc(1,sizeof *b);
 if(!b)return 0;
 size_t n=strlen(name);
 b->name=malloc(n+1);
 if(!b->name){free(b);return 0;}
 memcpy(b->name,name,n+1);
 dvm__obj(v,gc_ref_o_5)->data=b;dvm__obj(v,gc_ref_o_5)->data_destroy=package_box_destroy;
 return ref;
}
static bool package_get_name_call(struct dvm *v,dvm_ref self,union dvm_value *out) {
 struct package_box *b=package_box_of(v,self);
 if(!b)return fail(v,self?"java/lang/IllegalStateException":"java/lang/NullPointerException","Package.getName receiver");
 dvm_ref s=dvm_new_string(v,b->name);
 if(!s)return fail(v,"java/lang/OutOfMemoryError","Package.getName");
 out->l=s;
 return true;
}
static bool package_builtin(struct dvm *v,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out) {
 (void)args;
 struct dvm_method *m=dvm__builtin_method();
 if(!m||strcmp(m->name,"getName")||strcmp(m->sig,"()Ljava/lang/String;"))return false;
 if(nargs!=0)return fail(v,"java/lang/IllegalArgumentException","Package.getName arguments");
 return package_get_name_call(v,self,out);
}
/* Derive the dotted package name of a non-array class.  The class name kept
 * by the runtime is slash-separated (java/lang/Class); class files without a
 * package component belong to the default package and yield no Package. */
static bool class_package_name(struct dvm *v,struct dvm_class *c,char *buf,size_t cap) {
 (void)v;
 const char *name=c->name?c->name:"";
 const char *slash=strrchr(name,'/');
 const char *dot=strrchr(name,'.');
 const char *sep=(slash&&(!dot||slash>dot))?slash:(dot?dot:NULL);
 if(!sep||sep==name)return false;
 size_t n=(size_t)(sep-name);
 if(n+1>cap)return false;
 memcpy(buf,name,n);buf[n]=0;
 /* dvm_class.name is already slash-separated, but a dot-separated name must
  * still become a legal binary package name; normalize both forms. */
 for(size_t i=0;i<n;i++)if(buf[i]=='/')buf[i]='.';
 return true;
}
static void install_package_class(struct dvm *v) {
 struct dvm_class *c=dvm__register_builtin(v,"Ljava/lang/Package;");
 if(!c)return;
 c->super=dvm_find_class_or_external(v,"java/lang/Object");
 c->methods=calloc(1,sizeof *c->methods);
 if(!c->methods){c->methods=NULL;return;}
 c->nmethods=1;
 install_fixture_method(c,0,"getName","()Ljava/lang/String;",0x0001,package_builtin,'L');
 package_vm=v;
 package_class=c;
}
static bool throwable(struct dvm *v,dvm_ref ref){struct dvm_class *base=dvm_find_class_or_external(v,"java/lang/Throwable");const dvm_ref gc_ref_o_6=(ref); if(!gc_operation_admit(v,gc_ref_o_6))return false;return dvm__obj(v,gc_ref_o_6)&&base&&dvm__class_assignable(v,dvm__obj(v,gc_ref_o_6)->cls,base);}
static suppressed_entry *suppressed_for(dvm_ref owner){for(suppressed_entry *p=suppressed_head;p;p=p->next)if(p->owner==owner&&dvm_gc_handle_ref(active,p->owner_key)==owner)return p;return NULL;}
/* Suppression is a Throwable owner edge, never a global strong root. */
bool reflection_runtime_gc_visit_owner(struct dvm *v,dvm_ref owner,dvm_gc_visit_fn visit,void *user){
 if(v!=active||!visit||!dvm_gil_held(v))return false;
 suppressed_entry *p=suppressed_for(owner);if(!p)return true;
 if(p->count>p->capacity||(!p->items&&p->count))return false;
 for(size_t i=0;i<p->count;++i)if(p->items[i]){if(!dvm__obj(v,p->items[i]))return false;visit(&p->items[i],user);}return true;
}
static bool add_suppressed(struct dvm *v,dvm_ref owner,dvm_ref value){
 if(!throwable(v,owner))return fail(v,"java/lang/IllegalArgumentException","addSuppressed target");
 if(!value)return fail(v,"java/lang/NullPointerException","suppressed exception");
 if(value==owner)return fail(v,"java/lang/IllegalArgumentException","self suppression");
 if(!throwable(v,value))return fail(v,"java/lang/IllegalArgumentException","suppressed exception type");
 suppressed_entry *p=suppressed_for(owner);
 if(!p){p=calloc(1,sizeof *p);if(!p)return fail(v,"java/lang/OutOfMemoryError","suppressed list");p->owner_key=dvm_gc_handle_new(v,owner,DVM_GC_HANDLE_WEAK);if(!p->owner_key){free(p);return true;}p->owner=owner;p->next=suppressed_head;suppressed_head=p;dvm_pin(v,owner);}
 if(p->count==p->capacity){size_t cap=p->capacity?p->capacity*2:4;dvm_ref *items=realloc(p->items,cap*sizeof *items);if(!items)return fail(v,"java/lang/OutOfMemoryError","suppressed list");p->items=items;p->capacity=cap;}
 p->items[p->count++]=value;dvm_pin(v,value);return true;
}
static bool get_suppressed(struct dvm *v,dvm_ref owner,union dvm_value *out){
 if(!throwable(v,owner))return fail(v,"java/lang/IllegalArgumentException","getSuppressed target");
 suppressed_entry *p=suppressed_for(owner);size_t count=p?p->count:0;
 out->l=dvm_new_array(v,'L',"Ljava/lang/Throwable;",(uint32_t)count);
 if(!out->l)return fail(v,"java/lang/OutOfMemoryError","suppressed array");
 if(count)memcpy(({const dvm_ref gc_payload_ref_3=(out->l);if(!gc_operation_admit(v,gc_payload_ref_3))return 0;dvm_array_data(v,gc_payload_ref_3);}),p->items,count*sizeof(dvm_ref));return true;
}
static bool is_class_array(struct dvm *v,dvm_ref r) {
 if(!r)return true;
 const dvm_ref gc_ref_o_7=(r); if(!gc_operation_admit(v,gc_ref_o_7))return false;
 return dvm__obj(v,gc_ref_o_7)&&dvm__obj(v,gc_ref_o_7)->kind==DVM_OBJ_ARRAY&&dvm__obj(v,gc_ref_o_7)->cls&&dvm__obj(v,gc_ref_o_7)->cls->desc&&!strcmp(dvm__obj(v,gc_ref_o_7)->cls->desc,"[Ljava/lang/Class;");
}
static bool is_object_array(struct dvm *v,dvm_ref r) {
 if(!r)return true;
 const dvm_ref gc_ref_o_8=(r); if(!gc_operation_admit(v,gc_ref_o_8))return false;
 return dvm__obj(v,gc_ref_o_8)&&dvm__obj(v,gc_ref_o_8)->kind==DVM_OBJ_ARRAY&&dvm__obj(v,gc_ref_o_8)->cls&&dvm__obj(v,gc_ref_o_8)->cls->desc&&!strcmp(dvm__obj(v,gc_ref_o_8)->cls->desc,"[Ljava/lang/Object;");
}
/* Class.getMethod()/getDeclaredMethod() look up one method by name and exact
 * parameter descriptors.  The methods come from the same dvm_method tables
 * getDeclaredMethods() reports, so a returned Method keeps real metadata and
 * Method.invoke() can call the same dvm_method.  `params` is the descriptor
 * prefix built from the Class[] (for example "(I[Ljava/lang/String;)"), so a
 * method matches only when both the name and every parameter descriptor are
 * equal.  match_declared_method() scans one class's own table;
 * lookup_public_method() adds the superclass chain and then the interface
 * graph, which is the search order Java's Class.getMethod specifies.
 * A missing member table or a member without a name/signature means the class
 * metadata itself is malformed, so it is reported instead of being silently
 * folded into "no such method". */
static struct dvm_method *match_declared_method(struct dvm_class *c,const char *name,const char *params,size_t plen,bool public_only,bool *malformed) {
 if(!c)return NULL;
 if(c->nmethods<0||(c->nmethods>0&&!c->methods)){*malformed=true;return NULL;}
 for(int i=0;i<c->nmethods;i++) {
  struct dvm_method *m=&c->methods[i];
  if(!m->name||!m->sig){*malformed=true;return NULL;}
  if(m->name[0]=='<')continue;
  if(public_only&&!(m->access&DEX_ACC_PUBLIC))continue;
  if(strcmp(m->name,name))continue;
  if(strncmp(m->sig,params,plen))continue;
  return m;
 }
 return NULL;
}
static struct dvm_method *match_interface_method(struct dvm_class *c,const char *name,const char *params,size_t plen,bool *malformed,int depth) {
 if(!c){*malformed=true;return NULL;}
 if(depth>32){*malformed=true;return NULL;}
 if(c->external&&!c->nmethods)return NULL;
 if(c->nifaces<0||(c->nifaces>0&&!c->ifaces)){*malformed=true;return NULL;}
 struct dvm_method *m=match_declared_method(c,name,params,plen,true,malformed);
 if(*malformed||m)return m;
 for(int i=0;i<c->nifaces;i++) {
  if(!c->ifaces[i]){*malformed=true;return NULL;}
  m=match_interface_method(c->ifaces[i],name,params,plen,malformed,depth+1);
  if(*malformed||m)return m;
 }
 return NULL;
}
static struct dvm_method *lookup_public_method(struct dvm_class *owner,const char *name,const char *params,size_t plen,bool *malformed) {
 int depth=0;
 for(struct dvm_class *c=owner;c;c=c->super) {
  if(++depth>64){*malformed=true;return NULL;}
  if(c->external&&!c->nmethods)continue;
  if(c->nifaces<0||(c->nifaces>0&&!c->ifaces)){*malformed=true;return NULL;}
  struct dvm_method *m=match_declared_method(c,name,params,plen,true,malformed);
  if(*malformed||m)return m;
 }
 for(struct dvm_class *c=owner;c;c=c->super) {
  if(c->external&&!c->nmethods)continue;
  for(int i=0;i<c->nifaces;i++) {
   if(!c->ifaces[i]){*malformed=true;return NULL;}
   struct dvm_method *m=match_interface_method(c->ifaces[i],name,params,plen,malformed,0);
   if(*malformed||m)return m;
  }
 }
 return NULL;
}
/* Build the exact parameter descriptor prefix "(...)" from a Class[].
 * A null element is the NullPointerException Java specifies; a non-Class
 * element cannot come from verified bytecode but can still reach the native
 * hook through an earlier reflective call, so it is rejected explicitly. */
static bool method_parameter_prefix(struct dvm *v,dvm_ref types,char **out,size_t *length) {
 uint32_t count=dvm_array_length(v,types);
 dvm_ref *items=types?(dvm_ref*)({const dvm_ref gc_payload_ref_4=(types);if(!gc_operation_admit(v,gc_payload_ref_4))return 0;dvm_array_data(v,gc_payload_ref_4);}):NULL;
 if(count&&!items){dvm__throw(v,"java/lang/InternalError","method parameter array");return false;}
 size_t cap=2;
 for(uint32_t i=0;i<count;i++) {
  if(!items[i]){dvm__throw(v,"java/lang/NullPointerException","parameter type");return false;}
  const dvm_ref gc_ref_o_9=(items[i]); if(!gc_operation_admit(v,gc_ref_o_9))return false;
  if(!dvm__obj(v,gc_ref_o_9)||dvm__obj(v,gc_ref_o_9)->kind!=DVM_OBJ_CLASS||!dvm__obj(v,gc_ref_o_9)->klass||!dvm__obj(v,gc_ref_o_9)->klass->desc||!dvm__obj(v,gc_ref_o_9)->klass->desc[0]){dvm__throw(v,"java/lang/IllegalArgumentException","getMethod parameter type");return false;}
  cap+=strlen(dvm__obj(v,gc_ref_o_9)->klass->desc);
 }
 char *prefix=malloc(cap+1);
 if(!prefix){dvm__throw(v,"java/lang/OutOfMemoryError","method parameter signature");return false;}
 size_t at=0;prefix[at++]='(';
 for(uint32_t i=0;i<count;i++) {
  const dvm_ref gc_ref_o_10=(items[i]); if(!gc_operation_admit(v,gc_ref_o_10))return false;
  size_t n=strlen(dvm__obj(v,gc_ref_o_10)->klass->desc);
  memcpy(prefix+at,dvm__obj(v,gc_ref_o_10)->klass->desc,n);at+=n;
 }
 prefix[at++]=')';prefix[at]=0;
 *out=prefix;*length=at;return true;
}
/* A Method object is never a bare stub: kind 5 holds the dvm_method pointer,
 * kinds 1/2 are the two built-in special methods, and every other entry point
 * (getName/getReturnType/getParameterTypes/setAccessible/invoke) reads that
 * same metadata. */
static bool new_method_object(struct dvm *v,struct dvm_class *owner,int kind,struct dvm_method *method,union dvm_value *out) {
 struct dvm_class *method_class=dvm_find_class_or_external(v,"java/lang/reflect/Method");
 if(!method_class)return fail(v,"java/lang/InternalError","Method class");
 reflected *r=calloc(1,sizeof *r);
 if(!r)return fail(v,"java/lang/OutOfMemoryError","Method");
 r->owner=owner;r->kind=kind;r->method=method;
 out->l=dvm_new_object(v,method_class);
 const dvm_ref gc_ref_mo_11=(out->l); if(!gc_operation_admit(v,gc_ref_mo_11))return false;
 if(!dvm__obj(v,gc_ref_mo_11)){out->l=0;free(r);return fail(v,"java/lang/OutOfMemoryError","Method");}
 dvm__obj(v,gc_ref_mo_11)->data=r;dvm__obj(v,gc_ref_mo_11)->data_destroy=reflected_free;return true;
}
/* JNI ToReflectedMethod（slot 9）把 jmethodID 还原成 java.lang.reflect.Method
 * 对象；new_method_object 的 kind 5 就是该句柄语义。owner 为 NULL 时用
 * method->cls。72pz 实测引擎在 worker 分派路径调用该槽失败留 pending
 * UnsatisfiedLinkError，打断 nativeRender 的帧回调链。 */
static bool reflection_runtime_method_object_operation(struct dvm *v,struct dvm_class *owner,struct dvm_method *method,dvm_ref *out) {
 if(!v||!method||!out)return false;
 if(!strcmp(method->name,"<init>")){
  struct dvm_class *klass=dvm_find_class_or_external(v,"java/lang/reflect/Constructor");
  reflected *r=calloc(1,sizeof *r);
  if(!klass||!r)return fail(v,"java/lang/OutOfMemoryError","reflected Constructor");
  r->owner=method->cls;r->kind=3;r->ctor=method;r->sig=strdup(method->sig);
  if(!r->sig){free(r);return fail(v,"java/lang/OutOfMemoryError","Constructor signature");}
  *out=dvm_new_object(v,klass);const dvm_ref gc_ref_o_12=(*out); if(!gc_operation_admit(v,gc_ref_o_12))return false;
  if(!dvm__obj(v,gc_ref_o_12)){reflected_free(r);return fail(v,"java/lang/OutOfMemoryError","Constructor object");}
  dvm__obj(v,gc_ref_o_12)->data=r;dvm__obj(v,gc_ref_o_12)->data_destroy=reflected_free;return true;
 }
 union dvm_value value={0};
 if(!new_method_object(v,owner?owner:method->cls,5,method,&value))return false;
 *out=value.l;return true;
}
bool reflection_runtime_method_object(struct dvm *v,struct dvm_class *owner,struct dvm_method *method,dvm_ref *out){
 struct gc_operation operation __attribute__((cleanup(gc_operation_end)));
 if(!gc_operation_begin(v,&operation))return false;
 bool result=reflection_runtime_method_object_operation(v,owner,method,out);
 gc_operation_result(&operation,"()Ljava/lang/reflect/Method;",result&&!dvm_exception(v)&&out?*out:0);
 return result;
}

static char *parameter_signature(struct dvm *v,dvm_ref types){
 uint32_t count=types?dvm_array_length(v,types):0;dvm_ref *items=types?(dvm_ref*)({const dvm_ref gc_payload_ref_5=(types);if(!gc_operation_admit(v,gc_payload_ref_5))return 0;dvm_array_data(v,gc_payload_ref_5);}):NULL;
 size_t cap=4;
 for(uint32_t i=0;i<count;i++){const dvm_ref gc_ref_o_13=((items[i])?(items[i]):0); if(!gc_operation_admit(v,gc_ref_o_13))return false;if(!dvm__obj(v,gc_ref_o_13)||dvm__obj(v,gc_ref_o_13)->kind!=DVM_OBJ_CLASS||!dvm__obj(v,gc_ref_o_13)->klass||!dvm__obj(v,gc_ref_o_13)->klass->desc){dvm__throw(v,"java/lang/NullPointerException","parameter type");return NULL;}cap+=strlen(dvm__obj(v,gc_ref_o_13)->klass->desc);}
 char *sig=malloc(cap+1);if(!sig){dvm__throw(v,"java/lang/OutOfMemoryError","signature");return NULL;}
 size_t at=0;sig[at++]='(';
 for(uint32_t i=0;i<count;i++){const dvm_ref gc_ref_o_14=(items[i]); if(!gc_operation_admit(v,gc_ref_o_14))return false;size_t n=strlen(dvm__obj(v,gc_ref_o_14)->klass->desc);memcpy(sig+at,dvm__obj(v,gc_ref_o_14)->klass->desc,n);at+=n;}
 sig[at++]=')';sig[at++]='V';sig[at]=0;return sig;
}
static bool malformed_signature(struct dvm *v,union dvm_value *args){native_scratch_release(args);dvm__throw(v,"java/lang/InternalError","malformed signature");return false;}
/* One parameter descriptor starting at `p`; returns one past its end, or NULL
 * when the signature is malformed.  Parameters cannot be V. */
static const char *field_desc_end(const char *p) {
 if(!p)return NULL;
 while(*p=='[')++p;
 if(*p=='L'){const char *end=strchr(p,';');return end?end+1:NULL;}
 if(*p&&strchr("ZBCSIJFD",*p))return p+1;
 return NULL;
}
static struct dvm_class *parameter_class(struct dvm *v,const char *desc,size_t length) {
 char *text=malloc(length+1);
 if(!text){dvm__throw(v,"java/lang/OutOfMemoryError","parameter type");return NULL;}
 memcpy(text,desc,length);text[length]=0;
 struct dvm_class *cls=dvm__class_by_desc(v,text);free(text);return cls;
}
/* Reflection unboxing follows the Java widening primitive conversion matrix
 * that Constructor.newInstance applies: Byte may feed short/int/long/float/
 * double; Short may feed int/long/float/double; Character may feed int/long/
 * float/double; Integer may feed long/float/double; Long may feed float/
 * double; Float may feed double; Boolean only feeds boolean.  Narrowing
 * (Long->int, Double->float, ...) stays rejected because reflection must not
 * silently truncate the argument.  Every wrapper is read through its own
 * value method from java_basics/small_boxes; no value is invented. */
enum wrapper_kind { WK_NONE, WK_BOOLEAN, WK_BYTE, WK_SHORT, WK_CHARACTER, WK_INTEGER, WK_LONG, WK_FLOAT, WK_DOUBLE };
static enum wrapper_kind wrapper_kind_of(const char *name) {
 if(!name)return WK_NONE;
 if(!strcmp(name,"java/lang/Boolean"))return WK_BOOLEAN;
 if(!strcmp(name,"java/lang/Byte"))return WK_BYTE;
 if(!strcmp(name,"java/lang/Short"))return WK_SHORT;
 if(!strcmp(name,"java/lang/Character"))return WK_CHARACTER;
 if(!strcmp(name,"java/lang/Integer"))return WK_INTEGER;
 if(!strcmp(name,"java/lang/Long"))return WK_LONG;
 if(!strcmp(name,"java/lang/Float"))return WK_FLOAT;
 if(!strcmp(name,"java/lang/Double"))return WK_DOUBLE;
 return WK_NONE;
}
/* `from` is the wrapper's primitive; `to` is the declared parameter kind. */
static bool primitive_widens_to(enum wrapper_kind from,char to) {
 switch(to){
  case 'Z':return from==WK_BOOLEAN;
  case 'B':return from==WK_BYTE;
  case 'S':return from==WK_BYTE||from==WK_SHORT;
  case 'C':return from==WK_CHARACTER;
  case 'I':return from==WK_BYTE||from==WK_SHORT||from==WK_CHARACTER||from==WK_INTEGER;
  case 'J':return from==WK_BYTE||from==WK_SHORT||from==WK_CHARACTER||from==WK_INTEGER||from==WK_LONG;
  case 'F':return from==WK_BYTE||from==WK_SHORT||from==WK_CHARACTER||from==WK_INTEGER||from==WK_LONG||from==WK_FLOAT;
  case 'D':return from==WK_BYTE||from==WK_SHORT||from==WK_CHARACTER||from==WK_INTEGER||from==WK_LONG||from==WK_FLOAT||from==WK_DOUBLE;
  default:return false;
 }
}
static bool read_wrapper_value(struct dvm *v,enum wrapper_kind kind,dvm_ref value,union dvm_value *out) {
 switch(kind){
  case WK_BOOLEAN:return java_basics_call(v,"java/lang/Boolean","booleanValue","()Z",value,NULL,0,out)&&!dvm_exception(v);
  case WK_BYTE:return small_boxes_call(v,"java/lang/Byte","byteValue","()B",value,NULL,0,out)&&!dvm_exception(v);
  case WK_SHORT:return small_boxes_call(v,"java/lang/Short","shortValue","()S",value,NULL,0,out)&&!dvm_exception(v);
  case WK_CHARACTER:return java_basics_call(v,"java/lang/Character","charValue","()C",value,NULL,0,out)&&!dvm_exception(v);
  case WK_INTEGER:return java_basics_call(v,"java/lang/Integer","intValue","()I",value,NULL,0,out)&&!dvm_exception(v);
  case WK_LONG:return java_basics_call(v,"java/lang/Long","longValue","()J",value,NULL,0,out)&&!dvm_exception(v);
  case WK_FLOAT:return java_basics_call(v,"java/lang/Float","floatValue","()F",value,NULL,0,out)&&!dvm_exception(v);
  case WK_DOUBLE:return java_basics_call(v,"java/lang/Double","doubleValue","()D",value,NULL,0,out)&&!dvm_exception(v);
  default:return false;
 }
}
static bool unbox_primitive(struct dvm *v,char kind,dvm_ref value,union dvm_value *out) {
 const dvm_ref gc_ref_o_15=(value); if(!gc_operation_admit(v,gc_ref_o_15))return false;
 if(!dvm__obj(v,gc_ref_o_15)||!dvm__obj(v,gc_ref_o_15)->cls||!dvm__obj(v,gc_ref_o_15)->cls->name)return false;
 enum wrapper_kind from=wrapper_kind_of(dvm__obj(v,gc_ref_o_15)->cls->name);
 if(from==WK_NONE||!primitive_widens_to(from,kind))return false;
 union dvm_value boxed={0};
 if(!read_wrapper_value(v,from,value,&boxed))return false;
 switch(kind){
  case 'Z':out->i=boxed.i!=0;return true;
  case 'B':out->i=(int8_t)boxed.i;return true;
  case 'S':out->i=(int16_t)boxed.i;return true;
  case 'C':out->i=(int32_t)(uint16_t)boxed.i;return true;
  case 'I':out->i=boxed.i;return true;
  case 'J':out->j=from==WK_LONG?boxed.j:(int64_t)boxed.i;return true;
  case 'F':
   if(from==WK_FLOAT)out->f=boxed.f;
   else if(from==WK_LONG)out->f=(float)boxed.j;
   else out->f=(float)boxed.i;
   return true;
  case 'D':
   if(from==WK_DOUBLE)out->d=boxed.d;
   else if(from==WK_FLOAT)out->d=(double)boxed.f;
   else if(from==WK_LONG)out->d=(double)boxed.j;
   else out->d=(double)boxed.i;
   return true;
  default:return false;
 }
}
static bool reflection_arguments(struct dvm *v,const char *sig,dvm_ref values,union dvm_value **out,int *count){
 if(!sig)return malformed_signature(v,NULL);
 uint32_t n=values?dvm_array_length(v,values):0;dvm_ref *items=values?(dvm_ref*)({const dvm_ref gc_payload_ref_6=(values);if(!gc_operation_admit(v,gc_payload_ref_6))return 0;dvm_array_data(v,gc_payload_ref_6);}):NULL;
 int wanted=0;
 for(const char *p=sig+1;*p&&*p!=')';++wanted){const char *end=field_desc_end(p);if(!end)return malformed_signature(v,NULL);p=end;}
 if((uint32_t)wanted!=n){dvm__throw(v,"java/lang/IllegalArgumentException","argument count");return false;}
 union dvm_value *args=wanted?native_scratch_acquire((size_t)wanted*sizeof *args):NULL;
 if(wanted&&!args){dvm__throw(v,"java/lang/OutOfMemoryError","reflection arguments");return false;}
 int index=0;const char *p=sig+1;
 while(*p&&*p!=')'){
  const char *end=field_desc_end(p);
  if(!end)return malformed_signature(v,args);
  char kind=*p;dvm_ref value=items[index];
  if(kind=='L'||kind=='['){
   /* A null reference or array is a legal argument for a reference/array
    * parameter.  A non-null value must be assignable to the declared type;
    * dvm__class_assignable() also covers array covariance and rejects int[]
    * for Object[]. */
   if(value){
    struct dvm_class *target=parameter_class(v,p,(size_t)(end-p));
    if(!target){native_scratch_release(args);if(!dvm_exception(v))dvm__throw(v,"java/lang/InternalError","parameter type");return false;}
    const dvm_ref gc_ref_o_16=(value); if(!gc_operation_admit(v,gc_ref_o_16))return false;
    if(!dvm__obj(v,gc_ref_o_16)||!dvm__obj(v,gc_ref_o_16)->cls||!dvm__class_assignable(v,dvm__obj(v,gc_ref_o_16)->cls,target)){native_scratch_release(args);dvm__throw(v,"java/lang/IllegalArgumentException","argument type");return false;}
   }
   args[index++].l=value;
  }else{
   /* Primitive parameters still reject null; a wrapper is unboxed and then
    * widened only when the reflection conversion matrix allows it. */
   if(!value){native_scratch_release(args);dvm__throw(v,"java/lang/IllegalArgumentException","null primitive argument");return false;}
   union dvm_value boxed={0};
   if(!unbox_primitive(v,kind,value,&boxed)){native_scratch_release(args);dvm__throw(v,"java/lang/IllegalArgumentException","primitive argument");return false;}
   args[index++]=boxed;
  }
  p=end;
 }
 *out=args;*count=wanted;return true;
}
/* Constructor.newInstance and Method.invoke wrap a throwable raised by the
 * invoked body in java/lang/reflect/InvocationTargetException, while
 * Class.newInstance propagates it unchanged.  dvm__throw() cannot attach a
 * cause, so construct the wrapper first and let throwable_reference_call()
 * initialize it with the original throwable; that call pins the cause and
 * fills the wrapper's message with the cause description.  If the wrapper
 * type or its (Throwable) constructor is unavailable, the fallback message
 * still carries the original class name and the (possibly
 * dvm__throw-truncated) message, but getCause() will be null: this is a
 * documented narrowing of the JDK contract, not a silent success. */
static void wrap_invocation_exception(struct dvm *v) {
 dvm_ref cause=dvm_exception(v);
 if(!cause)return;
 char text[1024];
 dvm_describe_exception(v,cause,text,sizeof text);
 dvm_clear_exception(v);
 dvm__throw(v,"java/lang/reflect/InvocationTargetException","%s",text);
 dvm_ref wrapper=dvm_exception(v);
 if(wrapper){
  dvm_clear_exception(v);
  union dvm_value arg={.l=cause},ignored={0};
  if(throwable_reference_call(v,"java/lang/reflect/InvocationTargetException","<init>","(Ljava/lang/Throwable;)V",wrapper,&arg,1,&ignored)&&!dvm_exception(v)){
   v->exception=wrapper;v->exc_ref=wrapper;v->nexc_trace=0;return;
  }
  dvm_clear_exception(v);
 }
 dvm__throw(v,"java/lang/reflect/InvocationTargetException","%s",text);
}
/* Constructors stay independent. Reflection result boxing may use immutable
 * wrapper valueOf caches; the constructor helper is defined below. */
static dvm_ref box_value(struct dvm *v,const char *cls,const char *sig,union dvm_value arg);
static bool field_reflected(struct dvm *v,dvm_ref self,reflected **out) {
 const dvm_ref gc_ref_o_17=(self); if(!gc_operation_admit(v,gc_ref_o_17))return false;
 reflected *r=dvm__obj(v,gc_ref_o_17)&&dvm__obj(v,gc_ref_o_17)->data_destroy==reflected_free?dvm__obj(v,gc_ref_o_17)->data:NULL;
 if(!r||r->kind!=4||!r->owner||!r->field||!r->field->cls||!r->field->name||!r->field->type)return false;
 *out=r;return true;
}
/* Field.get, Method.invoke and proxy arguments share primitive boxing. */
static bool box_kind(struct dvm *v,char kind,union dvm_value value,union dvm_value *out) {
 /* Only immutable wrapper factories with an existing rooted cache. Do not
  * share proxy arrays, Float/Double, or arbitrary timestamp Long instances.
  * Constructors remain independent; narrowing matches their old semantics. */
 if(perf_leak_small_boxing())switch(kind){
  case 'Z':return java_basics_call(v,"java/lang/Boolean","valueOf","(Z)Ljava/lang/Boolean;",0,&value,1,out);
  case 'B':value.i=(int8_t)value.i;return small_boxes_call(v,"java/lang/Byte","valueOf","(B)Ljava/lang/Byte;",0,&value,1,out);
  case 'S':value.i=(int16_t)value.i;return small_boxes_call(v,"java/lang/Short","valueOf","(S)Ljava/lang/Short;",0,&value,1,out);
  case 'I':return java_basics_call(v,"java/lang/Integer","valueOf","(I)Ljava/lang/Integer;",0,&value,1,out);
 }
 const char *cls,*sig;
 switch(kind){
  case 'Z':cls="java/lang/Boolean";sig="(Z)V";break;
  case 'B':cls="java/lang/Byte";sig="(B)V";break;
  case 'S':cls="java/lang/Short";sig="(S)V";break;
  case 'C':cls="java/lang/Character";sig="(C)V";break;
  case 'I':cls="java/lang/Integer";sig="(I)V";break;
  case 'J':return java_basics_call(v,"java/lang/Long","valueOf","(J)Ljava/lang/Long;",0,&value,1,out);
  case 'F':cls="java/lang/Float";sig="(F)V";break;
  case 'D':cls="java/lang/Double";sig="(D)V";break;
  default:return fail(v,"java/lang/InternalError","primitive return type");
 }
 out->l=box_value(v,cls,sig,value);
 if(out->l)return true;
 if(dvm_exception(v))return true;
 return fail(v,"java/lang/OutOfMemoryError","reflection boxing");
}
static bool field_box(struct dvm *v,struct dvm_field *f,union dvm_value value,union dvm_value *out) {
 return box_kind(v,f->kind,value,out);
}
/* Metadata names have VM/class lifetime and immutable contents. Sharing is
 * safe even when guest code retains the result: this object is never reset.
 * The existing intern table is per-VM, pinned and visited by core roots.
 * Restrict this helper to metadata, not arbitrary application strings. */
static dvm_ref metadata_name(struct dvm *v,const char *name){
 return perf_leak_metadata_names()?dvm__intern(v,name):dvm_new_string(v,name);
}

static struct dvm_class *proxy_class_for(struct dvm *vm, dvm_ref loader,
                                        struct dvm_class *const *interfaces, int count)
{
 for(int i=0;i<vm->nclasses;i++){
  struct dvm_class *klass=vm->classes[i];
  if(klass->generated_proxy&&klass->defining_loader==loader&&klass->nifaces==count&&
     (!count||!memcmp(klass->ifaces,interfaces,(size_t)count*sizeof *interfaces)))
   return klass;
 }
 struct dvm_class *base=dvm_find_class_or_external(vm,"java/lang/reflect/Proxy");
 if(!base||base->islots<0||base->islots>=65535)return NULL;
 struct dvm_class **list=count?malloc((size_t)count*sizeof *list):NULL;
 struct dvm_field *field=calloc(1,sizeof *field);
 if((count&&!list)||!field){free(list);free(field);return NULL;}
 if(count)memcpy(list,interfaces,(size_t)count*sizeof *list);
 char name[96];
 snprintf(name,sizeof name,"Lsdktrace/generated/Proxy%d;",vm->nclasses);
 struct dvm_class *klass=dvm__register_builtin(vm,name);
 if(!klass){free(list);free(field);return NULL;}
 klass->super=base;
 klass->access=DEX_ACC_PUBLIC|DEX_ACC_FINAL;
 klass->init_state=2;
 klass->ifaces=list;
 klass->nifaces=count;
 klass->defining_loader=loader;
 klass->ifields=field;
 klass->nifields=1;
 klass->islots=base->islots+1;
 *field=(struct dvm_field){.name="h",.type="Ljava/lang/reflect/InvocationHandler;",
  .access=DEX_ACC_PRIVATE|DEX_ACC_FINAL,.cls=klass,.slot=(uint16_t)base->islots,.width=1,.kind='L'};
 klass->generated_proxy=true;
 return klass;
}

static bool create_java_proxy(struct dvm *vm,const union dvm_value *args,union dvm_value *out)
{
 if(!args[1].l||!args[2].l)return fail(vm,"java/lang/NullPointerException","proxy interfaces or handler");
 if(!is_class_array(vm,args[1].l))return fail(vm,"java/lang/IllegalArgumentException","proxy interfaces");
 const dvm_ref gc_ref_handler_18=(args[2].l); if(!gc_operation_admit(vm,gc_ref_handler_18))return false;
 struct dvm_class *contract=dvm_find_class_or_external(vm,"java/lang/reflect/InvocationHandler");
 if(!dvm__obj(vm,gc_ref_handler_18)||!contract||!dvm__class_assignable(vm,dvm__obj(vm,gc_ref_handler_18)->cls,contract))
  return fail(vm,"java/lang/IllegalArgumentException","proxy handler");
 if(args[0].l){
  union dvm_value loader={0};
  dvm_ref type=dvm_class_object(vm,dvm__obj(vm,gc_ref_handler_18)->cls);
  if(!type||!discovery_runtime_call(vm,"java/lang/Class","getClassLoader","()Ljava/lang/ClassLoader;",type,NULL,0,&loader)||
     loader.l!=args[0].l)
   return fail(vm,"java/lang/IllegalArgumentException","proxy loader");
 }
 uint32_t count=dvm_array_length(vm,args[1].l);
 struct dvm_class *interfaces[64];
 if(count>sizeof interfaces/sizeof interfaces[0])
  return fail(vm,"java/lang/UnsupportedOperationException","proxy interface limit");
 const dvm_ref *entries=({const dvm_ref gc_payload_ref_7=(args[1].l);if(!gc_operation_admit(vm,gc_payload_ref_7))return 0;dvm_array_data(vm,gc_payload_ref_7);});
 for(uint32_t i=0;i<count;i++){
  if(!entries[i])return fail(vm,"java/lang/NullPointerException","proxy interface");
  const dvm_ref gc_ref_type_19=(entries[i]); if(!gc_operation_admit(vm,gc_ref_type_19))return false;
  if(!dvm__obj(vm,gc_ref_type_19)||dvm__obj(vm,gc_ref_type_19)->kind!=DVM_OBJ_CLASS||!dvm__obj(vm,gc_ref_type_19)->klass||!dvm__obj(vm,gc_ref_type_19)->klass->is_interface||
     (!args[0].l&&!dvm__obj(vm,gc_ref_type_19)->klass->external)){
   if(dvm__obj(vm,gc_ref_type_19)&&dvm__obj(vm,gc_ref_type_19)->kind==DVM_OBJ_CLASS&&dvm__obj(vm,gc_ref_type_19)->klass){
    static unsigned proxy_reject_probe;
    if(proxy_reject_probe<8){proxy_reject_probe++;FILE*f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt","a");
     if(f){fprintf(f,"PROXY_IFACE_REJECT name=%s is_interface=%d external=%d loader_null=%d\n",
       dvm__obj(vm,gc_ref_type_19)->klass->name?dvm__obj(vm,gc_ref_type_19)->klass->name:"?",dvm__obj(vm,gc_ref_type_19)->klass->is_interface?1:0,
       dvm__obj(vm,gc_ref_type_19)->klass->external?1:0,args[0].l?0:1);fclose(f);}}
   }
   return fail(vm,"java/lang/IllegalArgumentException","proxy interface or loader visibility");
  }
  for(uint32_t j=0;j<i;j++)
   if(interfaces[j]==dvm__obj(vm,gc_ref_type_19)->klass)return fail(vm,"java/lang/IllegalArgumentException","duplicate proxy interface");
  interfaces[i]=dvm__obj(vm,gc_ref_type_19)->klass;
 }
 struct dvm_class *klass=proxy_class_for(vm,args[0].l,interfaces,(int)count);
 out->l=klass?dvm_new_object(vm,klass):0;
 const dvm_ref gc_ref_proxy_20=(out->l); if(!gc_operation_admit(vm,gc_ref_proxy_20))return false;
 if(!dvm__obj(vm,gc_ref_proxy_20))return fail(vm,"java/lang/OutOfMemoryError","proxy instance");
 dvm__obj(vm,gc_ref_proxy_20)->slots[klass->ifields[0].slot].l=args[2].l;
 FILE *log=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt","a");
 if(log){
  fprintf(log,"JAVA_PROXY_CREATED ref=%u handler=%u interfaces=%u first=%s\n",
          (unsigned)out->l,(unsigned)args[2].l,(unsigned)count,count?interfaces[0]->name:"-");
  fclose(log);
 }
 return true;
}

static bool dvm_proxy_try_invoke_operation(struct dvm *vm,dvm_ref self,struct dvm_class *decl,
                          const char *name,const char *sig,const union dvm_value *args,
                          int nargs,union dvm_value *out)
{
 if(!gc_operation_admit(vm,self))return false;
 const dvm_ref proxy_ref=self;
 if(vm!=active||!dvm__obj(vm,proxy_ref)||!dvm__obj(vm,proxy_ref)->cls||!dvm__obj(vm,proxy_ref)->cls->generated_proxy)return false;
 if(leak_scope_current)leak_scope_cancel(vm,LEAK_NESTED);
 dvm_leak_reap_pairs(vm);
 struct dvm_class *klass=dvm__obj(vm,proxy_ref)->cls;
 struct dvm_method *method=NULL;
 if(decl&&!strcmp(decl->name,"java/lang/Object"))
  method=dvm_find_method(vm,decl,name,sig);
 for(int i=0;!method&&i<klass->nifaces;i++)
  method=dvm_find_method(vm,klass->ifaces[i],name,sig);
 if(!method||(method->access&DEX_ACC_STATIC))return false;
 dvm_ref handler=dvm__obj(vm,proxy_ref)->slots[klass->ifields[0].slot].l;
 const dvm_ref gc_ref_handler_object_21=(handler); if(!gc_operation_admit(vm,gc_ref_handler_object_21))return false;
 struct dvm_method *invoke=dvm__obj(vm,gc_ref_handler_object_21)?dvm_find_method(vm,dvm__obj(vm,gc_ref_handler_object_21)->cls,"invoke",
    "(Ljava/lang/Object;Ljava/lang/reflect/Method;[Ljava/lang/Object;)Ljava/lang/Object;"):NULL;
 if(!invoke)return fail(vm,"java/lang/AbstractMethodError","InvocationHandler.invoke");
 union dvm_value reflected_method={0};
 proxy_method_cache *cached=proxy_methods;for(;cached&&(cached->proxy!=klass||cached->method!=method);cached=cached->next);
 if(cached){
  /* Strong handle checks token generation and object identity generation. */
  reflected_method.l=dvm_gc_handle_ref(vm,cached->root);
  if(!reflected_method.l||reflected_method.l!=cached->object||!gc_operation_admit(vm,reflected_method.l))return fail(vm,"java/lang/IllegalStateException","proxy Method cache lost strong identity");
  struct dvm_object *body=dvm__obj(vm,reflected_method.l);
  reflected *entry=body&&body->data_destroy==reflected_free?body->data:NULL;
  if(!entry||entry->kind!=5||entry->method!=method)return fail(vm,"java/lang/IllegalStateException","proxy Method cache metadata identity");
  proxy_method_hits++;
 }
 else{
  new_method_object(vm,method->cls,5,method,&reflected_method);
  if(reflected_method.l&&!dvm_exception(vm)){
   cached=calloc(1,sizeof *cached);if(!cached)return fail(vm,"java/lang/OutOfMemoryError","proxy Method cache");
   dvm_gc_handle root=dvm_gc_handle_new(vm,reflected_method.l,DVM_GC_HANDLE_STRONG);if(!root){free(cached);return true;}
   *cached=(proxy_method_cache){.proxy=klass,.method=method,.object=reflected_method.l,.root=root,.next=proxy_methods};proxy_methods=cached;dvm_pin(vm,cached->object);proxy_method_misses++;
  }
 }
 /* No periodic proxy-cache filesystem diagnostics in the invocation path. */
 if(!reflected_method.l||dvm_exception(vm))return true;
 dvm_pin(vm,reflected_method.l);
 dvm_ref values=nargs?dvm_new_array(vm,'L',"Ljava/lang/Object;",(uint32_t)nargs):0;
 if(nargs&&!values){dvm_unpin(vm,reflected_method.l);return fail(vm,"java/lang/OutOfMemoryError","proxy arguments");}
 if(values)dvm_pin(vm,values);
 const char *parameter=sig+1;
 for(int i=0;i<nargs;i++){
  const char *end=field_desc_end(parameter);
  if(!end){fail(vm,"java/lang/InternalError","proxy signature");break;}
  char kind=dvm__kind_of(parameter);
  union dvm_value boxed={0};
  if(kind=='L')boxed.l=args[i].l;
  else box_kind(vm,kind,args[i],&boxed);
  if(dvm_exception(vm))break;
  ((dvm_ref*)({const dvm_ref gc_payload_ref_8=(values);if(!gc_operation_admit(vm,gc_payload_ref_8))return 0;dvm_array_data(vm,gc_payload_ref_8);}))[i]=boxed.l;
  parameter=end;
 }
 union dvm_value result={0};
 union dvm_value invoke_args[]={{.l=self},{.l=reflected_method.l},{.l=values}};
 struct leak_scope ownership={0};
 if(perf_leak_proxy_reclaim()&&!dvm_exception(vm)&&nargs==1&&!strcmp(name,"doFrame")&&!strcmp(sig,"(J)V")&&
    method->cls&&method->cls->name&&!strcmp(method->cls->name,"android/view/Choreographer$FrameCallback")&&
    dvm__obj(vm,gc_ref_handler_object_21)->cls->name&&!strcmp(dvm__obj(vm,gc_ref_handler_object_21)->cls->name,"bitter/jnibridge/a")){
  dvm_ref box=((dvm_ref*)({const dvm_ref gc_payload_ref_9=(values);if(!gc_operation_admit(vm,gc_payload_ref_9))return 0;dvm_array_data(vm,gc_payload_ref_9);}))[0];
  leak_scope_begin(&ownership,vm,values,box,args[0].j);
 }
 if(!dvm_exception(vm)&&!dvm_call(vm,invoke,handler,invoke_args,3,&result)&&!dvm_exception(vm))
  fail(vm,"java/lang/InternalError","proxy handler failed");
 leak_scope_end(&ownership,!dvm_exception(vm),result.l);
 if(values)dvm_unpin(vm,values);
 dvm_unpin(vm,reflected_method.l);
 if(dvm_exception(vm))return true;
 if(method->ret_kind=='V')return true;
 if(method->ret_kind=='L'){
  const char *return_type=strchr(sig,')')+1;
  const dvm_ref gc_ref_value_22=(result.l); if(!gc_operation_admit(vm,gc_ref_value_22))return false;
  if(result.l&&(!dvm__obj(vm,gc_ref_value_22)||!dvm__class_assignable(vm,dvm__obj(vm,gc_ref_value_22)->cls,dvm__class_by_desc(vm,return_type))))
   return fail(vm,"java/lang/ClassCastException","proxy return type");
  out->l=result.l;
  return true;
 }
 if(!result.l)return fail(vm,"java/lang/NullPointerException","proxy primitive return");
 if(!unbox_primitive(vm,method->ret_kind,result.l,out))
  return fail(vm,"java/lang/ClassCastException","proxy primitive return");
 return true;
}
bool dvm_proxy_try_invoke(struct dvm *vm,dvm_ref self,struct dvm_class *decl,
                          const char *name,const char *sig,const union dvm_value *args,
                          int nargs,union dvm_value *out){
 struct gc_operation operation __attribute__((cleanup(gc_operation_end)));
 if(!gc_operation_begin(vm,&operation))return false;
 bool result=dvm_proxy_try_invoke_operation(vm,self,decl,name,sig,args,nargs,out);
 const char *return_type=sig?strchr(sig,')'):NULL;
 gc_operation_result(&operation,sig,result&&!dvm_exception(vm)&&out&&return_type&&(return_type[1]=='L'||return_type[1]=='[')?out->l:0);
 return result;
}

static bool field_get(struct dvm *v,dvm_ref self,dvm_ref receiver,union dvm_value *out) {
 reflected *r;
 if(!field_reflected(v,self,&r))return fail(v,"java/lang/IllegalArgumentException","Field.get receiver");
 struct dvm_field *f=r->field;
 union dvm_value value={0};
 if(r->statics){
  /* Java ignores the receiver for a static field, null included.  Reading
   * one initialises the declaring class exactly as a static get would. */
  if(!dvm_init_class(v,f->cls))return true;
  if(!f->cls->sslots||f->slot>=f->cls->nsslots)return fail(v,"java/lang/InternalError","static field slot");
  value=f->cls->sslots[f->slot];
 }else{
  if(!receiver)return fail(v,"java/lang/NullPointerException","Field.get receiver");
  const dvm_ref gc_ref_o_23=(receiver); if(!gc_operation_admit(v,gc_ref_o_23))return false;
  if(!dvm__obj(v,gc_ref_o_23)||!dvm__obj(v,gc_ref_o_23)->cls||!dvm__class_assignable(v,dvm__obj(v,gc_ref_o_23)->cls,r->owner))return fail(v,"java/lang/IllegalArgumentException","Field.get receiver type");
  if(!dvm__obj(v,gc_ref_o_23)->slots||f->slot>=dvm__obj(v,gc_ref_o_23)->cls->islots)return fail(v,"java/lang/InternalError","instance field slot");
  value=dvm__obj(v,gc_ref_o_23)->slots[f->slot];
 }
 if(f->kind=='L'){out->l=value.l;return true;}
 return field_box(v,f,value,out);
}
static bool field_set(struct dvm *v,dvm_ref self,dvm_ref receiver,dvm_ref input){
 reflected *r;if(!field_reflected(v,self,&r))return fail(v,"java/lang/IllegalArgumentException","Field.set receiver");struct dvm_field *f=r->field;
 if(!r->accessible&&(!(f->access&DEX_ACC_PUBLIC)||!(r->owner->access&DEX_ACC_PUBLIC)))return fail(v,"java/lang/IllegalAccessException","field is not accessible");
 if((f->access&DEX_ACC_FINAL)&&(r->statics||!r->accessible))return fail(v,"java/lang/IllegalAccessException","final field");
 if(!r->statics){const dvm_ref gc_ref_o_24=(receiver); if(!gc_operation_admit(v,gc_ref_o_24))return false;if(!receiver)return fail(v,"java/lang/NullPointerException","Field.set target");if(!dvm__obj(v,gc_ref_o_24)||!dvm__obj(v,gc_ref_o_24)->cls||!dvm__class_assignable(v,dvm__obj(v,gc_ref_o_24)->cls,r->owner))return fail(v,"java/lang/IllegalArgumentException","Field.set target type");}
 union dvm_value value={0};if(f->kind=='L'){if(input){const dvm_ref gc_ref_o_25=(input); if(!gc_operation_admit(v,gc_ref_o_25))return false;struct dvm_class *type=dvm__class_by_desc(v,f->type);if(!dvm__obj(v,gc_ref_o_25)||!dvm__obj(v,gc_ref_o_25)->cls||!type||!dvm__class_assignable(v,dvm__obj(v,gc_ref_o_25)->cls,type))return fail(v,"java/lang/IllegalArgumentException","Field.set value type");}value.l=input;}else if(!unbox_primitive(v,f->kind,input,&value)){if(dvm_exception(v))return true;return fail(v,"java/lang/IllegalArgumentException","Field.set primitive value");}
 if(r->statics){if(!dvm_init_class(v,f->cls))return true;if(!f->cls->sslots||f->slot>=f->cls->nsslots)return fail(v,"java/lang/InternalError","static field slot");f->cls->sslots[f->slot]=value;}else if(!dvm__set_field_from(v,receiver,r->owner,f->name,f->type,value))return fail(v,"java/lang/InternalError","Field.set slot");return true;
}
static bool method_reflected(struct dvm *v,dvm_ref self,reflected **out) {
 const dvm_ref gc_ref_o_26=(self); if(!gc_operation_admit(v,gc_ref_o_26))return false;
 reflected *r=dvm__obj(v,gc_ref_o_26)&&dvm__obj(v,gc_ref_o_26)->data_destroy==reflected_free?dvm__obj(v,gc_ref_o_26)->data:NULL;
 if(!r||r->kind!=5||!r->method||!r->method->cls||!r->method->name||!r->method->sig)return false;
 *out=r;return true;
}
/* Method.getName()/getParameterTypes() also answer for the two special
 * methods, so they accept kinds 1, 2 and 5. */
static bool any_method_reflected(struct dvm *v,dvm_ref self,reflected **out) {
 const dvm_ref gc_ref_o_27=(self); if(!gc_operation_admit(v,gc_ref_o_27))return false;
 reflected *r=dvm__obj(v,gc_ref_o_27)&&dvm__obj(v,gc_ref_o_27)->data_destroy==reflected_free?dvm__obj(v,gc_ref_o_27)->data:NULL;
 if(!r||(r->kind!=1&&r->kind!=2&&r->kind!=5))return false;
 *out=r;return true;
}
/* The annotation argument of isAnnotationPresent()/getAnnotation() must be a
 * real Class.  null is the NullPointerException Java specifies; any other
 * value cannot come from verified bytecode, but can still reach a native hook
 * through an earlier reflective call, so it is rejected explicitly. */
static bool annotation_class(struct dvm *v,dvm_ref ref,struct dvm_class **out) {
 if(!ref){dvm__throw(v,"java/lang/NullPointerException","annotation class");return false;}
 const dvm_ref gc_ref_o_28=(ref); if(!gc_operation_admit(v,gc_ref_o_28))return false;
 if(!dvm__obj(v,gc_ref_o_28)||dvm__obj(v,gc_ref_o_28)->kind!=DVM_OBJ_CLASS||!dvm__obj(v,gc_ref_o_28)->klass||!dvm__obj(v,gc_ref_o_28)->klass->desc){dvm__throw(v,"java/lang/IllegalArgumentException","annotation class");return false;}
 *out=dvm__obj(v,gc_ref_o_28)->klass;return true;
}
/* Runtime-visible annotations are read straight from the declaring dex.
 * dex_field_annotation()/dex_method_annotation() only match visibility==1
 * entries and decode the encoded_annotation themselves; nothing is invented
 * when the annotation is absent.  Builtin members have no dex, so they report
 * no annotations instead of guessing.  A class_def_idx belongs to the dex the
 * class was loaded from, which is exactly the file stored on the member. */
static bool field_annotation(struct dvm_field *f,const char *desc,uint32_t *off) {
 return f&&f->cls&&f->name&&f->cls->dex&&dex_field_annotation(f->cls->dex,f->cls->class_def_idx,f->name,desc,off);
}
static bool method_annotation(struct dvm_method *m,const char *desc,uint32_t *off) {
 return m&&m->cls&&m->name&&m->sig&&m->dex&&dex_method_annotation(m->dex,m->cls->class_def_idx,m->name,m->sig,desc,off);
}
/* Materialise the annotation the same way annotation_value_to_dvm() does: an
 * instance of the annotation interface whose zero-argument methods are the
 * annotation_element_builtin bound at class load, with the encoded value's
 * dex and offset attached.  Calling RequestParam.name() then decodes the real
 * element (or its AnnotationDefault) instead of returning a fabricated value.
 * Both the dex and the class metadata live until dvm_destroy(). */
static bool new_annotation(struct dvm *v,struct dvm_class *type,struct dex_file *dex,uint32_t off,union dvm_value *out) {
 dvm_ref ref=dvm_new_object(v,type);
 const dvm_ref gc_ref_o_29=(ref); if(!gc_operation_admit(v,gc_ref_o_29))return false;
 if(!dvm__obj(v,gc_ref_o_29))return fail(v,"java/lang/OutOfMemoryError","annotation instance");
 dvm__obj(v,gc_ref_o_29)->annotation_dex=dex;dvm__obj(v,gc_ref_o_29)->annotation_off=off;out->l=ref;return true;
}
/* Preserve the pre-wrap cause of a failed Method.invoke: the guest catches
 * InvocationTargetException and drops the field, so the original type and
 * the throwing dex frame are otherwise lost.  Do not print the message:
 * apkSign-related exceptions may contain request/signature material. */
static void trace_invoke_cause(struct dvm *v,struct dvm_class *owner,const char *name) {
 dvm_ref exception=dvm_exception(v);
 const char *type="none";
 if(exception){const dvm_ref gc_ref_e_30=(exception); if(!gc_operation_admit(v,gc_ref_e_30))return ;if(dvm__obj(v,gc_ref_e_30)&&dvm__obj(v,gc_ref_e_30)->cls&&dvm__obj(v,gc_ref_e_30)->cls->name)type=dvm__obj(v,gc_ref_e_30)->cls->name;}
 const struct dvm_method *at=v->nexc_trace>0?v->exc_trace[0]:NULL;
 sdk_trace_fprintf(stderr,"REFLECT_INVOKE_CAUSE class=%s name=%s cause_type=%s throwing=%s.%s wrapped=InvocationTargetException\n",
  owner&&owner->name?owner->name:"?",name?name:"?",type,
  at&&at->cls&&at->cls->name?at->cls->name:"?",at&&at->name?at->name:"?");
}
/* The diagnostic view classes are real runtime types: view_state_runtime
 * implements their <init> and layout behavior, but their DEX carries no
 * member metadata.  Zero declared members is the truthful answer for them.
 * Any other external class still fails closed because nothing models its
 * members, and inventing an empty list there would be a silent lie. */
static bool reflection_runtime_method_handle_operation(struct dvm *v, dvm_ref object,
                                      struct dvm_method **out) {
 if(!out)return false;
 *out=NULL;
 const dvm_ref gc_ref_o_31=(object); if(!gc_operation_admit(v,gc_ref_o_31))return false;
 reflected *r=dvm__obj(v,gc_ref_o_31)&&dvm__obj(v,gc_ref_o_31)->data_destroy==reflected_free?dvm__obj(v,gc_ref_o_31)->data:NULL;
 if(!r)return false;
 if(r->kind==3&&r->ctor){*out=r->ctor;return true;}
 if(r->kind!=5||!r->method)return false;
 *out=r->method;
 return true;
}
bool reflection_runtime_method_handle(struct dvm *v, dvm_ref object,
                                      struct dvm_method **out){
 struct gc_operation operation __attribute__((cleanup(gc_operation_end)));
 if(!gc_operation_begin(v,&operation))return false;
 bool result=reflection_runtime_method_handle_operation(v,object,out);
 return result;
}

static bool reflection_runtime_field_handle_operation(struct dvm *v, dvm_ref object,
                                     struct dvm_field **field,
                                     struct dvm_class **owner,
                                     bool *is_static) {
 if(!field||!owner||!is_static)return false;
 *field=NULL;*owner=NULL;*is_static=false;
 const dvm_ref gc_ref_o_32=(object); if(!gc_operation_admit(v,gc_ref_o_32))return false;
 reflected *r=dvm__obj(v,gc_ref_o_32)&&dvm__obj(v,gc_ref_o_32)->data_destroy==reflected_free?dvm__obj(v,gc_ref_o_32)->data:NULL;
 if(!r||r->kind!=4||!r->field||!r->owner)return false;
 *field=r->field;*owner=r->owner;*is_static=r->statics;
 return true;
}
bool reflection_runtime_field_handle(struct dvm *v, dvm_ref object,
                                     struct dvm_field **field,
                                     struct dvm_class **owner,
                                     bool *is_static){
 struct gc_operation operation __attribute__((cleanup(gc_operation_end)));
 if(!gc_operation_begin(v,&operation))return false;
 bool result=reflection_runtime_field_handle_operation(v,object,field,owner,is_static);
 return result;
}

static bool external_without_metadata(struct dvm_class *klass) {
 if(!klass||!klass->external||!klass->name)return false;
 return !strcmp(klass->name,"android/widget/FrameLayout")||
        !strcmp(klass->name,"android/view/ViewGroup")||
        !strcmp(klass->name,"android/view/View")||
        /* java.lang.Object truthfully declares no fields; the reflection walk
         * over the view super chain ends here. */
        !strcmp(klass->name,"java/lang/Object")||
        /* Host-registered Build types publish real static fields (enumerated
         * above) but register no methods, so getDeclaredMethods() truthfully
         * returns an empty array for them. */
        !strcmp(klass->name,"android/os/Build")||
        !strcmp(klass->name,"android/os/Build$VERSION")||
        /* 宿主 Activity/Context 链由 activity_runtime 分派，DEX 没有它们的
         * 方法表；反射报告零声明方法（真实方法查找走 GetMethodID 与平台声明）。 */
        !strcmp(klass->name,"android/app/Activity")||
        !strcmp(klass->name,"android/view/ContextThemeWrapper")||
        !strcmp(klass->name,"android/content/ContextWrapper")||
        !strcmp(klass->name,"android/content/Context");
}
static bool reflection_runtime_call_operation(struct dvm *v,const char *c,const char *m,const char *s,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out) {
 if(v!=active)return false;
 if(!strcmp(c,"java/lang/Class")&&!strcmp(m,"toString")&&!strcmp(s,"()Ljava/lang/String;")&&!n){
  const dvm_ref gc_ref_object_33=(self); if(!gc_operation_admit(v,gc_ref_object_33))return true;
  if(!dvm__obj(v,gc_ref_object_33)||dvm__obj(v,gc_ref_object_33)->kind!=DVM_OBJ_CLASS||!dvm__obj(v,gc_ref_object_33)->klass)return fail(v,"java/lang/NullPointerException","Class.toString receiver");
  struct dvm_class *type=dvm__obj(v,gc_ref_object_33)->klass;
  const char *name=type->is_array?type->desc:type->name;
  const char *prefix=type->is_primitive?"":type->is_interface?"interface ":"class ";
  if(type->is_primitive){
   const char *codes="VZBCSIJFD";
   const char *names[]={"void","boolean","byte","char","short","int","long","float","double"};
   const char *code=type->desc&&type->desc[0]?strchr(codes,type->desc[0]):NULL;
   if(!code)return fail(v,"java/lang/InternalError","Class.toString primitive descriptor");
   name=names[code-codes];
  }
  if(!name)return fail(v,"java/lang/InternalError","Class.toString class name");
  size_t prefix_length=strlen(prefix),length=strlen(name);
  char *value=malloc(prefix_length+length+1);
  if(!value)return fail(v,"java/lang/OutOfMemoryError","Class.toString");
  memcpy(value,prefix,prefix_length);
  for(size_t i=0;i<length;i++)value[prefix_length+i]=name[i]=='/'?'.':name[i];
  value[prefix_length+length]=0;out->l=dvm_new_string_n(v,value,prefix_length+length);free(value);
  if(!out->l)return fail(v,"java/lang/OutOfMemoryError","Class.toString result");
  return true;
 }
 if(!strcmp(c,"java/lang/reflect/AccessibleObject")&&!strcmp(m,"setAccessible")&&!strcmp(s,"(Z)V")&&n==1){
  const dvm_ref gc_ref_object_34=(self); if(!gc_operation_admit(v,gc_ref_object_34))return true;reflected *r=dvm__obj(v,gc_ref_object_34)&&dvm__obj(v,gc_ref_object_34)->data_destroy==reflected_free?dvm__obj(v,gc_ref_object_34)->data:NULL;
  if(!self)return fail(v,"java/lang/NullPointerException","AccessibleObject receiver");
  if(!r)return fail(v,"java/lang/IllegalArgumentException","AccessibleObject receiver");
  const char *target=r->kind==3?"java/lang/reflect/Constructor":r->kind==4?"java/lang/reflect/Field":"java/lang/reflect/Method";
  return reflection_runtime_call(v,target,m,s,self,a,n,out);
 }
 if(!strcmp(c,"java/lang/Class")&&!strcmp(m,"getAnnotation")&&!strcmp(s,"(Ljava/lang/Class;)Ljava/lang/annotation/Annotation;")&&n==1){
  const dvm_ref gc_ref_co_35=(self); if(!gc_operation_admit(v,gc_ref_co_35))return true;struct dvm_class *type=NULL;
  if(!dvm__obj(v,gc_ref_co_35)||dvm__obj(v,gc_ref_co_35)->kind!=DVM_OBJ_CLASS||!dvm__obj(v,gc_ref_co_35)->klass)return fail(v,"java/lang/NullPointerException","Class.getAnnotation receiver");
  if(!annotation_class(v,a[0].l,&type))return true;
  uint32_t inherited_off=0;bool inherited=type->dex&&dex_class_annotation(type->dex,type->class_def_idx,"Ljava/lang/annotation/Inherited;",&inherited_off);
  for(struct dvm_class *k=dvm__obj(v,gc_ref_co_35)->klass;k;k=inherited&&!k->is_interface?k->super:NULL){uint32_t offset=0;if(k->dex&&dex_class_annotation(k->dex,k->class_def_idx,type->desc,&offset))return new_annotation(v,type,k->dex,offset,out);}
  out->l=0;return true;
 }
 if(!strcmp(c,"java/lang/Class")&&!strcmp(m,"getGenericSuperclass")&&!strcmp(s,"()Ljava/lang/reflect/Type;")&&!n)return generic_superclass(v,self,false,out);
 if(!strcmp(c,"java/lang/Class")&&!strcmp(m,"getGenericInterfaces")&&!strcmp(s,"()[Ljava/lang/reflect/Type;")&&!n)return generic_superclass(v,self,true,out);
 if(!strcmp(c,"java/lang/Class")&&!strcmp(m,"getTypeParameters")&&!strcmp(s,"()[Ljava/lang/reflect/TypeVariable;")&&!n){const dvm_ref gc_ref_co_36=(self); if(!gc_operation_admit(v,gc_ref_co_36))return true;if(!dvm__obj(v,gc_ref_co_36)||dvm__obj(v,gc_ref_co_36)->kind!=DVM_OBJ_CLASS||!dvm__obj(v,gc_ref_co_36)->klass)return fail(v,"java/lang/NullPointerException","type parameters receiver");char names[32][128];int count;if(!formal_names(dvm__obj(v,gc_ref_co_36)->klass,NULL,names,&count))return fail(v,"java/lang/reflect/GenericSignatureFormatError","formal parameters");out->l=dvm_new_array(v,'L',"Ljava/lang/reflect/TypeVariable;",count);for(int i=0;out->l&&i<count;i++){dvm_ref t=new_variable(v,dvm__obj(v,gc_ref_co_36)->klass,names[i]);if(!t)return true;((dvm_ref*)({const dvm_ref gc_payload_ref_10=(out->l);if(!gc_operation_admit(v,gc_payload_ref_10))return true;dvm_array_data(v,gc_payload_ref_10);}))[i]=t;}return true;}
 const dvm_ref gc_ref_generic_object_37=((self)?(self):0); if(!gc_operation_admit(v,gc_ref_generic_object_37))return true;
 if(dvm__obj(v,gc_ref_generic_object_37)&&dvm__obj(v,gc_ref_generic_object_37)->data_destroy==variable_free){generic_variable *t=dvm__obj(v,gc_ref_generic_object_37)->data;if(!n&&!strcmp(m,"getGenericDeclaration")&&!strcmp(s,"()Ljava/lang/reflect/GenericDeclaration;")){out->l=dvm_class_object(v,t->declaration);return true;}if(!n&&(!strcmp(m,"getName")||!strcmp(m,"toString")||!strcmp(m,"getTypeName"))&&!strcmp(s,"()Ljava/lang/String;")){out->l=dvm_new_string(v,t->name);return true;}if(n==1&&!strcmp(m,"equals")&&!strcmp(s,"(Ljava/lang/Object;)Z")){const dvm_ref gc_ref_other_38=(a[0].l); if(!gc_operation_admit(v,gc_ref_other_38))return true;generic_variable *u=dvm__obj(v,gc_ref_other_38)&&dvm__obj(v,gc_ref_other_38)->data_destroy==variable_free?dvm__obj(v,gc_ref_other_38)->data:NULL;out->i=u&&u->declaration==t->declaration&&!strcmp(u->name,t->name);return true;}if(!n&&!strcmp(m,"hashCode")&&!strcmp(s,"()I")){uint32_t hash=0;for(const unsigned char *p=(const unsigned char*)t->name;*p;p++)hash=hash*31+*p;out->i=(int32_t)(hash^dvm_class_object(v,t->declaration));return true;}}
 if(dvm__obj(v,gc_ref_generic_object_37)&&dvm__obj(v,gc_ref_generic_object_37)->data_destroy==generic_free&&!n){
  generic_type *t=dvm__obj(v,gc_ref_generic_object_37)->data;
  if(!strcmp(m,"getRawType")&&!strcmp(s,"()Ljava/lang/reflect/Type;")){out->l=t->raw;return true;}
  if(!strcmp(m,"getOwnerType")&&!strcmp(s,"()Ljava/lang/reflect/Type;")){out->l=t->owner;return true;}
  if(!strcmp(m,"getActualTypeArguments")&&!strcmp(s,"()[Ljava/lang/reflect/Type;")){uint32_t count=dvm_array_length(v,t->args);out->l=dvm_new_array(v,'L',"Ljava/lang/reflect/Type;",count);if(out->l)memcpy(({const dvm_ref gc_payload_ref_11=(out->l);if(!gc_operation_admit(v,gc_payload_ref_11))return true;dvm_array_data(v,gc_payload_ref_11);}),({const dvm_ref gc_payload_ref_12=(t->args);if(!gc_operation_admit(v,gc_payload_ref_12))return true;dvm_array_data(v,gc_payload_ref_12);}),count*sizeof(dvm_ref));return true;}
 }
 /* java.lang.reflect.Modifier: pure bit tests over the int the VM already
  * publishes from getModifiers().  No class metadata is consulted and no
  * value is invented; an unknown predicate falls through unchanged. */
 if(!strcmp(c,"java/lang/reflect/Modifier")&&n==1&&!strcmp(s,"(I)Z")){
  static const struct { const char *name; int bit; } modifier_bits[]={
   {"isPublic",0x1},{"isPrivate",0x2},{"isProtected",0x4},{"isStatic",0x8},
   {"isFinal",0x10},{"isSynchronized",0x20},{"isVolatile",0x40},
   {"isTransient",0x80},{"isNative",0x100},{"isInterface",0x200},
   {"isAbstract",0x400},{"isStrict",0x800}};
  for(size_t i=0;i<sizeof modifier_bits/sizeof modifier_bits[0];++i)
   if(!strcmp(m,modifier_bits[i].name)){out->i=(a[0].i&modifier_bits[i].bit)?1:0;return true;}
  return false;
 }
 if(!strcmp(c,"java/lang/reflect/Proxy")){
  if(!strcmp(m,"newProxyInstance")&&n==3&&
     !strcmp(s,"(Ljava/lang/ClassLoader;[Ljava/lang/Class;Ljava/lang/reflect/InvocationHandler;)Ljava/lang/Object;"))
   return create_java_proxy(v,a,out);
  if(!strcmp(m,"getInvocationHandler")&&n==1&&
     !strcmp(s,"(Ljava/lang/Object;)Ljava/lang/reflect/InvocationHandler;")){
   const dvm_ref gc_ref_proxy_39=(a[0].l); if(!gc_operation_admit(v,gc_ref_proxy_39))return true;
   if(!dvm__obj(v,gc_ref_proxy_39)||!dvm__obj(v,gc_ref_proxy_39)->cls||!dvm__obj(v,gc_ref_proxy_39)->cls->generated_proxy)
    return fail(v,"java/lang/IllegalArgumentException","not a proxy");
   out->l=dvm__obj(v,gc_ref_proxy_39)->slots[dvm__obj(v,gc_ref_proxy_39)->cls->ifields[0].slot].l;
   return true;
  }
 }
 if(!strcmp(c,"java/lang/Throwable")&&!strcmp(m,"addSuppressed")&&!strcmp(s,"(Ljava/lang/Throwable;)V")&&n==1)return add_suppressed(v,self,a[0].l);
 if(!strcmp(c,"java/lang/Throwable")&&!strcmp(m,"getSuppressed")&&!strcmp(s,"()[Ljava/lang/Throwable;")&&n==0)return get_suppressed(v,self,out);
 if(!strcmp(c,"java/lang/Package")&&!strcmp(m,"getName")&&!strcmp(s,"()Ljava/lang/String;")&&n==0)
  return package_get_name_call(v,self,out);
 /* Class.getPackage(): real Package for a named package; null for a
  * primitive, a primitive array, or the default package.  Android resolves an
  * array through its component class, so [Lcom/foo/Bar; yields com.foo. */
 if(!strcmp(c,"java/lang/Class")&&!strcmp(m,"getPackage")&&!strcmp(s,"()Ljava/lang/Package;")&&n==0) {
  const dvm_ref gc_ref_co_40=(self); if(!gc_operation_admit(v,gc_ref_co_40))return true;
  if(!dvm__obj(v,gc_ref_co_40)||dvm__obj(v,gc_ref_co_40)->kind!=DVM_OBJ_CLASS||!dvm__obj(v,gc_ref_co_40)->klass)return fail(v,"java/lang/NullPointerException","Class.getPackage");
  struct dvm_class *k=dvm__obj(v,gc_ref_co_40)->klass;
  if(k->is_array)k=k->elem;
  if(!k||k->is_primitive){out->l=0;return true;}
  char pkg[512];
  if(!class_package_name(v,k,pkg,sizeof pkg)){out->l=0;return true;}
  dvm_ref p=package_create(v,pkg);
  if(!p)return fail(v,"java/lang/OutOfMemoryError","Class.getPackage");
  out->l=p;return true;
 }
 if(!strcmp(c,"java/lang/Class")&&(!strcmp(m,"getConstructor")||!strcmp(m,"getDeclaredConstructor"))&&
    !strcmp(s,"([Ljava/lang/Class;)Ljava/lang/reflect/Constructor;")&&n==1) {
  bool declared=!strcmp(m,"getDeclaredConstructor");
  const dvm_ref gc_ref_co_41=(self); if(!gc_operation_admit(v,gc_ref_co_41))return true;
  if(!dvm__obj(v,gc_ref_co_41)||dvm__obj(v,gc_ref_co_41)->kind!=DVM_OBJ_CLASS||!dvm__obj(v,gc_ref_co_41)->klass)return fail(v,"java/lang/NullPointerException","Class.getConstructor");
  if(!is_class_array(v,a[0].l))return fail(v,"java/lang/IllegalArgumentException","getConstructor parameter types");
  char *sig=parameter_signature(v,a[0].l);if(!sig)return true;
  struct dvm_method *ctor=dvm_find_method(v,dvm__obj(v,gc_ref_co_41)->klass,"<init>",sig);
  /* Class.getConstructor exposes public constructors only.  dvm_find_method()
   * returns the raw method (including a private one), so the access bit has
   * to be checked here rather than inferred from the lookup. */
  if(!ctor||ctor->cls!=dvm__obj(v,gc_ref_co_41)->klass||(!declared&&!(ctor->access&DEX_ACC_PUBLIC))){
   free(sig);
   return fail(v,"java/lang/NoSuchMethodException","constructor");
  }
  reflected *r=calloc(1,sizeof *r);if(!r){free(sig);return fail(v,"java/lang/OutOfMemoryError","Constructor");}
  r->owner=dvm__obj(v,gc_ref_co_41)->klass;r->kind=3;r->ctor=ctor;r->sig=sig;
  out->l=dvm_new_object(v,dvm_find_class_or_external(v,"java/lang/reflect/Constructor"));
  const dvm_ref gc_ref_mo_42=(out->l); if(!gc_operation_admit(v,gc_ref_mo_42))return true;if(!dvm__obj(v,gc_ref_mo_42)){free(r->sig);free(r);return fail(v,"java/lang/OutOfMemoryError","Constructor");}
  dvm__obj(v,gc_ref_mo_42)->data=r;dvm__obj(v,gc_ref_mo_42)->data_destroy=reflected_free;return true;
 }
 /* Class.getConstructors() / getDeclaredConstructors(): every constructor
  * this class declares.  Unity's AndroidJavaObject inspects them before
  * invoking <init> (72rf master data path: NoSuchMethodError on
  * Class.getConstructors).  Constructors carry the dvm_method so
  * Constructor.newInstance resolves the real implementation. */
 if(!strcmp(c,"java/lang/Class")&&(!strcmp(m,"getConstructors")||!strcmp(m,"getDeclaredConstructors"))&&
    !strcmp(s,"()[Ljava/lang/reflect/Constructor;")&&n==0) {
  bool declared=!strcmp(m,"getDeclaredConstructors");
  const dvm_ref gc_ref_co_43=(self); if(!gc_operation_admit(v,gc_ref_co_43))return true;
  if(!dvm__obj(v,gc_ref_co_43)||dvm__obj(v,gc_ref_co_43)->kind!=DVM_OBJ_CLASS||!dvm__obj(v,gc_ref_co_43)->klass)return fail(v,"java/lang/NullPointerException","Class.getConstructors");
  struct dvm_class *klass=dvm__obj(v,gc_ref_co_43)->klass;
  uint32_t count=0;
  if(!klass->is_array&&!klass->is_primitive&&klass->nmethods>0&&klass->methods)
   for(int i=0;i<klass->nmethods;i++){
    struct dvm_method *mm=&klass->methods[i];
    if(!mm->cls||!mm->name||strcmp(mm->name,"<init>"))continue;
    if(!declared&&!(mm->access&DEX_ACC_PUBLIC))continue;
    ++count;
   }
  struct dvm_class *cons_cls=dvm_find_class_or_external(v,"java/lang/reflect/Constructor");
  if(!cons_cls)return fail(v,"java/lang/InternalError","Constructor class");
  dvm_ref arr=dvm_new_array(v,'L',"Ljava/lang/reflect/Constructor;",count);
  if(!arr)return fail(v,"java/lang/OutOfMemoryError","Class.getConstructors array");
  dvm_ref *items=({const dvm_ref gc_payload_ref_13=(arr);if(!gc_operation_admit(v,gc_payload_ref_13))return true;dvm_array_data(v,gc_payload_ref_13);});
  uint32_t at=0;
  if(count&&klass->nmethods>0&&klass->methods)for(int i=0;i<klass->nmethods;i++){
   struct dvm_method *mm=&klass->methods[i];
   if(!mm->cls||!mm->name||strcmp(mm->name,"<init>"))continue;
   if(!declared&&!(mm->access&DEX_ACC_PUBLIC))continue;
   reflected *r=calloc(1,sizeof *r);
   if(!r)return fail(v,"java/lang/OutOfMemoryError","Constructor");
   r->owner=mm->cls;r->kind=3;r->ctor=mm;
   r->sig=mm->sig?strdup(mm->sig):NULL;
   dvm_ref ref=dvm_new_object(v,cons_cls);
   const dvm_ref gc_ref_mo_44=(ref); if(!gc_operation_admit(v,gc_ref_mo_44))return true;
   if(!dvm__obj(v,gc_ref_mo_44)){free(r->sig);free(r);return fail(v,"java/lang/OutOfMemoryError","Constructor");}
   dvm__obj(v,gc_ref_mo_44)->data=r;dvm__obj(v,gc_ref_mo_44)->data_destroy=reflected_free;items[at++]=ref;
  }
  out->l=arr;return true;
 }
 if(!strcmp(c,"java/lang/Class")&&!strcmp(m,"newInstance")&&!strcmp(s,"()Ljava/lang/Object;")&&n==0) {
  const dvm_ref gc_ref_co_45=(self); if(!gc_operation_admit(v,gc_ref_co_45))return true;
  if(!dvm__obj(v,gc_ref_co_45)||dvm__obj(v,gc_ref_co_45)->kind!=DVM_OBJ_CLASS||!dvm__obj(v,gc_ref_co_45)->klass)return fail(v,"java/lang/NullPointerException","Class.newInstance");
  if(dvm__obj(v,gc_ref_co_45)->klass->access&(DEX_ACC_ABSTRACT|DEX_ACC_INTERFACE))return fail(v,"java/lang/InstantiationException","abstract class");
  struct dvm_method *ctor=dvm_find_method(v,dvm__obj(v,gc_ref_co_45)->klass,"<init>","()V");
  if(!ctor)return fail(v,"java/lang/InstantiationException","no zero-argument constructor");
  dvm_ref object=dvm_new_object(v,dvm__obj(v,gc_ref_co_45)->klass);
  if(!object)return fail(v,"java/lang/OutOfMemoryError","Class.newInstance");
  union dvm_value result={0};
  /* Unlike Constructor.newInstance, Class.newInstance does not wrap the
   * constructor's throwable; it leaves it pending for the caller.  A failed
   * builtin with no pending exception must not become a silent success. */
  bool ok=dvm_call(v,ctor,object,NULL,0,&result);
  if(!ok||v->exception){if(!dvm_exception(v))return fail(v,"java/lang/InstantiationException","constructor failed without exception");return true;}
  out->l=object;return true;
 }
 /* Named lookup uses the same real metadata/payload as field enumeration. */
 if(!strcmp(c,"java/lang/Class")&&!strcmp(m,"getDeclaredField")&&!strcmp(s,"(Ljava/lang/String;)Ljava/lang/reflect/Field;")&&n==1){
  const dvm_ref gc_ref_co_46=(self); if(!gc_operation_admit(v,gc_ref_co_46))return true;const dvm_ref gc_ref_name_47=(a[0].l); if(!gc_operation_admit(v,gc_ref_name_47))return true;
  if(!dvm__obj(v,gc_ref_co_46)||dvm__obj(v,gc_ref_co_46)->kind!=DVM_OBJ_CLASS||!dvm__obj(v,gc_ref_co_46)->klass)return fail(v,"java/lang/NullPointerException","Class.getDeclaredField receiver");
  if(!a[0].l)return fail(v,"java/lang/NullPointerException","field name");
  if(!dvm__obj(v,gc_ref_name_47)||dvm__obj(v,gc_ref_name_47)->kind!=DVM_OBJ_STRING)return fail(v,"java/lang/IllegalArgumentException","field name");
  struct dvm_class *k=dvm__obj(v,gc_ref_co_46)->klass;out->l=0;
  if(k->external&&!k->nifields&&!k->nsfields)return fail(v,"java/lang/UnsupportedOperationException","Class.getDeclaredField external class");
  if(!k->is_array&&!k->is_primitive)for(int pass=0;pass<2;pass++){
   struct dvm_field *fields=pass?k->sfields:k->ifields;int count=pass?k->nsfields:k->nifields;
   if(count<0||(count&&!fields))return fail(v,"java/lang/InternalError","field metadata");
   for(int i=0;i<count;i++){
    struct dvm_field *f=&fields[i];
    if(!f->name||!f->type||!f->cls)return fail(v,"java/lang/InternalError","field metadata");
    if(strlen(f->name)!=dvm__obj(v,gc_ref_name_47)->utf8_len||memcmp(f->name,dvm__obj(v,gc_ref_name_47)->utf8,dvm__obj(v,gc_ref_name_47)->utf8_len))continue;
    reflected *r=calloc(1,sizeof *r);if(!r)return fail(v,"java/lang/OutOfMemoryError","Field");
    r->owner=k;r->kind=4;r->field=f;r->statics=pass!=0;
    out->l=dvm_new_object(v,dvm_find_class_or_external(v,"java/lang/reflect/Field"));
    const dvm_ref gc_ref_fo_48=(out->l); if(!gc_operation_admit(v,gc_ref_fo_48))return true;
    if(!dvm__obj(v,gc_ref_fo_48)){free(r);return fail(v,"java/lang/OutOfMemoryError","Field");}
    dvm__obj(v,gc_ref_fo_48)->data=r;dvm__obj(v,gc_ref_fo_48)->data_destroy=reflected_free;return true;
   }
  }
  return fail(v,"java/lang/NoSuchFieldException","field");
 }
 /* Class.getDeclaredFields(): every field declared by this class, instance
  * fields first and then static fields, both in dex declaration order.
  * Java calls the same method on arrays and primitive classes and gets an
  * empty array; that is a real answer, not a missing capability.  An
  * external class is a host stub whose members this VM does not know, so
  * returning an empty array would silently claim it has no fields; fail
  * closed instead.  Each Field keeps the dvm_field pointer and owner for
  * later getName/getType/get/getModifiers calls. */
 if(!strcmp(c,"java/lang/Class")&&!strcmp(m,"getDeclaredFields")&&!strcmp(s,"()[Ljava/lang/reflect/Field;")&&n==0) {
  const dvm_ref gc_ref_co_49=(self); if(!gc_operation_admit(v,gc_ref_co_49))return true;
  if(!dvm__obj(v,gc_ref_co_49)||dvm__obj(v,gc_ref_co_49)->kind!=DVM_OBJ_CLASS||!dvm__obj(v,gc_ref_co_49)->klass)return fail(v,"java/lang/NullPointerException","Class.getDeclaredFields");
  struct dvm_class *klass=dvm__obj(v,gc_ref_co_49)->klass;
  bool declared=!klass->is_array&&!klass->is_primitive;
  uint32_t count=0;
  if(declared) {
   /* A host-registered class can carry real field metadata (android.os.Build
    * and Build$VERSION publish genuine static fields), so it enumerates them
    * like any dex class.  Only an external class with no member metadata
    * falls through to the fail-closed / view-class handling below. */
   bool host_members=klass->external&&
       ((klass->nifields>0&&klass->ifields)||(klass->nsfields>0&&klass->sfields));
   if(klass->external&&!host_members){
    if(!external_without_metadata(klass)){
     static unsigned ext_fields_probe;
     if(ext_fields_probe<8){ext_fields_probe++;FILE*f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt","a");
      if(f){fprintf(f,"GET_DECLARED_FIELDS_EXTERNAL class=%s\n",klass->name?klass->name:"?");fclose(f);}}
     dvm__throw(v,"java/lang/UnsupportedOperationException","Class.getDeclaredFields external class: %s",klass->name?klass->name:"?");return true;
    }
   } else {
    if(klass->nifields<0||klass->nsfields<0)return fail(v,"java/lang/InternalError","field count");
    count=(uint32_t)klass->nifields+(uint32_t)klass->nsfields;
   }
  }
  struct dvm_class *field_class=dvm_find_class_or_external(v,"java/lang/reflect/Field");
  if(!field_class)return fail(v,"java/lang/InternalError","Field class");
  dvm_ref fields=dvm_new_array(v,'L',"Ljava/lang/reflect/Field;",count);
  if(!fields)return fail(v,"java/lang/OutOfMemoryError","Class.getDeclaredFields array");
  dvm_ref *items=({const dvm_ref gc_payload_ref_14=(fields);if(!gc_operation_admit(v,gc_payload_ref_14))return true;dvm_array_data(v,gc_payload_ref_14);});
  uint32_t at=0;
  if(declared)for(int pass=0;pass<2;pass++){
   struct dvm_field *list=pass?klass->sfields:klass->ifields;
   int total=pass?klass->nsfields:klass->nifields;
   if(total>0&&!list)return fail(v,"java/lang/InternalError","field metadata");
   for(int i=0;i<total;i++,at++){
    struct dvm_field *f=&list[i];
    if(!f->cls||!f->name||!f->type)return fail(v,"java/lang/InternalError","field metadata");
    reflected *r=calloc(1,sizeof *r);
    if(!r)return fail(v,"java/lang/OutOfMemoryError","Field");
    r->owner=f->cls;r->kind=4;r->field=f;r->statics=pass!=0;
    dvm_ref ref=dvm_new_object(v,field_class);
    const dvm_ref gc_ref_fo_50=(ref); if(!gc_operation_admit(v,gc_ref_fo_50))return true;
    if(!dvm__obj(v,gc_ref_fo_50)){free(r);return fail(v,"java/lang/OutOfMemoryError","Field");}
    dvm__obj(v,gc_ref_fo_50)->data=r;dvm__obj(v,gc_ref_fo_50)->data_destroy=reflected_free;
    items[at]=ref;
   }
  }
  out->l=fields;return true;
 }
 /* Class.getDeclaredMethods(): only the methods this class declares, in dex
  * order, excluding <init>/<clinit>.  Java returns an empty array for arrays
  * and primitive classes; an external class fails closed unless it is one of
  * the view classes view_state_runtime implements (those truthfully declare
  * zero members here).  Each Method keeps the dvm_method pointer, which is
  * owned by the class table and lives until dvm_destroy(). */
 if(!strcmp(c,"java/lang/Class")&&!strcmp(m,"getDeclaredMethods")&&!strcmp(s,"()[Ljava/lang/reflect/Method;")&&n==0) {
  const dvm_ref gc_ref_co_51=(self); if(!gc_operation_admit(v,gc_ref_co_51))return true;
  if(!dvm__obj(v,gc_ref_co_51)||dvm__obj(v,gc_ref_co_51)->kind!=DVM_OBJ_CLASS||!dvm__obj(v,gc_ref_co_51)->klass)return fail(v,"java/lang/NullPointerException","Class.getDeclaredMethods");
  struct dvm_class *klass=dvm__obj(v,gc_ref_co_51)->klass;
  bool declared=!klass->is_array&&!klass->is_primitive;
  uint32_t count=0;
  if(declared) {
   /* 宿主注册的方法表（activity_runtime 给 Activity/Context 安装）是真实
    * 方法清单，必须像 DEX 类一样枚举；只有没有方法元数据的 external 类
    * 才走 fail-closed / 白名单。 */
   bool host_methods=klass->external&&klass->nmethods>0&&klass->methods;
   if(klass->external&&!host_methods){
    if(!external_without_metadata(klass)){
     static unsigned ext_methods_probe;
     if(ext_methods_probe<64){ext_methods_probe++;FILE*f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/jni-gaps-external.txt","a");
      if(f){fprintf(f,"pid=%d GET_DECLARED_METHODS_EXTERNAL class=%s\n",getpid(),klass->name?klass->name:"?");fclose(f);}}
     dvm__throw(v,"java/lang/UnsupportedOperationException","Class.getDeclaredMethods external class: %s",klass->name?klass->name:"?");return true;
    }
   } else {
    if(klass->nmethods<0||(klass->nmethods>0&&!klass->methods))return fail(v,"java/lang/InternalError","method metadata");
    for(int i=0;i<klass->nmethods;i++){
     struct dvm_method *m=&klass->methods[i];
     if(!m->cls||!m->name||!m->sig)return fail(v,"java/lang/InternalError","method metadata");
     if(m->name[0]!='<')++count;
    }
   }
  }
  struct dvm_class *method_class=dvm_find_class_or_external(v,"java/lang/reflect/Method");
  if(!method_class)return fail(v,"java/lang/InternalError","Method class");
  dvm_ref methods=dvm_new_array(v,'L',"Ljava/lang/reflect/Method;",count);
  if(!methods)return fail(v,"java/lang/OutOfMemoryError","Class.getDeclaredMethods array");
  dvm_ref *items=({const dvm_ref gc_payload_ref_15=(methods);if(!gc_operation_admit(v,gc_payload_ref_15))return true;dvm_array_data(v,gc_payload_ref_15);});
  uint32_t at=0;
  if(declared&&klass->nmethods>0&&klass->methods)for(int i=0;i<klass->nmethods;i++){
   struct dvm_method *m=&klass->methods[i];
   if(m->name[0]=='<')continue;
   reflected *r=calloc(1,sizeof *r);
   if(!r)return fail(v,"java/lang/OutOfMemoryError","Method");
   r->owner=m->cls;r->kind=5;r->method=m;
   dvm_ref ref=dvm_new_object(v,method_class);
   const dvm_ref gc_ref_mo_52=(ref); if(!gc_operation_admit(v,gc_ref_mo_52))return true;
   if(!dvm__obj(v,gc_ref_mo_52)){free(r);return fail(v,"java/lang/OutOfMemoryError","Method");}
   dvm__obj(v,gc_ref_mo_52)->data=r;dvm__obj(v,gc_ref_mo_52)->data_destroy=reflected_free;items[at++]=ref;
  }
  out->l=methods;return true;
 }
 if(!strcmp(c,"java/lang/reflect/Field")&&!strcmp(m,"getName")&&!strcmp(s,"()Ljava/lang/String;")&&n==0) {
  reflected *r;if(!field_reflected(v,self,&r))return fail(v,"java/lang/IllegalArgumentException","Field.getName receiver");
  out->l=metadata_name(v,r->field->name);
  if(!out->l)return fail(v,"java/lang/OutOfMemoryError","Field.getName");
  return true;
 }
 if(!strcmp(c,"java/lang/reflect/Field")&&!strcmp(m,"getDeclaringClass")&&!strcmp(s,"()Ljava/lang/Class;")&&n==0) {
  reflected *r;if(!field_reflected(v,self,&r)||!r->owner)return fail(v,"java/lang/IllegalArgumentException","Field.getDeclaringClass receiver");
  out->l=dvm_class_object(v,r->owner);
  if(!out->l)return fail(v,"java/lang/OutOfMemoryError","Field.getDeclaringClass");
  return true;
 }
 if(!strcmp(c,"java/lang/reflect/Field")&&!strcmp(m,"getType")&&!strcmp(s,"()Ljava/lang/Class;")&&n==0) {
  reflected *r;if(!field_reflected(v,self,&r))return fail(v,"java/lang/IllegalArgumentException","Field.getType receiver");
  struct dvm_class *type=dvm__class_by_desc(v,r->field->type);
  if(!type){dvm__throw(v,"java/lang/NoClassDefFoundError","%s",r->field->type);return true;}
  out->l=dvm_class_object(v,type);
  if(!out->l)return fail(v,"java/lang/OutOfMemoryError","Field.getType");
  return true;
 }
 if(!strcmp(c,"java/lang/reflect/Field")&&!strcmp(m,"get")&&!strcmp(s,"(Ljava/lang/Object;)Ljava/lang/Object;")&&n==1)
  return field_get(v,self,a[0].l,out);
 if(!strcmp(c,"java/lang/reflect/Field")&&!strcmp(m,"set")&&!strcmp(s,"(Ljava/lang/Object;Ljava/lang/Object;)V")&&n==2)return field_set(v,self,a[0].l,a[1].l);
 if(!strcmp(c,"java/lang/reflect/Method")&&n==0){
  bool declaring=!strcmp(m,"getDeclaringClass")&&!strcmp(s,"()Ljava/lang/Class;");
  bool modifiers=!strcmp(m,"getModifiers")&&!strcmp(s,"()I");
  if(declaring||modifiers){
   reflected *r;
   if(!any_method_reflected(v,self,&r)||!r->owner)
    return fail(v,"java/lang/IllegalArgumentException","Method metadata receiver");
   if(declaring){
    out->l=dvm_class_object(v,r->owner);
    if(!out->l)return fail(v,"java/lang/OutOfMemoryError","Method declaring class");
   }else{
    out->i=r->method?(int32_t)r->method->access:DEX_ACC_PUBLIC;
   }
   return true;
  }
 }
 if(!strcmp(c,"java/lang/reflect/Field")&&!strcmp(m,"getGenericType")&&!strcmp(s,"()Ljava/lang/reflect/Type;")&&!n){
  reflected *r;if(!field_reflected(v,self,&r))return fail(v,"java/lang/IllegalArgumentException","Field.getGenericType receiver");
  char signature[4096];
  if(r->owner->dex&&dex_field_signature(r->owner->dex,r->owner->class_def_idx,r->field->name,signature,sizeof signature)){const char *cursor=signature;out->l=generic_parse(v,&cursor,0,r->owner);return true;}
  struct dvm_class *type=dvm__class_by_desc(v,r->field->type);out->l=type?dvm_class_object(v,type):0;return true;
 }
 if(!strcmp(c,"java/lang/reflect/Field")&&!strcmp(m,"isSynthetic")&&!strcmp(s,"()Z")&&!n){
  reflected *r;if(!field_reflected(v,self,&r))return fail(v,"java/lang/IllegalArgumentException","Field.isSynthetic receiver");out->i=(r->field->access&0x1000)!=0;return true;
 }
 if(!strcmp(c,"java/lang/reflect/Field")&&!strcmp(m,"getModifiers")&&!strcmp(s,"()I")&&n==0) {
  reflected *r;if(!field_reflected(v,self,&r))return fail(v,"java/lang/IllegalArgumentException","Field.getModifiers receiver");
  out->i=(int)r->field->access;return true;
 }
 if(!strcmp(c,"java/lang/reflect/Field")&&!strcmp(m,"setAccessible")&&!strcmp(s,"(Z)V")&&n==1) {
  reflected *r;if(!field_reflected(v,self,&r))return fail(v,"java/lang/IllegalArgumentException","Field.setAccessible receiver");
  r->accessible=a[0].i!=0;return true;
 }
 /* Field annotations: only runtime-visible annotations declared on this exact
  * field are reported.  isAnnotationPresent returns false and getAnnotation
  * returns null when the dex declares no matching annotation or the supplied
  * Class is a different annotation type; neither path fabricates an
  * annotation object. */
 if(!strcmp(c,"java/lang/reflect/Field")&&!strcmp(m,"isAnnotationPresent")&&!strcmp(s,"(Ljava/lang/Class;)Z")&&n==1) {
  reflected *r;if(!field_reflected(v,self,&r))return fail(v,"java/lang/IllegalArgumentException","Field.isAnnotationPresent receiver");
  struct dvm_class *type;
  if(!annotation_class(v,a[0].l,&type))return true;
  uint32_t off=0;
  out->i=field_annotation(r->field,type->desc,&off)?1:0;return true;
 }
 if(!strcmp(c,"java/lang/reflect/Field")&&!strcmp(m,"getAnnotation")&&!strcmp(s,"(Ljava/lang/Class;)Ljava/lang/annotation/Annotation;")&&n==1) {
  reflected *r;if(!field_reflected(v,self,&r))return fail(v,"java/lang/IllegalArgumentException","Field.getAnnotation receiver");
  struct dvm_class *type;
  if(!annotation_class(v,a[0].l,&type))return true;
  uint32_t off=0;
  if(!field_annotation(r->field,type->desc,&off)){out->l=0;return true;}
  return new_annotation(v,type,r->field->cls->dex,off,out);
 }
 /* Class.isAssignableFrom(): A.isAssignableFrom(B) asks whether a B may be
  * used where an A is required, i.e. whether B is a subtype of A.
  * dvm__class_assignable(from=B, to=A) already implements the superclass
  * chain, recursive interface inheritance (direct interfaces are followed
  * through their own ifaces), array covariance, Object covering every
  * reference/array type, and primitive classes matching only themselves.
  * Unknown hierarchies answer false there rather than an unsafe true, so the
  * result stays fail-closed.  Null and non-Class operands cannot occur in
  * verified bytecode; they are rejected here instead of being treated as
  * assignable because reaching a native hook through reflection can still
  * produce them. */
 /* Class reflection surface used by Unity's ReflectionHelper.  Every answer
  * is read from the DEX class record (name/desc/super/access/elem) or from
  * the VM's own assignability relation; only the fixed Java contract values
  * for arrays ("public final") and primitives ("public abstract final"
  * modifiers, primitive display names) are synthesized. */
 if(!strcmp(c,"java/lang/Class")&&!strcmp(m,"asSubclass")&&
    !strcmp(s,"(Ljava/lang/Class;)Ljava/lang/Class;")&&n==1) {
  if(!self)return fail(v,"java/lang/NullPointerException","Class.asSubclass receiver");
  const dvm_ref gc_ref_co_53=(self); if(!gc_operation_admit(v,gc_ref_co_53))return true;
  if(!dvm__obj(v,gc_ref_co_53)||dvm__obj(v,gc_ref_co_53)->kind!=DVM_OBJ_CLASS||!dvm__obj(v,gc_ref_co_53)->klass)return fail(v,"java/lang/IllegalArgumentException","Class.asSubclass receiver");
  if(!a[0].l)return fail(v,"java/lang/NullPointerException","Class.asSubclass argument");
  const dvm_ref gc_ref_ao_54=(a[0].l); if(!gc_operation_admit(v,gc_ref_ao_54))return true;
  if(!dvm__obj(v,gc_ref_ao_54)||dvm__obj(v,gc_ref_ao_54)->kind!=DVM_OBJ_CLASS||!dvm__obj(v,gc_ref_ao_54)->klass)return fail(v,"java/lang/IllegalArgumentException","Class.asSubclass argument");
  if(!dvm__class_assignable(v,dvm__obj(v,gc_ref_co_53)->klass,dvm__obj(v,gc_ref_ao_54)->klass))return fail(v,"java/lang/ClassCastException","Class.asSubclass");
  out->l=self;return true;
 }
 if(!strcmp(c,"java/lang/Class")&&!strcmp(m,"getName")&&!strcmp(s,"()Ljava/lang/String;")&&n==0) {
  const dvm_ref gc_ref_co_55=(self); if(!gc_operation_admit(v,gc_ref_co_55))return true;
  if(!dvm__obj(v,gc_ref_co_55)||dvm__obj(v,gc_ref_co_55)->kind!=DVM_OBJ_CLASS||!dvm__obj(v,gc_ref_co_55)->klass)return fail(v,"java/lang/NullPointerException","Class.getName receiver");
  struct dvm_class *k=dvm__obj(v,gc_ref_co_55)->klass;
  static const struct { const char *desc; const char *name; } primitive_names[]={
   {"Z","boolean"},{"B","byte"},{"C","char"},{"S","short"},
   {"I","int"},{"J","long"},{"F","float"},{"D","double"},{"V","void"}};
  if(k->is_primitive&&k->desc)
   for(size_t i=0;i<sizeof primitive_names/sizeof primitive_names[0];++i)
    if(!strcmp(k->desc,primitive_names[i].desc)){out->l=metadata_name(v,primitive_names[i].name);return true;}
  const char *raw=(k->is_array&&k->desc)?k->desc:(k->name?k->name:"");
  size_t raw_length=strlen(raw);
  char *text=malloc(raw_length+1);
  if(!text){dvm__throw(v,"java/lang/OutOfMemoryError","Class.getName");return true;}
  for(size_t i=0;i<raw_length;++i)text[i]=raw[i]=='/'?'.':raw[i];
  text[raw_length]='\0';
  out->l=metadata_name(v,text);free(text);return true;
 }
 if(!strcmp(c,"java/lang/Class")&&!strcmp(m,"getInterfaces")&&!strcmp(s,"()[Ljava/lang/Class;")&&n==0) {
  const dvm_ref gc_ref_co_56=(self); if(!gc_operation_admit(v,gc_ref_co_56))return true;
  if(!dvm__obj(v,gc_ref_co_56)||dvm__obj(v,gc_ref_co_56)->kind!=DVM_OBJ_CLASS||!dvm__obj(v,gc_ref_co_56)->klass)return fail(v,"java/lang/NullPointerException","Class.getInterfaces receiver");
  struct dvm_class *k=dvm__obj(v,gc_ref_co_56)->klass;uint32_t count=k->is_primitive?0:k->is_array?2:(uint32_t)k->nifaces;
  out->l=dvm_new_array(v,'L',"Ljava/lang/Class;",count);if(!out->l)return fail(v,"java/lang/OutOfMemoryError","Class interfaces");
  for(uint32_t i=0;i<count;i++){struct dvm_class *iface=k->is_array?dvm_find_class_or_external(v,i?"java/io/Serializable":"java/lang/Cloneable"):k->ifaces[i];((dvm_ref*)({const dvm_ref gc_payload_ref_16=(out->l);if(!gc_operation_admit(v,gc_payload_ref_16))return true;dvm_array_data(v,gc_payload_ref_16);}))[i]=dvm_class_object(v,iface);if(dvm_exception(v))return true;}
  return true;
 }
 if(!strcmp(c,"java/lang/Class")&&!strcmp(m,"getEnclosingClass")&&!strcmp(s,"()Ljava/lang/Class;")&&n==0) {
  const dvm_ref gc_ref_co_57=(self); if(!gc_operation_admit(v,gc_ref_co_57))return true;
  if(!dvm__obj(v,gc_ref_co_57)||dvm__obj(v,gc_ref_co_57)->kind!=DVM_OBJ_CLASS||!dvm__obj(v,gc_ref_co_57)->klass)return fail(v,"java/lang/NullPointerException","Class.getEnclosingClass receiver");
  struct dvm_class *k=dvm__obj(v,gc_ref_co_57)->klass;out->l=0;
  /* Both member and local/anonymous classes carry real enclosing metadata.
   * A '$' in a binary name alone does not establish an enclosing class. */
  const char *desc=(!k->is_array&&!k->is_primitive&&k->dex)?dex_class_enclosing_type(k->dex,k->class_def_idx):NULL;
  if(desc){struct dvm_class *owner=dvm__class_by_desc(v,desc);if(owner)out->l=dvm_class_object(v,owner);}
  return true;
 }
 if(!strcmp(c,"java/lang/Class")&&!strcmp(m,"getSuperclass")&&!strcmp(s,"()Ljava/lang/Class;")&&n==0) {
  const dvm_ref gc_ref_co_58=(self); if(!gc_operation_admit(v,gc_ref_co_58))return true;
  if(!dvm__obj(v,gc_ref_co_58)||dvm__obj(v,gc_ref_co_58)->kind!=DVM_OBJ_CLASS||!dvm__obj(v,gc_ref_co_58)->klass)return fail(v,"java/lang/NullPointerException","Class.getSuperclass receiver");
  struct dvm_class *k=dvm__obj(v,gc_ref_co_58)->klass;
  if(k->is_primitive||k->is_interface){out->l=0;return true;}
  /* Java reports Object as the superclass of every array class. */
  struct dvm_class *parent=k->is_array?dvm__class_by_desc(v,"Ljava/lang/Object;"):k->super;
  out->l=parent?dvm_class_object(v,parent):0;return true;
 }
 if(!strcmp(c,"java/lang/Class")&&!strcmp(m,"isLocalClass")&&!strcmp(s,"()Z")&&n==0) {
  const dvm_ref gc_ref_co_59=(self); if(!gc_operation_admit(v,gc_ref_co_59))return true;
  if(!dvm__obj(v,gc_ref_co_59)||dvm__obj(v,gc_ref_co_59)->kind!=DVM_OBJ_CLASS||!dvm__obj(v,gc_ref_co_59)->klass)return fail(v,"java/lang/NullPointerException","Class.isLocalClass receiver");
  struct dvm_class *k=dvm__obj(v,gc_ref_co_59)->klass;const char *inner_name=NULL;
  /* Local classes are named, non-member enclosed classes; this includes
   * classes in initializer blocks where EnclosingMethod.method is null. */
  out->i=!k->is_array&&!k->is_primitive&&k->dex&&dex_class_enclosing_type(k->dex,k->class_def_idx)&&!dex_class_is_member(k->dex,k->class_def_idx)&&dex_class_inner_name(k->dex,k->class_def_idx,&inner_name)&&inner_name!=NULL;return true;
 }
 if(!strcmp(c,"java/lang/Class")&&!strcmp(m,"isMemberClass")&&!strcmp(s,"()Z")&&!n){
  const dvm_ref gc_ref_co_60=(self); if(!gc_operation_admit(v,gc_ref_co_60))return true;
  if(!dvm__obj(v,gc_ref_co_60)||dvm__obj(v,gc_ref_co_60)->kind!=DVM_OBJ_CLASS||!dvm__obj(v,gc_ref_co_60)->klass)return fail(v,"java/lang/NullPointerException","Class.isMemberClass receiver");
  struct dvm_class *k=dvm__obj(v,gc_ref_co_60)->klass;
  out->i=!k->is_array&&!k->is_primitive&&k->dex&&dex_class_is_member(k->dex,k->class_def_idx);return true;
 }
 if(!strcmp(c,"java/lang/Class")&&!strcmp(m,"isAnonymousClass")&&!strcmp(s,"()Z")&&n==0) {
  const dvm_ref gc_ref_co_61=(self); if(!gc_operation_admit(v,gc_ref_co_61))return true;
  if(!dvm__obj(v,gc_ref_co_61)||dvm__obj(v,gc_ref_co_61)->kind!=DVM_OBJ_CLASS||!dvm__obj(v,gc_ref_co_61)->klass)return fail(v,"java/lang/NullPointerException","Class.isAnonymousClass receiver");
  struct dvm_class *k=dvm__obj(v,gc_ref_co_61)->klass;const char *inner_name=NULL;
  out->i=!k->is_array&&!k->is_primitive&&k->dex&&dex_class_inner_name(k->dex,k->class_def_idx,&inner_name)&&inner_name==NULL;return true;
 }
 if(!strcmp(c,"java/lang/Class")&&!strcmp(m,"isInterface")&&!strcmp(s,"()Z")&&n==0) {
  const dvm_ref gc_ref_co_62=(self); if(!gc_operation_admit(v,gc_ref_co_62))return true;
  if(!dvm__obj(v,gc_ref_co_62)||dvm__obj(v,gc_ref_co_62)->kind!=DVM_OBJ_CLASS||!dvm__obj(v,gc_ref_co_62)->klass)return fail(v,"java/lang/NullPointerException","Class.isInterface receiver");
  out->i=(!dvm__obj(v,gc_ref_co_62)->klass->is_array&&dvm__obj(v,gc_ref_co_62)->klass->is_interface)?1:0;return true;
 }
 if(!strcmp(c,"java/lang/Class")&&!strcmp(m,"isInstance")&&!strcmp(s,"(Ljava/lang/Object;)Z")&&n==1) {
  const dvm_ref gc_ref_co_63=(self); if(!gc_operation_admit(v,gc_ref_co_63))return true;
  if(!dvm__obj(v,gc_ref_co_63)||dvm__obj(v,gc_ref_co_63)->kind!=DVM_OBJ_CLASS||!dvm__obj(v,gc_ref_co_63)->klass)return fail(v,"java/lang/NullPointerException","Class.isInstance receiver");
  if(!a[0].l){out->i=0;return true;}
  struct dvm_class *from=dvm_object_class(v,a[0].l);
  if(!from)return fail(v,"java/lang/IllegalArgumentException","Class.isInstance argument");
  out->i=dvm__class_assignable(v,from,dvm__obj(v,gc_ref_co_63)->klass);return true;
 }
 if(!strcmp(c,"java/lang/Class")&&!strcmp(m,"getComponentType")&&!strcmp(s,"()Ljava/lang/Class;")&&n==0) {
  const dvm_ref gc_ref_co_64=(self); if(!gc_operation_admit(v,gc_ref_co_64))return true;
  if(!dvm__obj(v,gc_ref_co_64)||dvm__obj(v,gc_ref_co_64)->kind!=DVM_OBJ_CLASS||!dvm__obj(v,gc_ref_co_64)->klass)return fail(v,"java/lang/NullPointerException","Class.getComponentType receiver");
  struct dvm_class *elem=dvm__obj(v,gc_ref_co_64)->klass->is_array?dvm__obj(v,gc_ref_co_64)->klass->elem:NULL;
  out->l=elem?dvm_class_object(v,elem):0;return true;
 }
 if(!strcmp(c,"java/lang/Class")&&!strcmp(m,"getModifiers")&&!strcmp(s,"()I")&&n==0) {
  const dvm_ref gc_ref_co_65=(self); if(!gc_operation_admit(v,gc_ref_co_65))return true;
  if(!dvm__obj(v,gc_ref_co_65)||dvm__obj(v,gc_ref_co_65)->kind!=DVM_OBJ_CLASS||!dvm__obj(v,gc_ref_co_65)->klass)return fail(v,"java/lang/NullPointerException","Class.getModifiers receiver");
  struct dvm_class *k=dvm__obj(v,gc_ref_co_65)->klass;
  if(k->is_array)out->i=0x11;            /* public final */
  else if(k->is_primitive)out->i=0x411;  /* public abstract final */
  else {uint32_t flags=k->access;if(k->dex) (void)dex_class_inner_flags(k->dex,k->class_def_idx,&flags);out->i=flags;}
  return true;
 }
 if(!strcmp(c,"java/lang/Class")&&!strcmp(m,"cast")&&!strcmp(s,"(Ljava/lang/Object;)Ljava/lang/Object;")&&n==1) {
  const dvm_ref gc_ref_co_66=(self); if(!gc_operation_admit(v,gc_ref_co_66))return true;
  if(!dvm__obj(v,gc_ref_co_66)||dvm__obj(v,gc_ref_co_66)->kind!=DVM_OBJ_CLASS||!dvm__obj(v,gc_ref_co_66)->klass)return fail(v,"java/lang/NullPointerException","Class.cast receiver");
  if(!a[0].l){out->l=0;return true;}
  struct dvm_class *from=dvm_object_class(v,a[0].l);
  if(!from)return fail(v,"java/lang/IllegalArgumentException","Class.cast argument");
  if(!dvm__class_assignable(v,from,dvm__obj(v,gc_ref_co_66)->klass))return fail(v,"java/lang/ClassCastException","Class.cast");
  out->l=a[0].l;return true;
 }
 if(!strcmp(c,"java/lang/Class")&&!strcmp(m,"isAssignableFrom")&&!strcmp(s,"(Ljava/lang/Class;)Z")&&n==1) {
  if(!self)return fail(v,"java/lang/NullPointerException","Class.isAssignableFrom receiver");
  const dvm_ref gc_ref_co_67=(self); if(!gc_operation_admit(v,gc_ref_co_67))return true;
  if(!dvm__obj(v,gc_ref_co_67)||dvm__obj(v,gc_ref_co_67)->kind!=DVM_OBJ_CLASS||!dvm__obj(v,gc_ref_co_67)->klass)return fail(v,"java/lang/IllegalArgumentException","Class.isAssignableFrom receiver");
  if(!a[0].l)return fail(v,"java/lang/NullPointerException","Class.isAssignableFrom argument");
  const dvm_ref gc_ref_ao_68=(a[0].l); if(!gc_operation_admit(v,gc_ref_ao_68))return true;
  if(!dvm__obj(v,gc_ref_ao_68)||dvm__obj(v,gc_ref_ao_68)->kind!=DVM_OBJ_CLASS||!dvm__obj(v,gc_ref_ao_68)->klass)return fail(v,"java/lang/IllegalArgumentException","Class.isAssignableFrom argument");
  out->i=dvm__class_assignable(v,dvm__obj(v,gc_ref_ao_68)->klass,dvm__obj(v,gc_ref_co_67)->klass);
  return true;
 }
 /* Class metadata predicates backed by the DEX record itself: primitive
  * kind, and the ENUM/ANNOTATION/SYNTHETIC access flags.  An array is not
  * primitive and carries no access flags, so its answers stay false like
  * java.lang.Class on Android. */
 if(!strcmp(c,"java/lang/Class")&&n==0&&!strcmp(s,"()Z")&&
    (!strcmp(m,"isPrimitive")||!strcmp(m,"isEnum")||!strcmp(m,"isAnnotation")||!strcmp(m,"isSynthetic"))) {
  if(!self)return fail(v,"java/lang/NullPointerException","Class predicate receiver");
  const dvm_ref gc_ref_co_69=(self); if(!gc_operation_admit(v,gc_ref_co_69))return true;
  if(!dvm__obj(v,gc_ref_co_69)||dvm__obj(v,gc_ref_co_69)->kind!=DVM_OBJ_CLASS||!dvm__obj(v,gc_ref_co_69)->klass)return fail(v,"java/lang/IllegalArgumentException","Class predicate receiver");
  struct dvm_class *k=dvm__obj(v,gc_ref_co_69)->klass;
  if(!strcmp(m,"isPrimitive"))out->i=k->is_primitive?1:0;
  else if(!strcmp(m,"isEnum"))out->i=(!k->is_array&&(k->access&DEX_ACC_ENUM))?1:0;
  else if(!strcmp(m,"isAnnotation"))out->i=(!k->is_array&&(k->access&DEX_ACC_ANNOTATION))?1:0;
  else out->i=(!k->is_array&&(k->access&DEX_ACC_SYNTHETIC))?1:0;
  return true;
 }
 /* Class.isArray() is true exactly for descriptors beginning with '['.
  * Primitive and ordinary reference classes are false; multi-dimensional
  * arrays are still arrays.  A class without a descriptor is malformed
  * metadata, not a boolean answer. */
 if(!strcmp(c,"java/lang/Class")&&!strcmp(m,"isArray")&&!strcmp(s,"()Z")&&n==0) {
  if(!self)return fail(v,"java/lang/NullPointerException","Class.isArray receiver");
  const dvm_ref gc_ref_co_70=(self); if(!gc_operation_admit(v,gc_ref_co_70))return true;
  if(!dvm__obj(v,gc_ref_co_70)||dvm__obj(v,gc_ref_co_70)->kind!=DVM_OBJ_CLASS||!dvm__obj(v,gc_ref_co_70)->klass)return fail(v,"java/lang/IllegalArgumentException","Class.isArray receiver");
  if(!dvm__obj(v,gc_ref_co_70)->klass->desc)return fail(v,"java/lang/InternalError","Class.isArray descriptor");
  out->i=dvm__obj(v,gc_ref_co_70)->klass->desc[0]=='[';
  return true;
 }
 if(!strcmp(c,"java/lang/reflect/Constructor")&&!strcmp(m,"getModifiers")&&!strcmp(s,"()I")&&!n){
  const dvm_ref gc_ref_co_71=(self); if(!gc_operation_admit(v,gc_ref_co_71))return true;reflected *r=dvm__obj(v,gc_ref_co_71)&&dvm__obj(v,gc_ref_co_71)->data_destroy==reflected_free?dvm__obj(v,gc_ref_co_71)->data:NULL;
  if(!r||r->kind!=3||!r->ctor)return fail(v,"java/lang/IllegalArgumentException","Constructor.getModifiers receiver");
  /* DEX's 0x10000 constructor marker is not a Java modifier bit. */
  out->i=(int32_t)(r->ctor->access&0xffffu);return true;
 }
 if(!strcmp(c,"java/lang/reflect/Constructor")&&!strcmp(m,"isAccessible")&&!strcmp(s,"()Z")&&!n){
  const dvm_ref gc_ref_object_72=(self); if(!gc_operation_admit(v,gc_ref_object_72))return true;reflected *r=dvm__obj(v,gc_ref_object_72)&&dvm__obj(v,gc_ref_object_72)->data_destroy==reflected_free?dvm__obj(v,gc_ref_object_72)->data:NULL;
  if(!r||r->kind!=3||!r->ctor)return fail(v,"java/lang/IllegalArgumentException","Constructor.isAccessible receiver");out->i=r->accessible;return true;
 }
 if(!strcmp(c,"java/lang/reflect/Constructor")&&!strcmp(m,"setAccessible")&&!strcmp(s,"(Z)V")&&n==1) {
  const dvm_ref gc_ref_object_73=(self); if(!gc_operation_admit(v,gc_ref_object_73))return true;
  reflected *r=dvm__obj(v,gc_ref_object_73)&&dvm__obj(v,gc_ref_object_73)->data_destroy==reflected_free?dvm__obj(v,gc_ref_object_73)->data:NULL;
  if(!r||r->kind!=3||!r->ctor)return fail(v,"java/lang/IllegalArgumentException","Constructor.setAccessible receiver");
  r->accessible=a[0].i!=0;
  return true;
 }
 if(!strcmp(c,"java/lang/reflect/Constructor")&&!strcmp(m,"newInstance")&&!strcmp(s,"([Ljava/lang/Object;)Ljava/lang/Object;")&&n==1) {
  const dvm_ref gc_ref_mo_74=(self); if(!gc_operation_admit(v,gc_ref_mo_74))return true;reflected *r=dvm__obj(v,gc_ref_mo_74)&&dvm__obj(v,gc_ref_mo_74)->data_destroy==reflected_free?dvm__obj(v,gc_ref_mo_74)->data:NULL;
  if(!r||r->kind!=3||!r->ctor||!r->owner)return fail(v,"java/lang/IllegalArgumentException","Constructor.newInstance receiver");
  if(!(r->ctor->access&DEX_ACC_PUBLIC)&&!r->accessible)return fail(v,"java/lang/IllegalAccessException","constructor is not public");
  if(r->owner->access&(DEX_ACC_ABSTRACT|DEX_ACC_INTERFACE))return fail(v,"java/lang/InstantiationException","abstract class");
  if(!is_object_array(v,a[0].l))return fail(v,"java/lang/IllegalArgumentException","Constructor.newInstance arguments");
  union dvm_value *args=NULL;int count=0;
  /* Argument conversion errors are IllegalArgumentException and are not
   * wrapped: InvocationTargetException is reserved for a throwable raised by
   * the constructor body itself. */
  if(!reflection_arguments(v,r->sig,a[0].l,&args,&count))return true;
  dvm_ref object=dvm_new_object(v,r->owner);
  if(!object){native_scratch_release(args);return fail(v,"java/lang/OutOfMemoryError","Constructor.newInstance");}
  union dvm_value result={0};bool ok=dvm_call(v,r->ctor,object,args,count,&result);native_scratch_release(args);
  if(!ok||v->exception){
   if(!dvm_exception(v))return fail(v,"java/lang/InstantiationException","constructor failed without exception");
   wrap_invocation_exception(v);return true;
  }
  out->l=object;return true;
 }
 if(!strcmp(c,"java/lang/Class")&&(!strcmp(m,"getMethod")||!strcmp(m,"getDeclaredMethod"))&&!strcmp(s,"(Ljava/lang/String;[Ljava/lang/Class;)Ljava/lang/reflect/Method;")&&n==2) {
  const dvm_ref gc_ref_co_75=(self); if(!gc_operation_admit(v,gc_ref_co_75))return true;
  if(!dvm__obj(v,gc_ref_co_75)||dvm__obj(v,gc_ref_co_75)->kind!=DVM_OBJ_CLASS||!dvm__obj(v,gc_ref_co_75)->klass||!a[0].l)return fail(v,"java/lang/NullPointerException","Class.getMethod");
  /* Java rejects a null parameter array before any lookup; an empty array is
   * the varargs form and is valid. */
  if(!a[1].l)return fail(v,"java/lang/NullPointerException","getMethod parameter types");
  if(!is_class_array(v,a[1].l))return fail(v,"java/lang/IllegalArgumentException","getMethod parameter types");
  const char *name=({const dvm_ref gc_payload_ref_17=(a[0].l);if(!gc_operation_admit(v,gc_payload_ref_17))return true;dvm_string_utf8(v,gc_payload_ref_17);});if(!name||memchr(name,0,dvm_string_utf8_length(v,a[0].l)))return fail(v,"java/lang/IllegalArgumentException","method name");
  bool declared=!strcmp(m,"getDeclaredMethod");uint32_t count=dvm_array_length(v,a[1].l);
  struct dvm_class *owner=dvm__obj(v,gc_ref_co_75)->klass;
  /* Argument validation is part of the Java contract and comes before the
   * special cases: a null element is NPE even when the receiver itself is a
   * host stub.  The same prefix is the exact match key below. */
  char *params=NULL;size_t plen=0;
  if(!method_parameter_prefix(v,a[1].l,&params,&plen))return true;
  /* The two special Method objects keep their exact invocation semantics:
   * SharedPreferences.Editor.apply (kind 1) and Throwable.addSuppressed
   * (kind 2).  They are checked before the generic metadata lookup because
   * their classes are host stubs with no member tables. */
  int kind=0;
  if(!declared&&!count&&!strcmp(owner->name,"android/content/SharedPreferences$Editor")&&!strcmp(name,"apply"))kind=1;
  if(declared&&count==1&&!strcmp(owner->name,"java/lang/Throwable")&&!strcmp(name,"addSuppressed")){
   dvm_ref item=((dvm_ref*)({const dvm_ref gc_payload_ref_18=(a[1].l);if(!gc_operation_admit(v,gc_payload_ref_18))return true;dvm_array_data(v,gc_payload_ref_18);}))[0];const dvm_ref gc_ref_type_76=(item); if(!gc_operation_admit(v,gc_ref_type_76))return true;
   if(dvm__obj(v,gc_ref_type_76)&&dvm__obj(v,gc_ref_type_76)->kind==DVM_OBJ_CLASS&&dvm__obj(v,gc_ref_type_76)->klass==owner)kind=2;
  }
  if(!kind&&((!strcmp(owner->name,"javax/net/ssl/SSLParameters")&&!strcmp(name,"setApplicationProtocols"))||(!strcmp(owner->name,"javax/net/ssl/SSLSocket")&&!strcmp(name,"getApplicationProtocol")))){free(params);return fail(v,"java/lang/NoSuchMethodException",name);}
  if(kind){free(params);return new_method_object(v,owner,kind,NULL,out);}
  /* An external class is a host stub whose members this VM does not know.
   * Returning null/NoSuchMethodException would turn "unknown" into a false
   * "absent", so fail closed.  The special objects above are the methods
   * this VM really implements on such classes. */
  if(owner->external&&!owner->nmethods){sdk_trace_fprintf(stderr,"REFLECT_GETMETHOD_MISS class=%s name=%s params=%s reason=external\n",owner->name,name,params);free(params);return fail(v,"java/lang/UnsupportedOperationException","Class.getMethod external class");}
  bool malformed=false;
  struct dvm_method *hit=declared
    ?match_declared_method(owner,name,params,plen,false,&malformed)
    :lookup_public_method(owner,name,params,plen,&malformed);
  if(malformed){free(params);return fail(v,"java/lang/InternalError","method metadata");}
  if(!hit){sdk_trace_fprintf(stderr,"REFLECT_GETMETHOD_MISS class=%s name=%s params=%s reason=nosuch\n",owner->name,name,params);free(params);return fail(v,"java/lang/NoSuchMethodException",name);}
  if(real_getmethod_log<12&&strncmp(owner->name,"sdktrace/",9)){
   sdk_trace_fprintf(stderr,"REFLECT_GETMETHOD_HIT class=%s name=%s params=%s declared=%d\n",owner->name,name,params,declared?1:0);
   ++real_getmethod_log;
  }
  free(params);
  return new_method_object(v,hit->cls,5,hit,out);
 }
 /* Method.setAccessible stores the flag rather than pretending the VM has a
  * security manager.  Method.invoke uses it to decide whether a non-public
  * dex method may be called; public methods need no flag. */
 if(!strcmp(c,"java/lang/reflect/Method")&&!strcmp(m,"setAccessible")&&!strcmp(s,"(Z)V")&&n==1) {
  const dvm_ref gc_ref_mo_77=(self); if(!gc_operation_admit(v,gc_ref_mo_77))return true;reflected *r=dvm__obj(v,gc_ref_mo_77)&&dvm__obj(v,gc_ref_mo_77)->data_destroy==reflected_free?dvm__obj(v,gc_ref_mo_77)->data:NULL;
  if(!r||(r->kind!=1&&r->kind!=2&&r->kind!=5))return fail(v,"java/lang/IllegalArgumentException","Method.setAccessible receiver");
  r->accessible=a[0].i!=0;return true;
 }
 if(!strcmp(c,"java/lang/reflect/Method")&&!strcmp(m,"getReturnType")&&!strcmp(s,"()Ljava/lang/Class;")&&n==0) {
  const dvm_ref gc_ref_mo_78=(self); if(!gc_operation_admit(v,gc_ref_mo_78))return true;reflected *r=dvm__obj(v,gc_ref_mo_78)&&dvm__obj(v,gc_ref_mo_78)->data_destroy==reflected_free?dvm__obj(v,gc_ref_mo_78)->data:NULL;
  if(!r||(r->kind!=1&&r->kind!=2&&r->kind!=5))return fail(v,"java/lang/IllegalArgumentException","Method.getReturnType receiver");
  const char *sig=r->kind==5?(r->method?r->method->sig:NULL):"()V";
  const char *ret=sig?strchr(sig,')'):NULL;
  if(!ret||!ret[1])return fail(v,"java/lang/InternalError","method signature");
  struct dvm_class *type=dvm__class_by_desc(v,ret+1);
  if(!type){dvm__throw(v,"java/lang/NoClassDefFoundError","%s",ret+1);return true;}
  out->l=dvm_class_object(v,type);
  if(!out->l)return fail(v,"java/lang/OutOfMemoryError","Method.getReturnType");
  return true;
 }
 if(!strcmp(c,"java/lang/reflect/Method")&&!strcmp(m,"getName")&&!strcmp(s,"()Ljava/lang/String;")&&n==0) {
  reflected *r;if(!any_method_reflected(v,self,&r))return fail(v,"java/lang/IllegalArgumentException","Method.getName receiver");
  const char *name=r->kind==1?"apply":r->kind==2?"addSuppressed":r->method?r->method->name:NULL;
  if(!name)return fail(v,"java/lang/InternalError","method metadata");
  out->l=metadata_name(v,name);
  if(!out->l)return fail(v,"java/lang/OutOfMemoryError","Method.getName");
  return true;
 }
 /* The parameter Class[] is built from the method's own dex signature, the
  * same descriptors getMethod() matched, so the elements are real Class
  * objects (including array and primitive classes) rather than invented
  * wrappers. */
 if((!strcmp(c,"java/lang/reflect/Method")||!strcmp(c,"java/lang/reflect/Constructor"))&&
    !strcmp(m,"getParameterTypes")&&!strcmp(s,"()[Ljava/lang/Class;")&&n==0) {
  reflected *r=NULL;
  if(!strcmp(c,"java/lang/reflect/Constructor")){
   const dvm_ref gc_ref_object_79=(self); if(!gc_operation_admit(v,gc_ref_object_79))return true;
   r=dvm__obj(v,gc_ref_object_79)&&dvm__obj(v,gc_ref_object_79)->data_destroy==reflected_free?dvm__obj(v,gc_ref_object_79)->data:NULL;
   if(!r||r->kind!=3||!r->ctor)return fail(v,"java/lang/IllegalArgumentException","Constructor.getParameterTypes receiver");
  }else if(!any_method_reflected(v,self,&r))return fail(v,"java/lang/IllegalArgumentException","Method.getParameterTypes receiver");
  const char *sig=r->kind==3?r->sig:r->kind==1?"()V":r->kind==2?"(Ljava/lang/Throwable;)V":r->method?r->method->sig:NULL;
  if(!sig||sig[0]!='(')return fail(v,"java/lang/InternalError","method signature");
  uint32_t count=0;const char *p=sig+1;
  while(*p&&*p!=')') {
   const char *end=field_desc_end(p);
   if(!end)return fail(v,"java/lang/InternalError","method signature");
   ++count;p=end;
  }
  if(*p!=')')return fail(v,"java/lang/InternalError","method signature");
  dvm_ref types=dvm_new_array(v,'L',"Ljava/lang/Class;",count);
  if(!types)return fail(v,"java/lang/OutOfMemoryError","Method.getParameterTypes array");
  dvm_ref *items=({const dvm_ref gc_payload_ref_19=(types);if(!gc_operation_admit(v,gc_payload_ref_19))return true;dvm_array_data(v,gc_payload_ref_19);});
  p=sig+1;uint32_t at=0;
  while(*p&&*p!=')') {
   const char *end=field_desc_end(p);
   struct dvm_class *type=parameter_class(v,p,(size_t)(end-p));
   if(!type){if(dvm_exception(v))return true;return fail(v,"java/lang/NoClassDefFoundError","parameter type");}
   dvm_ref ref=dvm_class_object(v,type);
   if(!ref)return fail(v,"java/lang/OutOfMemoryError","Method.getParameterTypes");
   items[at++]=ref;p=end;
  }
  out->l=types;return true;
 }
 /* Method annotations follow the same dex-backed rules as fields. */
 if(!strcmp(c,"java/lang/reflect/Method")&&!strcmp(m,"isAnnotationPresent")&&!strcmp(s,"(Ljava/lang/Class;)Z")&&n==1) {
  reflected *r;if(!method_reflected(v,self,&r))return fail(v,"java/lang/IllegalArgumentException","Method.isAnnotationPresent receiver");
  struct dvm_class *type;
  if(!annotation_class(v,a[0].l,&type))return true;
  uint32_t off=0;
  out->i=method_annotation(r->method,type->desc,&off)?1:0;return true;
 }
 if(!strcmp(c,"java/lang/reflect/Method")&&!strcmp(m,"getAnnotation")&&!strcmp(s,"(Ljava/lang/Class;)Ljava/lang/annotation/Annotation;")&&n==1) {
  reflected *r;if(!method_reflected(v,self,&r))return fail(v,"java/lang/IllegalArgumentException","Method.getAnnotation receiver");
  struct dvm_class *type;
  if(!annotation_class(v,a[0].l,&type))return true;
  uint32_t off=0;
  if(!method_annotation(r->method,type->desc,&off)){out->l=0;return true;}
  return new_annotation(v,type,r->method->dex,off,out);
 }
 if(!strcmp(c,"java/lang/reflect/Method")&&!strcmp(m,"invoke")&&!strcmp(s,"(Ljava/lang/Object;[Ljava/lang/Object;)Ljava/lang/Object;")&&n==2) {
  const dvm_ref gc_ref_mo_80=(self); if(!gc_operation_admit(v,gc_ref_mo_80))return true;reflected *r=dvm__obj(v,gc_ref_mo_80)&&dvm__obj(v,gc_ref_mo_80)->data_destroy==reflected_free?dvm__obj(v,gc_ref_mo_80)->data:NULL;
  if(!r)return fail(v,"java/lang/IllegalArgumentException","Method.invoke receiver");
  if(!is_object_array(v,a[1].l))return fail(v,"java/lang/IllegalArgumentException","Method.invoke arguments");
  uint32_t count=a[1].l?dvm_array_length(v,a[1].l):0;
  /* The two special Method objects created by getMethod/getDeclaredMethod
   * keep their original semantics: Editor.apply() takes no arguments and
   * Throwable.addSuppressed(Throwable) takes exactly one. */
  if(r->kind==1||r->kind==2) {
   if(count!=(r->kind==2?1u:0u))return fail(v,"java/lang/IllegalArgumentException","Method.invoke arguments");
   const dvm_ref gc_ref_target_81=(a[0].l); if(!gc_operation_admit(v,gc_ref_target_81))return true;
   if(!dvm__obj(v,gc_ref_target_81)||!dvm__class_assignable(v,dvm__obj(v,gc_ref_target_81)->cls,r->owner))return fail(v,"java/lang/IllegalArgumentException","Method.invoke target");
   if(r->kind==1) {union dvm_value ignored={0};if(!prefs_runtime_call(v,"android/content/SharedPreferences$Editor","apply","()V",a[0].l,NULL,0,&ignored))return fail(v,"java/lang/IllegalArgumentException","Method.invoke editor");out->l=0;return true;}
   dvm_ref value=((dvm_ref*)({const dvm_ref gc_payload_ref_20=(a[1].l);if(!gc_operation_admit(v,gc_payload_ref_20))return true;dvm_array_data(v,gc_payload_ref_20);}))[0];out->l=0;return add_suppressed(v,a[0].l,value);
  }
  reflected *mr;
  if(!method_reflected(v,self,&mr))return fail(v,"java/lang/IllegalArgumentException","Method.invoke receiver");
  struct dvm_method *mth=mr->method;
  bool is_static=(mth->access&DEX_ACC_STATIC)!=0;
  /* Java ignores the receiver for a static method, null included.  An
   * instance call still needs a receiver assignable to the declaring class
   * (interfaces included through dvm__class_assignable). */
  if(!is_static) {
   if(!a[0].l)return fail(v,"java/lang/NullPointerException","Method.invoke target");
   const dvm_ref gc_ref_target_82=(a[0].l); if(!gc_operation_admit(v,gc_ref_target_82))return true;
   if(!dvm__obj(v,gc_ref_target_82)||!dvm__obj(v,gc_ref_target_82)->cls||!dvm__class_assignable(v,dvm__obj(v,gc_ref_target_82)->cls,mr->owner))return fail(v,"java/lang/IllegalArgumentException","Method.invoke target");
  }
  if(!(mth->access&DEX_ACC_PUBLIC)&&!mr->accessible)return fail(v,"java/lang/IllegalAccessException","method is not public");
  union dvm_value *args=NULL;int argc=0;
  /* Convert Object[] arguments exactly like Constructor.newInstance: count,
   * reference assignability, unboxing and Java's widening matrix.  These
   * errors are IllegalArgumentException and are not wrapped. */
  if(!reflection_arguments(v,mth->sig,a[1].l,&args,&argc)){sdk_trace_fprintf(stderr,"REFLECT_INVOKE_INVALID class=%s name=%s\n",mr->owner->name,mth->name);return true;}
  union dvm_value result={0};
  bool ok=dvm_call(v,mth,is_static?0:a[0].l,args,argc,&result);native_scratch_release(args);
  if(!ok||v->exception) {
   sdk_trace_fprintf(stderr,"REFLECT_INVOKE_FAIL class=%s name=%s\n",mr->owner->name,mth->name);
   trace_invoke_cause(v,mr->owner,mth->name);
   if(!dvm_exception(v))return fail(v,"java/lang/InternalError","method failed without exception");
   wrap_invocation_exception(v);return true;
  }
  char kind=dvm_sig_return_kind(mth->sig);
  if(kind=='V'){out->l=0;return true;}
  if(kind=='L'){out->l=result.l;return true;}
  return box_kind(v,kind,result,out);
 }
 return false;
}
bool reflection_runtime_call(struct dvm *v,const char *c,const char *m,const char *s,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 struct gc_operation operation __attribute__((cleanup(gc_operation_end)));
 if(!gc_operation_begin(v,&operation))return true;
 bool result=reflection_runtime_call_operation(v,c,m,s,self,a,n,out);
 const char *return_type=s?strchr(s,')'):NULL;
 gc_operation_result(&operation,s,result&&!dvm_exception(v)&&out&&return_type&&(return_type[1]=='L'||return_type[1]=='[')?out->l:0);
 return result;
}

static int fixture_argument_count;
static union dvm_value fixture_argument[2];
static bool fixture_ctor(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 (void)v;(void)self;(void)out;
 fixture_argument_count=n;
 for(int i=0;i<n&&i<2;i++)fixture_argument[i]=a[i];
 return true;
}
static bool fixture_throw_ctor(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 (void)self;(void)a;(void)n;(void)out;
 dvm__throw(v,"java/lang/IllegalStateException","fixture constructor boom");
 return true;
}
static void install_fixture_ctor(struct dvm_class *c,int index,const char *sig,uint32_t access,dvm_builtin_fn fn){
 struct dvm_method *m=&c->methods[index];
 m->cls=c;m->name="<init>";m->sig=strdup(sig);m->access=access;m->ret_kind='V';m->builtin=fn;
}
static dvm_ref fixture_method_argument;
static bool fixture_parent_method(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 (void)self;(void)a;(void)n;
 out->l=dvm_new_string(v,"parent-value");
 return out->l!=0;
}
static bool fixture_child_method(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 (void)self;(void)a;(void)n;
 out->l=dvm_new_string(v,"child-value");
 return out->l!=0;
}
static bool fixture_setter_method(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 (void)v;(void)self;(void)out;
 fixture_method_argument=n>0?a[0].l:0;
 return true;
}
static void install_fixture_method(struct dvm_class *c,int index,const char *name,const char *sig,uint32_t access,dvm_builtin_fn fn,char ret_kind){
 struct dvm_method *m=&c->methods[index];
 m->cls=c;m->name=name;m->sig=strdup(sig);m->access=access;m->ret_kind=ret_kind;m->builtin=fn;
}
static dvm_ref fixture_types(struct dvm *v,const char *const *descs,int count){
 dvm_ref types=dvm_new_array(v,'L',"Ljava/lang/Class;",(uint32_t)count);
 if(!types)return 0;
 dvm_ref *items=({const dvm_ref gc_payload_ref_21=(types);if(!gc_operation_admit(v,gc_payload_ref_21))return 0;dvm_array_data(v,gc_payload_ref_21);});
 for(int i=0;i<count;i++){
  struct dvm_class *c=descs[i][0]=='L'||descs[i][0]=='['?dvm__class_by_desc(v,descs[i]):dvm__define_primitive(v,descs[i]);
  if(!c)return 0;
  items[i]=dvm_class_object(v,c);
  if(!items[i])return 0;
 }
 return types;
}
static dvm_ref fixture_constructor(struct dvm *v,dvm_ref klass,const char *const *descs,int count,int *present){
 if(present)*present=0;
 dvm_ref types=fixture_types(v,descs,count);if(!types)return 0;
 union dvm_value arg={.l=types},out={0};
 if(!reflection_runtime_call(v,"java/lang/Class","getConstructor","([Ljava/lang/Class;)Ljava/lang/reflect/Constructor;",klass,&arg,1,&out))return 0;
 if(dvm_exception(v)||!out.l)return 0;
 if(present)*present=1;
 return out.l;
}
static dvm_ref object_arguments(struct dvm *v,dvm_ref first,int count){
 dvm_ref values=dvm_new_array(v,'L',"Ljava/lang/Object;",(uint32_t)count);
 if(!values)return 0;
 dvm_ref *items=({const dvm_ref gc_payload_ref_22=(values);if(!gc_operation_admit(v,gc_payload_ref_22))return 0;dvm_array_data(v,gc_payload_ref_22);});
 for(int i=0;i<count;i++)items[i]=i?0:first;
 return values;
}
static bool construct(struct dvm *v,dvm_ref ctor,dvm_ref values,dvm_ref *result){
 if(result)*result=0;
 union dvm_value arg={.l=values},out={0};
 if(!reflection_runtime_call(v,"java/lang/reflect/Constructor","newInstance","([Ljava/lang/Object;)Ljava/lang/Object;",ctor,&arg,1,&out))return false;
 if(result&&!dvm_exception(v)&&out.l&&!dvm_exception(v))*result=out.l;
 return true;
}
static bool class_named(struct dvm *v,dvm_ref ref,const char *name){
 const dvm_ref gc_ref_o_83=(ref); if(!gc_operation_admit(v,gc_ref_o_83))return false;
 return dvm__obj(v,gc_ref_o_83)&&dvm__obj(v,gc_ref_o_83)->cls&&dvm__obj(v,gc_ref_o_83)->cls->name&&!strcmp(dvm__obj(v,gc_ref_o_83)->cls->name,name);
}
static bool exception_named(struct dvm *v,const char *name){return class_named(v,dvm_exception(v),name);}
static dvm_ref box_value(struct dvm *v,const char *cls,const char *sig,union dvm_value arg){
 struct dvm_class *c=dvm_find_class_or_external(v,cls);dvm_ref ref=c?dvm_new_object(v,c):0;
 if(!ref)return 0;
 union dvm_value out={0};
 bool small=!strcmp(cls,"java/lang/Byte")||!strcmp(cls,"java/lang/Short");
 bool ok=small?small_boxes_call(v,cls,"<init>",sig,ref,&arg,1,&out):java_basics_call(v,cls,"<init>",sig,ref,&arg,1,&out);
 return ok&&!dvm_exception(v)?ref:0;
}
static void fixture_field(struct dvm_field *f,struct dvm_class *owner,const char *name,const char *type,uint32_t access,uint16_t slot){
 f->cls=owner;f->name=name;f->type=type;f->access=access;
 f->kind=dvm__kind_of(type);f->width=1;f->slot=slot;
}
static dvm_ref declared_field_named(struct dvm *v,dvm_ref fields,const char *name){
 uint32_t count=dvm_array_length(v,fields);dvm_ref *items=({const dvm_ref gc_payload_ref_23=(fields);if(!gc_operation_admit(v,gc_payload_ref_23))return 0;dvm_array_data(v,gc_payload_ref_23);});
 if(!items)return 0;
 for(uint32_t i=0;i<count;i++){
  union dvm_value out={0};
  if(!reflection_runtime_call(v,"java/lang/reflect/Field","getName","()Ljava/lang/String;",items[i],NULL,0,&out))return 0;
  if(dvm_exception(v))return 0;
  const char *text=out.l?({const dvm_ref gc_payload_ref_24=(out.l);if(!gc_operation_admit(v,gc_payload_ref_24))return 0;dvm_string_utf8(v,gc_payload_ref_24);}):NULL;
  if(text&&!strcmp(text,name))return items[i];
 }
 return 0;
}
static bool field_type_is(struct dvm *v,dvm_ref field,struct dvm_class *want){
 union dvm_value out={0};
 if(!reflection_runtime_call(v,"java/lang/reflect/Field","getType","()Ljava/lang/Class;",field,NULL,0,&out))return false;
 if(dvm_exception(v)||!out.l)return false;
 const dvm_ref gc_ref_o_84=(out.l); if(!gc_operation_admit(v,gc_ref_o_84))return false;
 return dvm__obj(v,gc_ref_o_84)&&dvm__obj(v,gc_ref_o_84)->kind==DVM_OBJ_CLASS&&dvm__obj(v,gc_ref_o_84)->klass==want;
}
static bool field_modifiers_are(struct dvm *v,dvm_ref field,uint32_t want){
 union dvm_value out={0};
 if(!reflection_runtime_call(v,"java/lang/reflect/Field","getModifiers","()I",field,NULL,0,&out))return false;
 return !dvm_exception(v)&&out.i==(int)want;
}
static bool field_boxed_int(struct dvm *v,dvm_ref box,const char *cls,const char *method,const char *sig,int want){
 union dvm_value out={0};
 if(!class_named(v,box,cls))return false;
 if(!java_basics_call(v,cls,method,sig,box,NULL,0,&out)||dvm_exception(v))return false;
 return out.i==want;
}
static bool assignable_expect(struct dvm *v,dvm_ref self,dvm_ref other,bool expected){
 union dvm_value arg={.l=other},out={0};
 if(!reflection_runtime_call(v,"java/lang/Class","isAssignableFrom","(Ljava/lang/Class;)Z",self,&arg,1,&out))return false;
 if(dvm_exception(v)){dvm_clear_exception(v);return false;}
 return (out.i!=0)==expected;
}
/* A minimal, hand-assembled dex used only by this selftest.  The selftest VM
 * is created without an APK, so there is no application dex that could carry
 * a field/method annotation fixture.  This file has real string/type/proto/
 * field/method/class_def tables, a class_data_item, an annotations_directory
 * and runtime-visible (visibility=1) annotation_set entries:
 *   Lsdktrace/tests/RequestParam;  annotation interface with name()String;
 *   Lsdktrace/tests/DexAnnotated;  fields tagged (name="tag_value") and plain;
 *                                  methods value() (static, name="method_value")
 *                                  and plainValue(), each returning a constant.
 * dex_open_memory() validates the same header the app's dex passes through;
 * the annotations are decoded from the encoded_annotation, not pre-baked C
 * values, so this exercises the production annotation path. */
static const unsigned char reflection_fixture_dex[624] = {
 0x64,0x65,0x78,0x0a,0x30,0x33,0x35,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
 0x70,0x02,0x00,0x00,0x70,0x00,0x00,0x00,0x78,0x56,0x34,0x12,0x00,0x00,0x00,0x00,
 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0e,0x00,0x00,0x00,0x70,0x00,0x00,0x00,
 0x04,0x00,0x00,0x00,0xa8,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0xb8,0x00,0x00,0x00,
 0x02,0x00,0x00,0x00,0xc4,0x00,0x00,0x00,0x03,0x00,0x00,0x00,0xd4,0x00,0x00,0x00,
 0x02,0x00,0x00,0x00,0xec,0x00,0x00,0x00,0x44,0x01,0x00,0x00,0x2c,0x01,0x00,0x00,
 0x2c,0x01,0x00,0x00,0x4b,0x01,0x00,0x00,0x5f,0x01,0x00,0x00,0x73,0x01,0x00,0x00,
 0x92,0x01,0x00,0x00,0x98,0x01,0x00,0x00,0xa3,0x01,0x00,0x00,0xb1,0x01,0x00,0x00,
 0xb9,0x01,0x00,0x00,0xc0,0x01,0x00,0x00,0xc7,0x01,0x00,0x00,0xd3,0x01,0x00,0x00,
 0xde,0x01,0x00,0x00,0xe8,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x00,0x00,0x00,
 0x02,0x00,0x00,0x00,0x03,0x00,0x00,0x00,0x0d,0x00,0x00,0x00,0x01,0x00,0x00,0x00,
 0x00,0x00,0x00,0x00,0x03,0x00,0x01,0x00,0x07,0x00,0x00,0x00,0x03,0x00,0x01,0x00,
 0x08,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x04,0x00,0x00,0x00,0x03,0x00,0x00,0x00,
 0x09,0x00,0x00,0x00,0x03,0x00,0x00,0x00,0x0a,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
 0x01,0x26,0x00,0x00,0x02,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xff,0xff,0xff,0xff,
 0x00,0x00,0x00,0x00,0xec,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x03,0x00,0x00,0x00,
 0x01,0x00,0x00,0x00,0x02,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xff,0xff,0xff,0xff,
 0x34,0x02,0x00,0x00,0xf4,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x1d,0x4c,0x73,0x64,
 0x6b,0x74,0x72,0x61,0x63,0x65,0x2f,0x74,0x65,0x73,0x74,0x73,0x2f,0x52,0x65,0x71,
 0x75,0x65,0x73,0x74,0x50,0x61,0x72,0x61,0x6d,0x3b,0x00,0x12,0x4c,0x6a,0x61,0x76,
 0x61,0x2f,0x6c,0x61,0x6e,0x67,0x2f,0x53,0x74,0x72,0x69,0x6e,0x67,0x3b,0x00,0x12,
 0x4c,0x6a,0x61,0x76,0x61,0x2f,0x6c,0x61,0x6e,0x67,0x2f,0x4f,0x62,0x6a,0x65,0x63,
 0x74,0x3b,0x00,0x1d,0x4c,0x73,0x64,0x6b,0x74,0x72,0x61,0x63,0x65,0x2f,0x74,0x65,
 0x73,0x74,0x73,0x2f,0x44,0x65,0x78,0x41,0x6e,0x6e,0x6f,0x74,0x61,0x74,0x65,0x64,
 0x3b,0x00,0x04,0x6e,0x61,0x6d,0x65,0x00,0x09,0x74,0x61,0x67,0x5f,0x76,0x61,0x6c,
 0x75,0x65,0x00,0x0c,0x6d,0x65,0x74,0x68,0x6f,0x64,0x5f,0x76,0x61,0x6c,0x75,0x65,
 0x00,0x06,0x74,0x61,0x67,0x67,0x65,0x64,0x00,0x05,0x70,0x6c,0x61,0x69,0x6e,0x00,
 0x05,0x76,0x61,0x6c,0x75,0x65,0x00,0x0a,0x70,0x6c,0x61,0x69,0x6e,0x56,0x61,0x6c,
 0x75,0x65,0x00,0x09,0x73,0x74,0x61,0x74,0x69,0x63,0x2d,0x6f,0x6b,0x00,0x08,0x70,
 0x6c,0x61,0x69,0x6e,0x2d,0x6f,0x6b,0x00,0x01,0x4c,0x00,0x00,0x00,0x00,0x01,0x00,
 0x00,0x81,0x08,0x00,0x00,0x02,0x01,0x01,0x00,0x01,0x01,0x01,0x01,0x09,0x84,0x04,
 0x02,0x01,0x9c,0x04,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
 0x03,0x00,0x00,0x00,0x1a,0x00,0x0b,0x00,0x0f,0x00,0x00,0x00,0x01,0x00,0x01,0x00,
 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x03,0x00,0x00,0x00,0x1a,0x00,0x0c,0x00,
 0x0f,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x01,0x00,0x00,0x00,
 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x54,0x02,0x00,0x00,0x01,0x00,0x00,0x00,
 0x5c,0x02,0x00,0x00,0x01,0x00,0x00,0x00,0x64,0x02,0x00,0x00,0x01,0x00,0x00,0x00,
 0x6a,0x02,0x00,0x00,0x01,0x00,0x01,0x04,0x17,0x05,0x01,0x00,0x01,0x04,0x17,0x06,
};
/* Exercise a malformed method table through the real getMethod hook.  The
 * helper restores nmethods=0 on every path BEFORE returning, including the
 * forced-failure call used to prove the old early-return cleanup bug cannot
 * leave a NULL methods table for dvm_destroy(). */
static bool malformed_method_probe(struct dvm *v,const char *descriptor,bool force_failure,bool *cleanup_ok) {
 struct dvm_class *klass=dvm__register_builtin(v,descriptor);
 if(!klass)return false;
 klass->access=DEX_ACC_PUBLIC;
 klass->nmethods=1;klass->methods=NULL;
 dvm_ref class_object=dvm_class_object(v,klass);
 bool internal_error=false;
 if(class_object){
  union dvm_value params={.l=dvm_new_array(v,'L',"Ljava/lang/Class;",0)};
  union dvm_value args[2]={{.l=dvm_new_string(v,"anything")},{.l=params.l}},out={0};
  if(args[0].l&&params.l)
   internal_error=reflection_runtime_call(v,"java/lang/Class","getMethod","(Ljava/lang/String;[Ljava/lang/Class;)Ljava/lang/reflect/Method;",class_object,args,2,&out)&&exception_named(v,"java/lang/InternalError");
 }
 if(dvm_exception(v))dvm_clear_exception(v);
 klass->nmethods=0;
 *cleanup_ok=klass->nmethods==0;
 if(force_failure)return false;
 return class_object&&internal_error;
}
static void task3_report_exception(struct dvm *v,const char *step){
 dvm_ref error=dvm_exception(v);
 if(!error){sdk_trace_fprintf(stderr,"TASK3_EXCEPTION step=%s type=none cause=none\n",step);return;}
 struct dvm_method *first=v->nexc_trace?v->exc_trace[0]:NULL;
 sdk_trace_fprintf(stderr,"TASK3_EXCEPTION_FRAME step=%s frame=%s.%s%s\n",step,first&&first->cls?first->cls->name:"not_found",first?first->name:"-",first?first->sig:"");
 dvm_clear_exception(v);
 for(int depth=0;error&&depth<4;depth++){
  char description[2048]={0};dvm_describe_exception(v,error,description,sizeof description);
  struct dvm_class *type=dvm_object_class(v,error);union dvm_value text={0},stack={0},cause={0};
  throwable_reference_call(v,"java/lang/Throwable","toString","()Ljava/lang/String;",error,NULL,0,&text);
  throwable_reference_call(v,"java/lang/Throwable","getStackTrace","()[Ljava/lang/StackTraceElement;",error,NULL,0,&stack);
  dvm_ref frame=stack.l&&dvm_array_length(v,stack.l)?((dvm_ref*)({const dvm_ref gc_payload_ref_25=(stack.l);if(!gc_operation_admit(v,gc_payload_ref_25))return ;dvm_array_data(v,gc_payload_ref_25);}))[0]:0;
  union dvm_value owner={0},method={0};
  if(frame){dvm_get_field(v,frame,"declaringClass","Ljava/lang/String;",&owner);dvm_get_field(v,frame,"methodName","Ljava/lang/String;",&method);}
  sdk_trace_fprintf(stderr,"TASK3_EXCEPTION step=%s depth=%d type=%s text=%s toString=%s stack0=%s.%s\n",step,depth,type?type->name:"not_found",description,text.l?({const dvm_ref gc_payload_ref_26=(text.l);if(!gc_operation_admit(v,gc_payload_ref_26))return ;dvm_string_utf8(v,gc_payload_ref_26);}):"not_found",owner.l?({const dvm_ref gc_payload_ref_27=(owner.l);if(!gc_operation_admit(v,gc_payload_ref_27))return ;dvm_string_utf8(v,gc_payload_ref_27);}):"not_found",method.l?({const dvm_ref gc_payload_ref_28=(method.l);if(!gc_operation_admit(v,gc_payload_ref_28))return ;dvm_string_utf8(v,gc_payload_ref_28);}):"-");
  throwable_reference_call(v,"java/lang/Throwable","getCause","()Ljava/lang/Throwable;",error,NULL,0,&cause);
  dvm_clear_exception(v);if(cause.l==error)break;error=cause.l;
 }
 dvm_clear_exception(v);
}
/* Exercise actual shipped constructors and Gson's own allocator on the
 * application VM after all three original DEX files have been loaded. No
 * credentials, request data, callbacks or SDK constructor choices are changed. */
static bool reflection_task3_constructor_probe_operation(struct dvm *v){
 struct dvm_class *data=dvm_find_class(v,"com/base/featureflag/FeatureResponse$Data");
 struct dvm_class *outer=dvm_find_class(v,"com/base/featureflag/FeatureResponse");
 if(!data||!outer)return false;
 dvm_ref klass=dvm_class_object(v,data);union dvm_value out={0},arg={0};
 const char *predicates[]={"getModifiers","isMemberClass","isSynthetic"};
 for(int i=0;i<3;i++){
  bool handled=reflection_runtime_call(v,"java/lang/Class",predicates[i],i?"()Z":"()I",klass,NULL,0,&out);
  sdk_trace_fprintf(stderr,"TASK3_CLASS query=%s handled=%d value=%d exception=%u\n",predicates[i],handled,out.i,dvm_exception(v));
  if(dvm_exception(v))task3_report_exception(v,predicates[i]);
 }
 if(!reflection_runtime_call(v,"java/lang/Class","getDeclaredConstructors","()[Ljava/lang/reflect/Constructor;",klass,NULL,0,&out)||dvm_exception(v)||!out.l)return false;
 dvm_ref constructors=out.l;uint32_t count=dvm_array_length(v,constructors);
 sdk_trace_fprintf(stderr,"TASK3_CONSTRUCTORS count=%u class=%s enclosing=%s\n",count,data->name,dex_class_enclosing_type(data->dex,data->class_def_idx));
 for(uint32_t i=0;i<count;i++){
  dvm_ref ref=((dvm_ref*)({const dvm_ref gc_payload_ref_29=(constructors);if(!gc_operation_admit(v,gc_payload_ref_29))return 0;dvm_array_data(v,gc_payload_ref_29);}))[i];const dvm_ref gc_ref_object_85=(ref); if(!gc_operation_admit(v,gc_ref_object_85))return false;reflected *r=dvm__obj(v,gc_ref_object_85)?dvm__obj(v,gc_ref_object_85)->data:NULL;
  if(!r||r->kind!=3||!r->ctor)return false;
  sdk_trace_fprintf(stderr,"TASK3_CONSTRUCTOR index=%u ref=%u signature=%s modifiers=0x%x synthetic=%d accessible=%d\n",i,ref,r->sig,r->ctor->access&0xffff,(r->ctor->access&DEX_ACC_SYNTHETIC)!=0,r->accessible);
 }
 arg.l=dvm_new_array(v,'L',"Ljava/lang/Class;",0);out.l=0;
 bool found=reflection_runtime_call(v,"java/lang/Class","getDeclaredConstructor","([Ljava/lang/Class;)Ljava/lang/reflect/Constructor;",klass,&arg,1,&out);
 sdk_trace_fprintf(stderr,"TASK3_LOOKUP parameters=() handled=%d result=%u exception=%u\n",found,out.l,dvm_exception(v));
 task3_report_exception(v,"getDeclaredConstructor_empty");
 arg.l=dvm_new_array(v,'L',"Ljava/lang/Class;",1);((dvm_ref*)({const dvm_ref gc_payload_ref_30=(arg.l);if(!gc_operation_admit(v,gc_payload_ref_30))return 0;dvm_array_data(v,gc_payload_ref_30);}))[0]=dvm_class_object(v,outer);out.l=0;
 if(!reflection_runtime_call(v,"java/lang/Class","getDeclaredConstructor","([Ljava/lang/Class;)Ljava/lang/reflect/Constructor;",klass,&arg,1,&out)||dvm_exception(v)||!out.l)return false;
 dvm_ref constructor=out.l;union dvm_value values={.l=dvm_new_array(v,'L',"Ljava/lang/Object;",0)};out.l=0;
 reflection_runtime_call(v,"java/lang/reflect/Constructor","newInstance","([Ljava/lang/Object;)Ljava/lang/Object;",constructor,&values,1,&out);
 sdk_trace_fprintf(stderr,"TASK3_NEWINSTANCE args=empty result=%u exception=%u\n",out.l,dvm_exception(v));task3_report_exception(v,"newInstance_empty");
 union dvm_value enabled={.i=1};reflection_runtime_call(v,"java/lang/reflect/AccessibleObject","setAccessible","(Z)V",constructor,&enabled,1,&out);
 reflection_runtime_call(v,"java/lang/reflect/Constructor","isAccessible","()Z",constructor,NULL,0,&out);
 sdk_trace_fprintf(stderr,"TASK3_ACCESSIBLE after_set_true=%d\n",out.i);
 out.l=0;reflection_runtime_call(v,"java/lang/reflect/Constructor","newInstance","([Ljava/lang/Object;)Ljava/lang/Object;",constructor,&values,1,&out);task3_report_exception(v,"newInstance_accessible_empty");
 dvm_ref parent=dvm_new_object(v,outer);struct dvm_method *init=dvm_find_method(v,outer,"<init>","()V");
 if(!init||!dvm_call(v,init,parent,NULL,0,&out))return false;
 values.l=dvm_new_array(v,'L',"Ljava/lang/Object;",1);((dvm_ref*)({const dvm_ref gc_payload_ref_31=(values.l);if(!gc_operation_admit(v,gc_payload_ref_31))return 0;dvm_array_data(v,gc_payload_ref_31);}))[0]=parent;out.l=0;
 if(!reflection_runtime_call(v,"java/lang/reflect/Constructor","newInstance","([Ljava/lang/Object;)Ljava/lang/Object;",constructor,&values,1,&out))return false;
 union dvm_value outer_value={0};if(out.l)dvm_get_field(v,out.l,"this$0","Lcom/base/featureflag/FeatureResponse;",&outer_value);
 sdk_trace_fprintf(stderr,"TASK3_NEWINSTANCE args=outer result=%u exception=%u outer_matches=%d\n",out.l,dvm_exception(v),outer_value.l==parent);
 if(dvm_exception(v))task3_report_exception(v,"newInstance_outer");
 struct dvm_class *allocator_type=dvm_find_class(v,"copy/google/json/internal/UnsafeAllocator");
 struct dvm_method *create=allocator_type?dvm_find_method(v,allocator_type,"create","()Lcopy/google/json/internal/UnsafeAllocator;"):NULL;
 if(!create||!dvm_call(v,create,0,NULL,0,&out))return false;
 dvm_ref allocator=out.l;struct dvm_class *actual=dvm_object_class(v,allocator);
 sdk_trace_fprintf(stderr,"TASK3_GSON_ALLOCATOR class=%s\n",actual?actual->name:"not_found");
 struct dvm_method *allocate=actual?dvm_find_method(v,actual,"newInstance","(Ljava/lang/Class;)Ljava/lang/Object;"):NULL;
 arg.l=klass;out.l=0;bool ok=allocate&&dvm_call(v,allocate,allocator,&arg,1,&out);
 sdk_trace_fprintf(stderr,"TASK3_GSON_ALLOCATE ok=%d result=%u exception=%u\n",ok,out.l,dvm_exception(v));
 task3_report_exception(v,"Gson_UnsafeAllocator_newInstance");
 sdk_trace_fprintf(stderr,"TASK3_PROBE_DONE original_body_executed=1 business_response_substituted=0\n");
 return true;
}
bool reflection_task3_constructor_probe(struct dvm *v){
 struct gc_operation operation __attribute__((cleanup(gc_operation_end)));
 if(!gc_operation_begin(v,&operation))return false;
 bool result=reflection_task3_constructor_probe_operation(v);
 return result;
}

static int unsafe_fixture_initializations;
static bool unsafe_fixture_clinit(struct dvm *v,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 (void)v;(void)self;(void)a;(void)n;(void)out;unsafe_fixture_initializations++;return true;
}
static bool unsafe_allocation_selftest(struct dvm *v){
 struct dvm_class *k=dvm__register_builtin(v,"Lsdktrace/tests/UnsafeAllocation;");
 if(!k)return false;
 k->access=DEX_ACC_PUBLIC;k->islots=3;k->methods=calloc(2,sizeof *k->methods);
 if(!k->methods)return false;
 k->nmethods=2;k->init_state=0;unsafe_fixture_initializations=0;
 install_fixture_method(k,0,"<clinit>","()V",DEX_ACC_STATIC,unsafe_fixture_clinit,'V');
 install_fixture_ctor(k,1,"()V",DEX_ACC_PUBLIC,fixture_throw_ctor);
 struct dvm_class *unsafe=dvm_find_class_or_external(v,"sun/misc/Unsafe");
 union dvm_value name={.l=dvm_new_string(v,"theUnsafe")},out={0};
 dvm_ref klass=dvm_class_object(v,unsafe);
 union dvm_value discovery={.l=dvm_new_string(v,"sun.misc.Unsafe")};
 if(!discovery_runtime_call(v,"java/lang/Class","forName","(Ljava/lang/String;)Ljava/lang/Class;",0,&discovery,1,&out)||out.l!=klass||dvm_exception(v))return false;
 if(!reflection_runtime_call(v,"java/lang/Class","getDeclaredField","(Ljava/lang/String;)Ljava/lang/reflect/Field;",klass,&name,1,&out)||!out.l||dvm_exception(v))return false;
 dvm_ref field=out.l;
 union dvm_value missing={.l=dvm_new_string(v,"notTheUnsafe")};
 if(!reflection_runtime_call(v,"java/lang/Class","getDeclaredField","(Ljava/lang/String;)Ljava/lang/reflect/Field;",klass,&missing,1,&out)||!exception_named(v,"java/lang/NoSuchFieldException"))return false;
 dvm_clear_exception(v);missing.l=0;
 if(!reflection_runtime_call(v,"java/lang/Class","getDeclaredField","(Ljava/lang/String;)Ljava/lang/reflect/Field;",klass,&missing,1,&out)||!exception_named(v,"java/lang/NullPointerException"))return false;
 dvm_clear_exception(v);
 union dvm_value enabled={.i=1};
 if(!reflection_runtime_call(v,"java/lang/reflect/Field","setAccessible","(Z)V",field,&enabled,1,&out)||dvm_exception(v))return false;
 union dvm_value target={0};
 if(!reflection_runtime_call(v,"java/lang/reflect/Field","get","(Ljava/lang/Object;)Ljava/lang/Object;",field,&target,1,&out)||!out.l||dvm_exception(v))return false;
 dvm_ref singleton=out.l;
 const char *types[]={"Ljava/lang/Class;"};
 union dvm_value lookup[2]={{.l=dvm_new_string(v,"allocateInstance")},{.l=fixture_types(v,types,1)}};
 if(!reflection_runtime_call(v,"java/lang/Class","getMethod","(Ljava/lang/String;[Ljava/lang/Class;)Ljava/lang/reflect/Method;",klass,lookup,2,&out)||!out.l||dvm_exception(v))return false;
 dvm_ref method=out.l;
 union dvm_value invocation[2]={{.l=singleton},{.l=dvm_new_array(v,'L',"Ljava/lang/Object;",1)}};
 ((dvm_ref*)({const dvm_ref gc_payload_ref_32=(invocation[1].l);if(!gc_operation_admit(v,gc_payload_ref_32))return 0;dvm_array_data(v,gc_payload_ref_32);}))[0]=dvm_class_object(v,k);
 for(int i=0;i<2;i++){
  if(!reflection_runtime_call(v,"java/lang/reflect/Method","invoke","(Ljava/lang/Object;[Ljava/lang/Object;)Ljava/lang/Object;",method,invocation,2,&out)||!out.l||dvm_exception(v))return false;
  const dvm_ref gc_ref_o_86=(out.l); if(!gc_operation_admit(v,gc_ref_o_86))return false;
  if(!dvm__obj(v,gc_ref_o_86)||dvm__obj(v,gc_ref_o_86)->cls!=k||!dvm__obj(v,gc_ref_o_86)->slots||dvm__obj(v,gc_ref_o_86)->slots[0].j||dvm__obj(v,gc_ref_o_86)->slots[1].j||dvm__obj(v,gc_ref_o_86)->slots[2].j||unsafe_fixture_initializations!=1||k->init_state!=2)return false;
 }
 struct dvm_method *allocate=&unsafe->methods[0];union dvm_value argument={0};
 if(dvm_call(v,allocate,singleton,&argument,1,&out)||!exception_named(v,"java/lang/NullPointerException"))return false;
 dvm_clear_exception(v);
 struct dvm_class *bad[]={dvm__class_by_desc(v,"I"),dvm__class_by_desc(v,"[I"),dvm_find_class_or_external(v,"java/lang/Runnable"),k};
 uint32_t saved=k->access;k->access|=DEX_ACC_ABSTRACT;
 for(int i=0;i<4;i++){
  if(i==2){bad[i]->is_interface=true;bad[i]->access|=DEX_ACC_INTERFACE|DEX_ACC_ABSTRACT;}
  argument.l=dvm_class_object(v,bad[i]);
  if(dvm_call(v,allocate,singleton,&argument,1,&out)||!exception_named(v,"java/lang/InstantiationException")){k->access=saved;return false;}
  dvm_clear_exception(v);
 }
 k->access=saved;
 sdk_trace_fprintf(stderr,"TASK3_UNSAFE_SELFTEST PASS reflection_discovery=1 singleton_field=1 method_invoke=1 constructor_skipped=1 zero_slots=3 class_init_once=1 null_rejected=1 primitive_array_interface_abstract_rejected=4\n");
 return true;
}
static bool mem_reflection_cache_selftest_operation(struct dvm *v){
 {union dvm_value a={.j=7},x={0},y={0};
  bool pass=box_kind(v,'J',a,&x)&&java_basics_call(v,"java/lang/Long","valueOf","(J)Ljava/lang/Long;",0,&a,1,&y)&&x.l&&x.l==y.l&&!dvm_exception(v);
  FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/mem-reflection-selftest.txt","w");if(f){fprintf(f,"REFLECTION_LONG_CACHE %s pid=%d value=7 same_identity=%d\n",pass?"PASS":"FAIL",getpid(),pass);fclose(f);}if(!pass)return false;
 }
 /* Startup only, before play; exercise the real reflection boxing route,
  * including non-cached boundaries and independent constructors. */
 if(perf_leak_small_boxing()){
  const char kinds[]={'Z','B','S','I'};
  const char *classes[]={"java/lang/Boolean","java/lang/Byte","java/lang/Short","java/lang/Integer"};
  const char *ctors[]={"(Z)V","(B)V","(S)V","(I)V"};
  const char *reads[]={"booleanValue","byteValue","shortValue","intValue"};
  const char *read_sigs[]={"()Z","()B","()S","()I"};
  for(unsigned i=0;i<sizeof kinds;i++){
   union dvm_value a={.i=i?127:1},x={0},y={0},r={0};
   if(!box_kind(v,kinds[i],a,&x)||!box_kind(v,kinds[i],a,&y)||!x.l||x.l!=y.l||dvm_exception(v))return false;
   dvm_ref fresh=box_value(v,classes[i],ctors[i],a);
   if(!fresh||fresh==x.l||dvm_exception(v))return false;
   bool read=i==1||i==2?small_boxes_call(v,classes[i],reads[i],read_sigs[i],x.l,NULL,0,&r):java_basics_call(v,classes[i],reads[i],read_sigs[i],x.l,NULL,0,&r);
   if(!read||r.i!=a.i||dvm_exception(v))return false;
   union dvm_value eq={.l=fresh};
   bool equal=i==1||i==2?small_boxes_call(v,classes[i],"equals","(Ljava/lang/Object;)Z",x.l,&eq,1,&r):java_basics_call(v,classes[i],"equals","(Ljava/lang/Object;)Z",x.l,&eq,1,&r);
   if(!equal||!r.i||dvm_exception(v))return false;
  }
  for(unsigned i=0;i<2;i++){
   union dvm_value a={.i=128},x={0},y={0};
   if(!box_kind(v,i?'I':'S',a,&x)||!box_kind(v,i?'I':'S',a,&y)||!x.l||!y.l||x.l==y.l||dvm_exception(v))return false;
  }
 }
 if(perf_leak_metadata_names()){
  const char *descs[]={"I","Ljava/lang/String;","[Ljava/lang/String;","[[I"};
  const char *names[]={"int","java.lang.String","[Ljava.lang.String;","[[I"};
  for(unsigned i=0;i<sizeof descs/sizeof descs[0];i++){
   struct dvm_class *k=dvm__class_by_desc(v,descs[i]);union dvm_value x={0},y={0};
   if(!k)return false;
   dvm_ref c=dvm_class_object(v,k);
   if(!reflection_runtime_call(v,"java/lang/Class","getName","()Ljava/lang/String;",c,NULL,0,&x)||!reflection_runtime_call(v,"java/lang/Class","getName","()Ljava/lang/String;",c,NULL,0,&y)||!x.l||x.l!=y.l||dvm_exception(v)||strcmp(({const dvm_ref gc_payload_ref_33=(x.l);if(!gc_operation_admit(v,gc_payload_ref_33))return 0;dvm_string_utf8(v,gc_payload_ref_33);}),names[i]))return false;
   dvm_ref fresh=dvm_new_string(v,names[i]);
   if(!fresh||fresh==x.l||strcmp(({const dvm_ref gc_payload_ref_34=(fresh);if(!gc_operation_admit(v,gc_payload_ref_34))return 0;dvm_string_utf8(v,gc_payload_ref_34);}),names[i]))return false;
  }
 }
 sdk_trace_fprintf(stderr,"LEAK_FIX_SELFTEST PASS small_boxing=%d metadata_names=%d independent_constructors=1\n",perf_leak_small_boxing(),perf_leak_metadata_names());
 return true;
}
bool mem_reflection_cache_selftest(struct dvm *v){
 struct gc_operation operation __attribute__((cleanup(gc_operation_end)));
 if(!gc_operation_begin(v,&operation))return false;
 bool result=mem_reflection_cache_selftest_operation(v);
 return result;
}

static bool reflection_runtime_selftest_operation(struct dvm *v) {
 if(v!=active)return false;

 if(!unsafe_allocation_selftest(v))return false;
 const char *class_descs[]={"Ljava/lang/String;","Ljava/lang/CharSequence;","I","[Ljava/lang/String;","[[I"};
 const char *class_texts[]={"class java.lang.String","interface java.lang.CharSequence","int","class [Ljava.lang.String;","class [[I"};
 for(int i=0;i<5;i++){
  struct dvm_class *type=dvm__class_by_desc(v,class_descs[i]);union dvm_value text={0};
  if(!type||!reflection_runtime_call(v,"java/lang/Class","toString","()Ljava/lang/String;",dvm_class_object(v,type),NULL,0,&text)||dvm_exception(v)||!text.l||strcmp(({const dvm_ref gc_payload_ref_35=(text.l);if(!gc_operation_admit(v,gc_payload_ref_35))return 0;dvm_string_utf8(v,gc_payload_ref_35);}),class_texts[i]))return false;
 }
 sdk_trace_fprintf(stderr,"TASK2_CLASS_TOSTRING_SELFTEST PASS class=1 interface=1 primitive=1 object_array=1 primitive_array=1\n");
 struct dvm_class *generic_fixture=dvm_find_class_or_external(v,"sdktrace/tests/GenericSuperclass");
 generic_fixture->runtime_signature="Ljava/util/ArrayList<Ljava/util/List<Ljava/lang/String;>;>;";
 union dvm_value generic_result={0},generic_arg={0};
 if(!generic_superclass(v,dvm_class_object(v,generic_fixture),false,&generic_result)||!generic_result.l||dvm_exception(v))return false;
 dvm_ref parameterized=generic_result.l;
 if(!dvm__class_assignable(v,dvm_object_class(v,parameterized),dvm_find_class_or_external(v,"java/lang/reflect/ParameterizedType")))return false;
 if(!reflection_runtime_call(v,"java/lang/reflect/ParameterizedType","getRawType","()Ljava/lang/reflect/Type;",parameterized,NULL,0,&generic_result)||generic_result.l!=dvm_class_object(v,dvm_find_class_or_external(v,"java/util/ArrayList")))return false;
 if(!reflection_runtime_call(v,"java/lang/reflect/ParameterizedType","getActualTypeArguments","()[Ljava/lang/reflect/Type;",parameterized,NULL,0,&generic_result)||dvm_array_length(v,generic_result.l)!=1)return false;
 dvm_ref first_array=generic_result.l,nested=((dvm_ref*)({const dvm_ref gc_payload_ref_36=(first_array);if(!gc_operation_admit(v,gc_payload_ref_36))return 0;dvm_array_data(v,gc_payload_ref_36);}))[0];((dvm_ref*)({const dvm_ref gc_payload_ref_37=(first_array);if(!gc_operation_admit(v,gc_payload_ref_37))return 0;dvm_array_data(v,gc_payload_ref_37);}))[0]=0;
 if(!reflection_runtime_call(v,"java/lang/reflect/ParameterizedType","getActualTypeArguments","()[Ljava/lang/reflect/Type;",parameterized,NULL,0,&generic_result)||((dvm_ref*)({const dvm_ref gc_payload_ref_38=(generic_result.l);if(!gc_operation_admit(v,gc_payload_ref_38))return 0;dvm_array_data(v,gc_payload_ref_38);}))[0]!=nested)return false;
 if(!reflection_runtime_call(v,"java/lang/reflect/ParameterizedType","getActualTypeArguments","()[Ljava/lang/reflect/Type;",nested,NULL,0,&generic_arg)||((dvm_ref*)({const dvm_ref gc_payload_ref_39=(generic_arg.l);if(!gc_operation_admit(v,gc_payload_ref_39))return 0;dvm_array_data(v,gc_payload_ref_39);}))[0]!=dvm_class_object(v,dvm_find_class_or_external(v,"java/lang/String")))return false;
 generic_fixture->runtime_signature="Ljava/lang/Object;Ljava/util/List<Ljava/lang/String;>;Ljava/io/Serializable;";
 if(!generic_superclass(v,dvm_class_object(v,generic_fixture),true,&generic_result)||dvm_array_length(v,generic_result.l)!=2||dvm_exception(v))return false;
 dvm_ref *generic_interfaces=({const dvm_ref gc_payload_ref_40=(generic_result.l);if(!gc_operation_admit(v,gc_payload_ref_40))return 0;dvm_array_data(v,gc_payload_ref_40);});if(generic_interfaces[1]!=dvm_class_object(v,dvm_find_class_or_external(v,"java/io/Serializable")))return false;
 if(!reflection_runtime_call(v,"java/lang/reflect/ParameterizedType","getRawType","()Ljava/lang/reflect/Type;",generic_interfaces[0],NULL,0,&generic_arg)||generic_arg.l!=dvm_class_object(v,dvm_find_class_or_external(v,"java/util/List")))return false;
 generic_fixture->runtime_signature=NULL;
 /* A boot List<E> exposes Collection<E>; its E belongs to List and must
  * compare equal to the variable returned by List.getTypeParameters(). */
 dvm_ref list_class=dvm_class_object(v,dvm_find_class_or_external(v,"java/util/List"));
 if(!generic_superclass(v,list_class,true,&generic_result)||dvm_array_length(v,generic_result.l)!=1||dvm_exception(v))return false;
 dvm_ref test_collection_parameterized=((dvm_ref*)({const dvm_ref gc_payload_ref_41=(generic_result.l);if(!gc_operation_admit(v,gc_payload_ref_41))return 0;dvm_array_data(v,gc_payload_ref_41);}))[0];
 if(!reflection_runtime_call(v,"java/lang/reflect/ParameterizedType","getActualTypeArguments","()[Ljava/lang/reflect/Type;",test_collection_parameterized,NULL,0,&generic_result))return false;
 dvm_ref variable=((dvm_ref*)({const dvm_ref gc_payload_ref_42=(generic_result.l);if(!gc_operation_admit(v,gc_payload_ref_42))return 0;dvm_array_data(v,gc_payload_ref_42);}))[0];
 if(!dvm__class_assignable(v,dvm_object_class(v,variable),dvm_find_class_or_external(v,"java/lang/reflect/TypeVariable")))return false;
 if(!reflection_runtime_call(v,"java/lang/reflect/TypeVariable","getGenericDeclaration","()Ljava/lang/reflect/GenericDeclaration;",variable,NULL,0,&generic_result)||generic_result.l!=list_class)return false;
 if(!reflection_runtime_call(v,"java/lang/Class","getTypeParameters","()[Ljava/lang/reflect/TypeVariable;",list_class,NULL,0,&generic_result)||dvm_array_length(v,generic_result.l)!=1)return false;
 generic_arg.l=((dvm_ref*)({const dvm_ref gc_payload_ref_43=(generic_result.l);if(!gc_operation_admit(v,gc_payload_ref_43))return 0;dvm_array_data(v,gc_payload_ref_43);}))[0];
 if(!reflection_runtime_call(v,"java/lang/reflect/TypeVariable","equals","(Ljava/lang/Object;)Z",variable,&generic_arg,1,&generic_result)||!generic_result.i)return false;
 struct dvm_method *variable_equals=dvm_find_method(v,dvm_object_class(v,variable),"equals","(Ljava/lang/Object;)Z");
 if(!variable_equals||!dvm_call(v,variable_equals,variable,&generic_arg,1,&generic_result)||!generic_result.i||dvm_exception(v))return false;
 dvm_ref foreign=new_variable(v,dvm_find_class_or_external(v,"java/util/Collection"),"E");generic_arg.l=foreign;
 if(!reflection_runtime_call(v,"java/lang/reflect/TypeVariable","equals","(Ljava/lang/Object;)Z",variable,&generic_arg,1,&generic_result)||generic_result.i)return false;
 if(!reflection_runtime_call(v,"java/lang/reflect/TypeVariable","getName","()Ljava/lang/String;",variable,NULL,0,&generic_result)||strcmp(({const dvm_ref gc_payload_ref_44=(generic_result.l);if(!gc_operation_admit(v,gc_payload_ref_44))return 0;dvm_string_utf8(v,gc_payload_ref_44);}),"E"))return false;
 dvm_ref interfaces_array=dvm_new_array(v,'I',"I",0),interfaces_class=dvm_class_object(v,dvm_object_class(v,interfaces_array));
 if(!reflection_runtime_call(v,"java/lang/Class","getInterfaces","()[Ljava/lang/Class;",interfaces_class,NULL,0,&generic_result)||dvm_array_length(v,generic_result.l)!=2)return false;
 dvm_ref *interface_values=({const dvm_ref gc_payload_ref_45=(generic_result.l);if(!gc_operation_admit(v,gc_payload_ref_45))return 0;dvm_array_data(v,gc_payload_ref_45);});
 if(interface_values[0]!=dvm_class_object(v,dvm_find_class_or_external(v,"java/lang/Cloneable"))||interface_values[1]!=dvm_class_object(v,dvm_find_class_or_external(v,"java/io/Serializable")))return false;
 interface_values[0]=0;
 if(!reflection_runtime_call(v,"java/lang/Class","getInterfaces","()[Ljava/lang/Class;",interfaces_class,NULL,0,&generic_result)||!((dvm_ref*)({const dvm_ref gc_payload_ref_46=(generic_result.l);if(!gc_operation_admit(v,gc_payload_ref_46))return 0;dvm_array_data(v,gc_payload_ref_46);}))[0])return false;
 static const char *const t_object[]={"Ljava/lang/Object;"};
 static const char *const t_string[]={"Ljava/lang/String;"};
 static const char *const t_int[]={"I"};
 static const char *const t_long[]={"J"};
 static const char *const t_boolean[]={"Z"};
 static const char *const t_byte[]={"B"};
 static const char *const t_short[]={"S"};
 static const char *const t_int_array[]={"[I"};
 static const char *const t_object_array[]={"[Ljava/lang/Object;"};
 static const char *const t_float[]={"F"};
 static const char *const t_double[]={"D"};
 static const char *const t_char[]={"C"};
 static const char *const ctor_sigs[]={"()V","(Ljava/lang/Object;)V","(Ljava/lang/String;)V","(I)V","(J)V","(Z)V","(B)V","(S)V","([I)V","([Ljava/lang/Object;)V","(F)V","(D)V","(C)V"};
 struct dvm_class *fixture=dvm__register_builtin(v,"Lsdktrace/tests/ReflectCtor;");
 fixture->access=DEX_ACC_PUBLIC;
 fixture->methods=calloc(sizeof ctor_sigs/sizeof ctor_sigs[0],sizeof *fixture->methods);
 if(!fixture->methods)return false;
 fixture->nmethods=(int)(sizeof ctor_sigs/sizeof ctor_sigs[0]);
 for(int i=0;i<fixture->nmethods;i++){install_fixture_ctor(fixture,i,ctor_sigs[i],DEX_ACC_PUBLIC,fixture_ctor);if(!fixture->methods[i].sig)return false;}
 dvm_ref fixture_class=dvm_class_object(v,fixture);
 union dvm_value fout={0};union dvm_value noargs={.l=dvm_new_array(v,'L',"Ljava/lang/Class;",0)};
 if(!reflection_runtime_call(v,"java/lang/Class","getConstructor","([Ljava/lang/Class;)Ljava/lang/reflect/Constructor;",fixture_class,&noargs,1,&fout)||!fout.l)return false;
 dvm_ref ctor_object=fout.l;
 union dvm_value empty={.l=dvm_new_array(v,'L',"Ljava/lang/Object;",0)};
 if(!reflection_runtime_call(v,"java/lang/reflect/Constructor","newInstance","([Ljava/lang/Object;)Ljava/lang/Object;",ctor_object,&empty,1,&fout)||!fout.l)return false;
 const dvm_ref gc_ref_created_87=(fout.l); if(!gc_operation_admit(v,gc_ref_created_87))return false;if(!dvm__obj(v,gc_ref_created_87)||dvm__obj(v,gc_ref_created_87)->cls!=fixture)return false;
 if(!reflection_runtime_call(v,"java/lang/Class","newInstance","()Ljava/lang/Object;",fixture_class,0,0,&fout)||!fout.l||dvm__obj(v,fout.l)->cls!=fixture)return false;
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
 if(!prefs_runtime_call(v,"android/content/SharedPreferences","getString","(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",pref,pa,2,&out)||!out.l||strcmp(({const dvm_ref gc_payload_ref_47=(out.l);if(!gc_operation_admit(v,gc_payload_ref_47))return 0;dvm_string_utf8(v,gc_payload_ref_47);}),"value"))return false;
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
 dvm_ref *list=({const dvm_ref gc_payload_ref_48=(out.l);if(!gc_operation_admit(v,gc_payload_ref_48))return 0;dvm_array_data(v,gc_payload_ref_48);});if(!list||list[0]!=first||list[1]!=second)return false;
 added.l=owner;if(!reflection_runtime_call(v,"java/lang/Throwable","addSuppressed","(Ljava/lang/Throwable;)V",owner,&added,1,&out)||!v->exception)return false;dvm_clear_exception(v);
 added.l=0;if(!reflection_runtime_call(v,"java/lang/Throwable","addSuppressed","(Ljava/lang/Throwable;)V",owner,&added,1,&out)||!v->exception)return false;dvm_clear_exception(v);
 dvm_ref params=dvm_new_array(v,'L',"Ljava/lang/Class;",1);((dvm_ref*)({const dvm_ref gc_payload_ref_49=(params);if(!gc_operation_admit(v,gc_payload_ref_49))return 0;dvm_array_data(v,gc_payload_ref_49);}))[0]=dvm_class_object(v,base);
 args[0].l=dvm_new_string(v,"addSuppressed");args[1].l=params;
 if(!reflection_runtime_call(v,"java/lang/Class","getDeclaredMethod","(Ljava/lang/String;[Ljava/lang/Class;)Ljava/lang/reflect/Method;",dvm_class_object(v,base),args,2,&out)||!out.l)return false;
 dvm_ref add_method=out.l,values=dvm_new_array(v,'L',"Ljava/lang/Object;",1);((dvm_ref*)({const dvm_ref gc_payload_ref_50=(values);if(!gc_operation_admit(v,gc_payload_ref_50))return 0;dvm_array_data(v,gc_payload_ref_50);}))[0]=first;
 invoke[0].l=owner;invoke[1].l=values;
 if(!reflection_runtime_call(v,"java/lang/reflect/Method","invoke","(Ljava/lang/Object;[Ljava/lang/Object;)Ljava/lang/Object;",add_method,invoke,2,&out)||out.l||v->exception)return false;
 if(!reflection_runtime_call(v,"java/lang/Throwable","getSuppressed","()[Ljava/lang/Throwable;",owner,NULL,0,&out)||dvm_array_length(v,out.l)!=3)return false;
 /* R4: constructor arguments and exception rules.  Expected exceptions are
  * cleared before the next case because the VM has a single pending slot. */
 int ctor_present=0;
 dvm_ref tested_ctor=fixture_constructor(v,fixture_class,t_object,1,&ctor_present);
 dvm_ref ctor_values=object_arguments(v,0,1),ctor_result=0;
 if(!tested_ctor||!ctor_present||!ctor_values)return false;
 if(!construct(v,tested_ctor,ctor_values,&ctor_result)||!ctor_result||dvm_exception(v))return false;
 if(fixture_argument_count!=1||fixture_argument[0].l!=0)return false;
 dvm_ref text_value=dvm_new_string(v,"argument");
 ctor_values=object_arguments(v,text_value,1);ctor_result=0;
 if(!construct(v,tested_ctor,ctor_values,&ctor_result)||!ctor_result||dvm_exception(v)||fixture_argument[0].l!=text_value)return false;
 tested_ctor=fixture_constructor(v,fixture_class,t_string,1,&ctor_present);
 dvm_ref integer_box=box_value(v,"java/lang/Integer","(I)V",(union dvm_value){.i=7});
 if(!tested_ctor||!integer_box)return false;
 ctor_values=object_arguments(v,integer_box,1);ctor_result=0;
 if(!construct(v,tested_ctor,ctor_values,&ctor_result)||ctor_result||!exception_named(v,"java/lang/IllegalArgumentException"))return false;
 dvm_clear_exception(v);
 /* Arrays accept null; non-null values must match the array type. */
 tested_ctor=fixture_constructor(v,fixture_class,t_int_array,1,&ctor_present);
 if(!tested_ctor)return false;
 ctor_values=object_arguments(v,0,1);ctor_result=0;
 if(!construct(v,tested_ctor,ctor_values,&ctor_result)||!ctor_result||dvm_exception(v)||fixture_argument[0].l!=0)return false;
 dvm_ref int_array=dvm_new_array(v,'I',"I",1);
 dvm_ref string_array=dvm_new_array(v,'L',"Ljava/lang/String;",1);
 if(!int_array||!string_array)return false;
 ctor_values=object_arguments(v,int_array,1);ctor_result=0;
 if(!construct(v,tested_ctor,ctor_values,&ctor_result)||!ctor_result||dvm_exception(v)||fixture_argument[0].l!=int_array)return false;
 ctor_values=object_arguments(v,string_array,1);ctor_result=0;
 if(!construct(v,tested_ctor,ctor_values,&ctor_result)||ctor_result||!exception_named(v,"java/lang/IllegalArgumentException"))return false;
 dvm_clear_exception(v);
 tested_ctor=fixture_constructor(v,fixture_class,t_object_array,1,&ctor_present);
 if(!tested_ctor)return false;
 ctor_values=object_arguments(v,string_array,1);ctor_result=0;
 if(!construct(v,tested_ctor,ctor_values,&ctor_result)||!ctor_result||dvm_exception(v))return false;
 ctor_values=object_arguments(v,int_array,1);ctor_result=0;
 if(!construct(v,tested_ctor,ctor_values,&ctor_result)||ctor_result||!exception_named(v,"java/lang/IllegalArgumentException"))return false;
 dvm_clear_exception(v);
 /* Exact primitive conversions: Integer->I, Long->J, Boolean->Z, Byte->B,
  * Short->S.  The value is observed in the builtin constructor. */
 tested_ctor=fixture_constructor(v,fixture_class,t_int,1,&ctor_present);
 if(!tested_ctor)return false;
 ctor_values=object_arguments(v,integer_box,1);ctor_result=0;
 if(!construct(v,tested_ctor,ctor_values,&ctor_result)||!ctor_result||dvm_exception(v)||fixture_argument[0].i!=7)return false;
 union dvm_value wide_arg={0};wide_arg.j=(int64_t)0x100000002LL;
 dvm_ref long_box=box_value(v,"java/lang/Long","(J)V",wide_arg);
 tested_ctor=fixture_constructor(v,fixture_class,t_long,1,&ctor_present);
 if(!tested_ctor||!long_box)return false;
 ctor_values=object_arguments(v,long_box,1);ctor_result=0;
 if(!construct(v,tested_ctor,ctor_values,&ctor_result)||!ctor_result||dvm_exception(v)||fixture_argument[0].j!=(int64_t)0x100000002LL)return false;
 union dvm_value flag_arg={.i=1};
 dvm_ref bool_box=box_value(v,"java/lang/Boolean","(Z)V",flag_arg);
 tested_ctor=fixture_constructor(v,fixture_class,t_boolean,1,&ctor_present);
 if(!tested_ctor||!bool_box)return false;
 ctor_values=object_arguments(v,bool_box,1);ctor_result=0;
 if(!construct(v,tested_ctor,ctor_values,&ctor_result)||!ctor_result||dvm_exception(v)||fixture_argument[0].i!=1)return false;
 union dvm_value small_arg={.i=-5};
 dvm_ref byte_box=box_value(v,"java/lang/Byte","(B)V",small_arg);
 tested_ctor=fixture_constructor(v,fixture_class,t_byte,1,&ctor_present);
 if(!tested_ctor||!byte_box)return false;
 ctor_values=object_arguments(v,byte_box,1);ctor_result=0;
 if(!construct(v,tested_ctor,ctor_values,&ctor_result)||!ctor_result||dvm_exception(v)||fixture_argument[0].i!=-5)return false;
 small_arg.i=300;
 dvm_ref short_box=box_value(v,"java/lang/Short","(S)V",small_arg);
 tested_ctor=fixture_constructor(v,fixture_class,t_short,1,&ctor_present);
 if(!tested_ctor||!short_box)return false;
 ctor_values=object_arguments(v,short_box,1);ctor_result=0;
 if(!construct(v,tested_ctor,ctor_values,&ctor_result)||!ctor_result||dvm_exception(v)||fixture_argument[0].i!=300)return false;
 /* Widening conversions accepted by reflection: Integer->long, Byte->int,
  * Character->int, Float->double.  The converted primitive is observed in
  * the builtin constructor. */
 tested_ctor=fixture_constructor(v,fixture_class,t_long,1,&ctor_present);
 if(!tested_ctor)return false;
 ctor_values=object_arguments(v,integer_box,1);ctor_result=0;
 if(!construct(v,tested_ctor,ctor_values,&ctor_result)||!ctor_result||dvm_exception(v)||fixture_argument[0].j!=7)return false;
 tested_ctor=fixture_constructor(v,fixture_class,t_int,1,&ctor_present);
 if(!tested_ctor)return false;
 ctor_values=object_arguments(v,byte_box,1);ctor_result=0;
 if(!construct(v,tested_ctor,ctor_values,&ctor_result)||!ctor_result||dvm_exception(v)||fixture_argument[0].i!=-5)return false;
 union dvm_value char_arg={.i=0x4e2d};
 dvm_ref char_box=box_value(v,"java/lang/Character","(C)V",char_arg);
 tested_ctor=fixture_constructor(v,fixture_class,t_int,1,&ctor_present);
 if(!char_box||!tested_ctor)return false;
 ctor_values=object_arguments(v,char_box,1);ctor_result=0;
 if(!construct(v,tested_ctor,ctor_values,&ctor_result)||!ctor_result||dvm_exception(v)||fixture_argument[0].i!=0x4e2d)return false;
 union dvm_value float_arg={.f=1.5f};
 dvm_ref float_box=box_value(v,"java/lang/Float","(F)V",float_arg);
 tested_ctor=fixture_constructor(v,fixture_class,t_double,1,&ctor_present);
 if(!float_box||!tested_ctor)return false;
 ctor_values=object_arguments(v,float_box,1);ctor_result=0;
 if(!construct(v,tested_ctor,ctor_values,&ctor_result)||!ctor_result||dvm_exception(v)||fixture_argument[0].d!=1.5)return false;
 /* The wrapper's own primitive still works (Float->F, Character->C). */
 tested_ctor=fixture_constructor(v,fixture_class,t_float,1,&ctor_present);
 if(!tested_ctor)return false;
 ctor_values=object_arguments(v,float_box,1);ctor_result=0;
 if(!construct(v,tested_ctor,ctor_values,&ctor_result)||!ctor_result||dvm_exception(v)||fixture_argument[0].f!=1.5f)return false;
 tested_ctor=fixture_constructor(v,fixture_class,t_char,1,&ctor_present);
 if(!tested_ctor)return false;
 ctor_values=object_arguments(v,char_box,1);ctor_result=0;
 if(!construct(v,tested_ctor,ctor_values,&ctor_result)||!ctor_result||dvm_exception(v)||fixture_argument[0].i!=0x4e2d)return false;
 /* Narrowing and null primitives are rejected, not silently converted. */
 tested_ctor=fixture_constructor(v,fixture_class,t_int,1,&ctor_present);
 if(!tested_ctor)return false;
 ctor_values=object_arguments(v,long_box,1);ctor_result=0;
 if(!construct(v,tested_ctor,ctor_values,&ctor_result)||ctor_result||!exception_named(v,"java/lang/IllegalArgumentException"))return false;
 dvm_clear_exception(v);
 ctor_values=object_arguments(v,0,1);ctor_result=0;
 if(!construct(v,tested_ctor,ctor_values,&ctor_result)||ctor_result||!exception_named(v,"java/lang/IllegalArgumentException"))return false;
 dvm_clear_exception(v);
 union dvm_value double_arg={.d=2.25};
 dvm_ref double_box=box_value(v,"java/lang/Double","(D)V",double_arg);
 tested_ctor=fixture_constructor(v,fixture_class,t_float,1,&ctor_present);
 if(!double_box||!tested_ctor)return false;
 ctor_values=object_arguments(v,double_box,1);ctor_result=0;
 if(!construct(v,tested_ctor,ctor_values,&ctor_result)||ctor_result||!exception_named(v,"java/lang/IllegalArgumentException"))return false;
 dvm_clear_exception(v);
 tested_ctor=fixture_constructor(v,fixture_class,t_int,1,&ctor_present);
 if(!tested_ctor)return false;
 ctor_values=object_arguments(v,bool_box,1);ctor_result=0;
 if(!construct(v,tested_ctor,ctor_values,&ctor_result)||ctor_result||!exception_named(v,"java/lang/IllegalArgumentException"))return false;
 dvm_clear_exception(v);
 /* Constructor.newInstance wraps the constructor's throwable, preserving
  * its cause; Class.newInstance propagates the original throwable. */
 struct dvm_class *throwing=dvm__register_builtin(v,"Lsdktrace/tests/ReflectThrow;");
 throwing->access=DEX_ACC_PUBLIC;
 throwing->methods=calloc(1,sizeof *throwing->methods);
 if(!throwing->methods)return false;
 throwing->nmethods=1;
 install_fixture_ctor(throwing,0,"()V",DEX_ACC_PUBLIC,fixture_throw_ctor);
 if(!throwing->methods[0].sig)return false;
 dvm_ref throwing_class=dvm_class_object(v,throwing);
 tested_ctor=fixture_constructor(v,throwing_class,NULL,0,&ctor_present);
 ctor_values=object_arguments(v,0,0);ctor_result=0;
 if(!tested_ctor||!ctor_present||!ctor_values)return false;
 if(!construct(v,tested_ctor,ctor_values,&ctor_result)||ctor_result||!exception_named(v,"java/lang/reflect/InvocationTargetException"))return false;
 dvm_ref wrapper=dvm_exception(v);
 union dvm_value cause={0};
 if(!throwable_reference_call(v,"java/lang/reflect/InvocationTargetException","getCause","()Ljava/lang/Throwable;",wrapper,NULL,0,&cause)||!cause.l||!class_named(v,cause.l,"java/lang/IllegalStateException"))return false;
 if(!throwable_reference_call(v,"java/lang/reflect/InvocationTargetException","getMessage","()Ljava/lang/String;",wrapper,NULL,0,&cause)||!cause.l)return false;
 const char *cause_text=({const dvm_ref gc_payload_ref_51=(cause.l);if(!gc_operation_admit(v,gc_payload_ref_51))return 0;dvm_string_utf8(v,gc_payload_ref_51);});
 if(!cause_text||!strstr(cause_text,"IllegalStateException")||!strstr(cause_text,"fixture constructor boom"))return false;
 dvm_clear_exception(v);
 union dvm_value direct={0};
 if(!reflection_runtime_call(v,"java/lang/Class","newInstance","()Ljava/lang/Object;",throwing_class,NULL,0,&direct)||direct.l||!exception_named(v,"java/lang/IllegalStateException"))return false;
 dvm_clear_exception(v);
 /* Abstract and private constructors. */
 struct dvm_class *abstract_fixture=dvm__register_builtin(v,"Lsdktrace/tests/ReflectAbstract;");
 abstract_fixture->access=DEX_ACC_PUBLIC|DEX_ACC_ABSTRACT;
 abstract_fixture->methods=calloc(1,sizeof *abstract_fixture->methods);
 if(!abstract_fixture->methods)return false;
 abstract_fixture->nmethods=1;
 install_fixture_ctor(abstract_fixture,0,"()V",DEX_ACC_PUBLIC,fixture_ctor);
 if(!abstract_fixture->methods[0].sig)return false;
 dvm_ref abstract_class=dvm_class_object(v,abstract_fixture);
 tested_ctor=fixture_constructor(v,abstract_class,NULL,0,&ctor_present);
 ctor_values=object_arguments(v,0,0);ctor_result=0;
 if(!tested_ctor||!ctor_present||!ctor_values)return false;
 if(!construct(v,tested_ctor,ctor_values,&ctor_result)||ctor_result||!exception_named(v,"java/lang/InstantiationException"))return false;
 dvm_clear_exception(v);
 direct.l=0;
 if(!reflection_runtime_call(v,"java/lang/Class","newInstance","()Ljava/lang/Object;",abstract_class,NULL,0,&direct)||direct.l||!exception_named(v,"java/lang/InstantiationException"))return false;
 dvm_clear_exception(v);
 struct dvm_class *private_fixture=dvm__register_builtin(v,"Lsdktrace/tests/ReflectPrivate;");
 private_fixture->access=DEX_ACC_PUBLIC;
 private_fixture->methods=calloc(1,sizeof *private_fixture->methods);
 if(!private_fixture->methods)return false;
 private_fixture->nmethods=1;
 install_fixture_ctor(private_fixture,0,"()V",0,fixture_ctor);
 if(!private_fixture->methods[0].sig)return false;
 dvm_ref private_class=dvm_class_object(v,private_fixture);
 int private_present=0;
 tested_ctor=fixture_constructor(v,private_class,NULL,0,&private_present);
 if(tested_ctor||private_present||!exception_named(v,"java/lang/NoSuchMethodException"))return false;
 dvm_clear_exception(v);
 struct dvm_method *private_method=dvm_find_method(v,private_fixture,"<init>","()V");
 reflected *private_reflected=calloc(1,sizeof *private_reflected);
 dvm_ref private_object=private_method?dvm_new_object(v,dvm_find_class_or_external(v,"java/lang/reflect/Constructor")):0;
 const dvm_ref gc_ref_private_slot_88=((private_object)?(private_object):0); if(!gc_operation_admit(v,gc_ref_private_slot_88))return false;
 if(!private_reflected||!dvm__obj(v,gc_ref_private_slot_88)){free(private_reflected);return false;}
 private_reflected->owner=private_fixture;private_reflected->kind=3;private_reflected->ctor=private_method;private_reflected->sig=strdup("()V");
 if(!private_reflected->sig){free(private_reflected);return false;}
 dvm__obj(v,gc_ref_private_slot_88)->data=private_reflected;dvm__obj(v,gc_ref_private_slot_88)->data_destroy=reflected_free;
 ctor_values=object_arguments(v,0,0);ctor_result=0;
 if(!construct(v,private_object,ctor_values,&ctor_result)||ctor_result||!exception_named(v,"java/lang/IllegalAccessException"))return false;
 dvm_clear_exception(v);
 /* Class.isAssignableFrom: class hierarchy, interface implementation and
  * inherited superinterfaces, primitive identity, array covariance, and the
  * null/non-Class error contract.  dvm_runtime_install() has already run
  * collections_vm_start(), so the ArrayList/List/Collection/Iterable
  * relations below are the real builtin metadata. */
 struct dvm_class *object_type=dvm_find_class_or_external(v,"java/lang/Object");
 struct dvm_class *string_type=dvm_find_class_or_external(v,"java/lang/String");
 struct dvm_class *integer_type=dvm_find_class_or_external(v,"java/lang/Integer");
 struct dvm_class *list_type=dvm_find_class_or_external(v,"java/util/List");
 struct dvm_class *collection_type=dvm_find_class_or_external(v,"java/util/Collection");
 struct dvm_class *iterable_type=dvm_find_class_or_external(v,"java/lang/Iterable");
 struct dvm_class *array_list_type=dvm_find_class_or_external(v,"java/util/ArrayList");
 struct dvm_class *primitive_int=dvm__define_primitive(v,"I");
 struct dvm_class *string_array_type=dvm__class_by_desc(v,"[Ljava/lang/String;");
 struct dvm_class *object_array_type=dvm__class_by_desc(v,"[Ljava/lang/Object;");
 struct dvm_class *int_array_type=dvm__class_by_desc(v,"[I");
 if(!object_type||!string_type||!integer_type||!list_type||!collection_type||!iterable_type||!array_list_type||!primitive_int||!string_array_type||!object_array_type||!int_array_type)return false;
 dvm_ref object_type_class=dvm_class_object(v,object_type),string_type_class=dvm_class_object(v,string_type),integer_type_class=dvm_class_object(v,integer_type);
 dvm_ref list_type_class=dvm_class_object(v,list_type),collection_type_class=dvm_class_object(v,collection_type),iterable_type_class=dvm_class_object(v,iterable_type);
 dvm_ref array_list_class=dvm_class_object(v,array_list_type),primitive_int_class=dvm_class_object(v,primitive_int);
 dvm_ref string_array_class=dvm_class_object(v,string_array_type),object_array_class=dvm_class_object(v,object_array_type),int_array_class=dvm_class_object(v,int_array_type);
 if(!object_type_class||!string_type_class||!integer_type_class||!list_type_class||!collection_type_class||!iterable_type_class||!array_list_class||!primitive_int_class||!string_array_class||!object_array_class||!int_array_class)return false;
 /* Class hierarchy: Object covers String; the reverse and class/primitive
  * cross relations do not hold. */
 if(!assignable_expect(v,object_type_class,string_type_class,true))return false;
 if(!assignable_expect(v,string_type_class,object_type_class,false))return false;
 if(!assignable_expect(v,string_type_class,string_type_class,true))return false;
 /* Interfaces: a direct interface, a superinterface reached through the
  * interface chain, and the reversed directions that must stay false. */
 if(!assignable_expect(v,list_type_class,array_list_class,true))return false;
 if(!assignable_expect(v,iterable_type_class,array_list_class,true))return false;
 if(!assignable_expect(v,array_list_class,list_type_class,false))return false;
 if(!assignable_expect(v,list_type_class,collection_type_class,false))return false;
 /* Primitive classes match only themselves, never their wrapper or Object. */
 if(!assignable_expect(v,primitive_int_class,primitive_int_class,true))return false;
 if(!assignable_expect(v,primitive_int_class,integer_type_class,false))return false;
 if(!assignable_expect(v,integer_type_class,primitive_int_class,false))return false;
 if(!assignable_expect(v,object_type_class,primitive_int_class,false))return false;
 /* Arrays: Object covers every array; reference arrays are covariant; a
  * primitive array element never widens to a reference type. */
 if(!assignable_expect(v,object_type_class,string_array_class,true))return false;
 if(!assignable_expect(v,string_array_class,string_array_class,true))return false;
 if(!assignable_expect(v,object_array_class,string_array_class,true))return false;
 if(!assignable_expect(v,string_array_class,object_array_class,false))return false;
 if(!assignable_expect(v,int_array_class,int_array_class,true))return false;
 if(!assignable_expect(v,object_type_class,int_array_class,true))return false;
 if(!assignable_expect(v,object_array_class,int_array_class,false))return false;
 if(!assignable_expect(v,int_array_class,object_array_class,false))return false;
 union dvm_value null_type={.l=0},assignable_out={0};
 if(!reflection_runtime_call(v,"java/lang/Class","isAssignableFrom","(Ljava/lang/Class;)Z",object_type_class,&null_type,1,&assignable_out)||!exception_named(v,"java/lang/NullPointerException"))return false;
 dvm_clear_exception(v);
 dvm_ref not_class=dvm_new_string(v,"not a Class");
 union dvm_value bad_type={.l=not_class};
 if(!not_class||!reflection_runtime_call(v,"java/lang/Class","isAssignableFrom","(Ljava/lang/Class;)Z",object_type_class,&bad_type,1,&assignable_out)||!exception_named(v,"java/lang/IllegalArgumentException"))return false;
 dvm_clear_exception(v);
 bad_type.l=string_type_class;
 if(!reflection_runtime_call(v,"java/lang/Class","isAssignableFrom","(Ljava/lang/Class;)Z",not_class,&bad_type,1,&assignable_out)||!exception_named(v,"java/lang/IllegalArgumentException"))return false;
 dvm_clear_exception(v);
 /* Class.isArray: reference and primitive arrays are arrays; ordinary and
  * primitive classes are not.  Null/non-Class receivers fail closed. */
 union dvm_value array_flag={0};
 if(!reflection_runtime_call(v,"java/lang/Class","isArray","()Z",int_array_class,NULL,0,&array_flag)||array_flag.i!=1)return false;
 if(!reflection_runtime_call(v,"java/lang/Class","isArray","()Z",object_array_class,NULL,0,&array_flag)||array_flag.i!=1)return false;
 if(!reflection_runtime_call(v,"java/lang/Class","isArray","()Z",string_type_class,NULL,0,&array_flag)||array_flag.i!=0)return false;
 if(!reflection_runtime_call(v,"java/lang/Class","isArray","()Z",primitive_int_class,NULL,0,&array_flag)||array_flag.i!=0)return false;
 if(!reflection_runtime_call(v,"java/lang/Class","isArray","()Z",0,NULL,0,&array_flag)||!exception_named(v,"java/lang/NullPointerException"))return false;
 dvm_clear_exception(v);
 if(!reflection_runtime_call(v,"java/lang/Class","isArray","()Z",not_class,NULL,0,&array_flag)||!exception_named(v,"java/lang/IllegalArgumentException"))return false;
 dvm_clear_exception(v);
 /* R5: Class.getDeclaredFields() and java.lang.reflect.Field.  The fixture
  * mirrors a dex class: three instance fields and one static field with
  * real names, descriptors and access flags. */
 struct dvm_class *fields_fixture=dvm__register_builtin(v,"Lsdktrace/tests/ReflectFields;");
 if(!fields_fixture)return false;
 fields_fixture->access=DEX_ACC_PUBLIC;
 fields_fixture->nifields=3;
 fields_fixture->ifields=calloc(3,sizeof *fields_fixture->ifields);
 fields_fixture->nsfields=1;
 fields_fixture->sfields=calloc(1,sizeof *fields_fixture->sfields);
 fields_fixture->nsslots=1;
 fields_fixture->sslots=calloc(1,sizeof *fields_fixture->sslots);
 fields_fixture->islots=3;
 fields_fixture->init_state=2;
 if(!fields_fixture->ifields||!fields_fixture->sfields||!fields_fixture->sslots)return false;
 fixture_field(&fields_fixture->ifields[0],fields_fixture,"text","Ljava/lang/String;",DEX_ACC_PUBLIC,0);
 fixture_field(&fields_fixture->ifields[1],fields_fixture,"count","I",DEX_ACC_PRIVATE,1);
 fixture_field(&fields_fixture->ifields[2],fields_fixture,"flag","Z",DEX_ACC_PUBLIC|DEX_ACC_FINAL,2);
 fixture_field(&fields_fixture->sfields[0],fields_fixture,"shared","J",DEX_ACC_PUBLIC|DEX_ACC_STATIC,0);
 dvm_ref fields_class=dvm_class_object(v,fields_fixture);
 union dvm_value fields_out={0};
 if(!reflection_runtime_call(v,"java/lang/Class","getDeclaredFields","()[Ljava/lang/reflect/Field;",fields_class,NULL,0,&fields_out)||dvm_exception(v)||!fields_out.l)return false;
 if(dvm_array_length(v,fields_out.l)!=4)return false;
 dvm_ref text_field=declared_field_named(v,fields_out.l,"text");
 dvm_ref count_field=declared_field_named(v,fields_out.l,"count");
 dvm_ref flag_field=declared_field_named(v,fields_out.l,"flag");
 dvm_ref shared_field=declared_field_named(v,fields_out.l,"shared");
 if(!text_field||!count_field||!flag_field||!shared_field)return false;
 union dvm_value declaring_out={0};
 if(!reflection_runtime_call(v,"java/lang/reflect/Field","getDeclaringClass","()Ljava/lang/Class;",text_field,NULL,0,&declaring_out)||dvm_exception(v)||declaring_out.l!=fields_class)return false;
 struct dvm_class *fields_string=dvm_find_class_or_external(v,"java/lang/String");
 struct dvm_class *fields_int=dvm__define_primitive(v,"I");
 struct dvm_class *fields_boolean=dvm__define_primitive(v,"Z");
 struct dvm_class *fields_long=dvm__define_primitive(v,"J");
 if(!fields_string||!fields_int||!fields_boolean||!fields_long)return false;
 if(!field_type_is(v,text_field,fields_string)||!field_type_is(v,count_field,fields_int)||!field_type_is(v,flag_field,fields_boolean)||!field_type_is(v,shared_field,fields_long))return false;
 if(!field_modifiers_are(v,text_field,DEX_ACC_PUBLIC)||!field_modifiers_are(v,count_field,DEX_ACC_PRIVATE)||!field_modifiers_are(v,shared_field,DEX_ACC_PUBLIC|DEX_ACC_STATIC))return false;
 /* Field.get reads the assigned slot; primitive values are boxed through
  * the wrapper's real value method, never fabricated. */
 dvm_ref fields_target=dvm_new_object(v,fields_fixture);
 const dvm_ref gc_ref_fields_object_89=((fields_target)?(fields_target):0); if(!gc_operation_admit(v,gc_ref_fields_object_89))return false;
 dvm_ref field_text=dvm_new_string(v,"field value");
 if(!dvm__obj(v,gc_ref_fields_object_89)||!dvm__obj(v,gc_ref_fields_object_89)->slots||!field_text)return false;
 dvm__obj(v,gc_ref_fields_object_89)->slots[0].l=field_text;
 dvm__obj(v,gc_ref_fields_object_89)->slots[1].i=42;
 dvm__obj(v,gc_ref_fields_object_89)->slots[2].i=1;
 union dvm_value field_got={0};
 union dvm_value field_arg={.l=fields_target};
 if(!reflection_runtime_call(v,"java/lang/reflect/Field","get","(Ljava/lang/Object;)Ljava/lang/Object;",text_field,&field_arg,1,&field_got)||dvm_exception(v)||field_got.l!=field_text)return false;
 field_got.l=0;
 if(!reflection_runtime_call(v,"java/lang/reflect/Field","get","(Ljava/lang/Object;)Ljava/lang/Object;",count_field,&field_arg,1,&field_got)||dvm_exception(v)||!field_boxed_int(v,field_got.l,"java/lang/Integer","intValue","()I",42))return false;
 field_got.l=0;
 if(!reflection_runtime_call(v,"java/lang/reflect/Field","get","(Ljava/lang/Object;)Ljava/lang/Object;",flag_field,&field_arg,1,&field_got)||dvm_exception(v)||!field_boxed_int(v,field_got.l,"java/lang/Boolean","booleanValue","()Z",1))return false;
 /* Static fields ignore a null receiver and initialise the owner first. */
 fields_fixture->sslots[0].j=(int64_t)0x100000002LL;
 field_got.l=0;field_arg.l=0;
 if(!reflection_runtime_call(v,"java/lang/reflect/Field","get","(Ljava/lang/Object;)Ljava/lang/Object;",shared_field,&field_arg,1,&field_got)||dvm_exception(v)||!field_got.l||!class_named(v,field_got.l,"java/lang/Long"))return false;
 union dvm_value long_read={0};
 if(!java_basics_call(v,"java/lang/Long","longValue","()J",field_got.l,NULL,0,&long_read)||dvm_exception(v)||long_read.j!=(int64_t)0x100000002LL)return false;
 /* Field writes preserve identity and honour the real accessibility flag. */
 union dvm_value accessible={.i=1},ignored={0};
 if(!reflection_runtime_call(v,"java/lang/reflect/Field","setAccessible","(Z)V",text_field,&accessible,1,&ignored)||dvm_exception(v))return false;
 union dvm_value write_args[2]={{.l=fields_target},{.l=dvm_new_string(v,"replacement")}};
 if(!reflection_runtime_call(v,"java/lang/reflect/Field","set","(Ljava/lang/Object;Ljava/lang/Object;)V",text_field,write_args,2,&ignored)||dvm_exception(v)||dvm__obj(v,gc_ref_fields_object_89)->slots[0].l!=write_args[1].l)return false;
 write_args[1].l=0;
 if(!reflection_runtime_call(v,"java/lang/reflect/Field","set","(Ljava/lang/Object;Ljava/lang/Object;)V",text_field,write_args,2,&ignored)||dvm_exception(v)||dvm__obj(v,gc_ref_fields_object_89)->slots[0].l)return false;
 dvm__obj(v,gc_ref_fields_object_89)->slots[0].l=field_text;
 accessible.i=0;
 if(!reflection_runtime_call(v,"java/lang/reflect/Field","setAccessible","(Z)V",text_field,&accessible,1,&ignored)||dvm_exception(v))return false;
 /* Errors are explicit: null receiver is NPE, a non-owner receiver is
  * IllegalArgumentException, unknown signatures are not claimed. */
 field_arg.l=0;field_got.l=0;
 if(!reflection_runtime_call(v,"java/lang/reflect/Field","get","(Ljava/lang/Object;)Ljava/lang/Object;",count_field,&field_arg,1,&field_got)||!exception_named(v,"java/lang/NullPointerException"))return false;
 dvm_clear_exception(v);
 struct dvm_class *fields_other=dvm__register_builtin(v,"Lsdktrace/tests/ReflectFieldsOther;");
 fields_other->islots=1;
 dvm_ref fields_wrong=dvm_new_object(v,fields_other);
 if(!fields_wrong)return false;
 field_arg.l=fields_wrong;field_got.l=0;
 if(!reflection_runtime_call(v,"java/lang/reflect/Field","get","(Ljava/lang/Object;)Ljava/lang/Object;",count_field,&field_arg,1,&field_got)||!exception_named(v,"java/lang/IllegalArgumentException"))return false;
 dvm_clear_exception(v);
 /* A bare Field object with no owner/field metadata must not be read as if
  * it were a real field. */
 dvm_ref fields_bare=dvm_new_object(v,dvm_find_class_or_external(v,"java/lang/reflect/Field"));
 field_arg.l=fields_target;field_got.l=0;
 if(!fields_bare||!reflection_runtime_call(v,"java/lang/reflect/Field","get","(Ljava/lang/Object;)Ljava/lang/Object;",fields_bare,&field_arg,1,&field_got)||!exception_named(v,"java/lang/IllegalArgumentException"))return false;
 dvm_clear_exception(v);
 if(reflection_runtime_call(v,"java/lang/Class","getDeclaredFields","()[Ljava/lang/reflect/Method;",fields_class,NULL,0,&field_got))return false;
 if(reflection_runtime_call(v,"java/lang/reflect/Field","get","()Ljava/lang/Object;",text_field,NULL,0,&field_got))return false;
 if(reflection_runtime_call(v,"java/lang/reflect/Field","getModifiers","()J",text_field,NULL,0,&field_got))return false;
 if(!reflection_runtime_call(v,"java/lang/Class","getDeclaredFields","()[Ljava/lang/reflect/Field;",0,NULL,0,&field_got)||!exception_named(v,"java/lang/NullPointerException"))return false;
 dvm_clear_exception(v);
 /* External classes have no member metadata here; an empty array would be
  * a silent lie, so they fail closed. */
 struct dvm_class *fields_external=dvm_find_class_or_external(v,"Lsdktrace/tests/ReflectExternalFields;");
 dvm_ref fields_external_class=dvm_class_object(v,fields_external);
 if(!fields_external||!fields_external->external||!fields_external_class)return false;
 if(!reflection_runtime_call(v,"java/lang/Class","getDeclaredFields","()[Ljava/lang/reflect/Field;",fields_external_class,NULL,0,&field_got)||!exception_named(v,"java/lang/UnsupportedOperationException"))return false;
 dvm_clear_exception(v);
 /* Arrays really do declare no fields; an empty array is correct there. */
 struct dvm_class *fields_array=dvm__class_by_desc(v,"[Ljava/lang/String;");
 dvm_ref fields_array_class=dvm_class_object(v,fields_array);
 field_got.l=0;
 if(!fields_array_class||!reflection_runtime_call(v,"java/lang/Class","getDeclaredFields","()[Ljava/lang/reflect/Field;",fields_array_class,NULL,0,&field_got)||dvm_exception(v)||!field_got.l||dvm_array_length(v,field_got.l)!=0)return false;
 /* R6: dex-backed annotations and Method reflection.  The fixture is loaded
  * as a real in-memory dex, so every annotation below is decoded from its
  * encoded_annotation by the same code the application dex uses. */
 if(!dvm_add_dex_memory(v,reflection_fixture_dex,sizeof reflection_fixture_dex,"sdktrace-reflection-fixture.dex"))return false;
 struct dvm_class *annot_type=dvm__class_by_desc(v,"Lsdktrace/tests/RequestParam;");
 struct dvm_class *annot_owner=dvm__class_by_desc(v,"Lsdktrace/tests/DexAnnotated;");
 if(!annot_type||!annot_owner||annot_type->external||annot_owner->external)return false;
 if(!(annot_type->access&DEX_ACC_ANNOTATION)||!annot_type->dex||!annot_owner->dex)return false;
 struct dvm_method *annot_element=dvm_find_method(v,annot_type,"name","()Ljava/lang/String;");
 if(!annot_element)return false;
 dvm_ref annot_type_class=dvm_class_object(v,annot_type);
 dvm_ref annot_owner_class=dvm_class_object(v,annot_owner);
 if(!annot_type_class||!annot_owner_class)return false;
 union dvm_value annot_arg={.l=annot_type_class};
 union dvm_value annot_wrong_arg={.l=string_type_class};
 union dvm_value annot_null_arg={.l=0};
 union dvm_value annot_out={0};
 /* getDeclaredMethods() returns exactly the two declared methods, in dex
  * order, with no inherited Object methods and no <init>/<clinit>. */
 if(!reflection_runtime_call(v,"java/lang/Class","getDeclaredMethods","()[Ljava/lang/reflect/Method;",annot_owner_class,NULL,0,&annot_out)||dvm_exception(v)||!annot_out.l||dvm_array_length(v,annot_out.l)!=2)return false;
 dvm_ref *annot_methods=(dvm_ref*)({const dvm_ref gc_payload_ref_52=(annot_out.l);if(!gc_operation_admit(v,gc_payload_ref_52))return 0;dvm_array_data(v,gc_payload_ref_52);});
 if(!annot_methods)return false;
 dvm_ref annot_method=0,annot_plain=0;
 for(uint32_t i=0;i<2;i++) {
  union dvm_value present={0};
  if(!annot_methods[i]||!reflection_runtime_call(v,"java/lang/reflect/Method","isAnnotationPresent","(Ljava/lang/Class;)Z",annot_methods[i],&annot_arg,1,&present)||dvm_exception(v))return false;
  if(present.i) {
   if(annot_method)return false;
   annot_method=annot_methods[i];
  } else {
   if(annot_plain)return false;
   annot_plain=annot_methods[i];
  }
 }
 if(!annot_method||!annot_plain)return false;
 /* Method.isAnnotationPresent: true only for the annotated method and only
  * for the annotation's own Class; a null Class is the specified NPE. */
 union dvm_value annot_present={0};
 if(!reflection_runtime_call(v,"java/lang/reflect/Method","isAnnotationPresent","(Ljava/lang/Class;)Z",annot_method,&annot_wrong_arg,1,&annot_present)||dvm_exception(v)||annot_present.i)return false;
 if(!reflection_runtime_call(v,"java/lang/reflect/Method","isAnnotationPresent","(Ljava/lang/Class;)Z",annot_method,&annot_null_arg,1,&annot_present)||!exception_named(v,"java/lang/NullPointerException"))return false;
 dvm_clear_exception(v);
 /* Method.getAnnotation returns a live annotation instance whose element
  * method reads the encoded value ("method_value"). */
 if(!reflection_runtime_call(v,"java/lang/reflect/Method","getAnnotation","(Ljava/lang/Class;)Ljava/lang/annotation/Annotation;",annot_method,&annot_arg,1,&annot_out)||dvm_exception(v)||!annot_out.l)return false;
 struct dvm_object *annot_object=dvm__obj(v,annot_out.l);
 if(!annot_object||annot_object->kind!=DVM_OBJ_PLAIN||annot_object->cls!=annot_type)return false;
 union dvm_value annot_element_value={0};
 if(!dvm_call(v,annot_element,annot_out.l,NULL,0,&annot_element_value)||dvm_exception(v)||!annot_element_value.l||strcmp(({const dvm_ref gc_payload_ref_53=(annot_element_value.l);if(!gc_operation_admit(v,gc_payload_ref_53))return 0;dvm_string_utf8(v,gc_payload_ref_53);}),"method_value"))return false;
 annot_out.l=0;
 if(!reflection_runtime_call(v,"java/lang/reflect/Method","getAnnotation","(Ljava/lang/Class;)Ljava/lang/annotation/Annotation;",annot_plain,&annot_arg,1,&annot_out)||dvm_exception(v)||annot_out.l)return false;
 if(!reflection_runtime_call(v,"java/lang/reflect/Method","getAnnotation","(Ljava/lang/Class;)Ljava/lang/annotation/Annotation;",annot_method,&annot_wrong_arg,1,&annot_out)||dvm_exception(v)||annot_out.l)return false;
 if(!reflection_runtime_call(v,"java/lang/reflect/Method","getAnnotation","(Ljava/lang/Class;)Ljava/lang/annotation/Annotation;",annot_method,&annot_null_arg,1,&annot_out)||!exception_named(v,"java/lang/NullPointerException"))return false;
 dvm_clear_exception(v);
 /* Field.isAnnotationPresent/getAnnotation use the same dex metadata. */
 if(!reflection_runtime_call(v,"java/lang/Class","getDeclaredFields","()[Ljava/lang/reflect/Field;",annot_owner_class,NULL,0,&annot_out)||dvm_exception(v)||!annot_out.l||dvm_array_length(v,annot_out.l)!=2)return false;
 dvm_ref annot_tagged=declared_field_named(v,annot_out.l,"tagged");
 dvm_ref annot_plain_field=declared_field_named(v,annot_out.l,"plain");
 if(!annot_tagged||!annot_plain_field)return false;
 union dvm_value annot_field_present={0};
 if(!reflection_runtime_call(v,"java/lang/reflect/Field","isAnnotationPresent","(Ljava/lang/Class;)Z",annot_tagged,&annot_arg,1,&annot_field_present)||dvm_exception(v)||!annot_field_present.i)return false;
 if(!reflection_runtime_call(v,"java/lang/reflect/Field","isAnnotationPresent","(Ljava/lang/Class;)Z",annot_plain_field,&annot_arg,1,&annot_field_present)||dvm_exception(v)||annot_field_present.i)return false;
 if(!reflection_runtime_call(v,"java/lang/reflect/Field","isAnnotationPresent","(Ljava/lang/Class;)Z",annot_tagged,&annot_wrong_arg,1,&annot_field_present)||dvm_exception(v)||annot_field_present.i)return false;
 if(!reflection_runtime_call(v,"java/lang/reflect/Field","isAnnotationPresent","(Ljava/lang/Class;)Z",annot_tagged,&annot_null_arg,1,&annot_field_present)||!exception_named(v,"java/lang/NullPointerException"))return false;
 dvm_clear_exception(v);
 annot_out.l=0;
 if(!reflection_runtime_call(v,"java/lang/reflect/Field","getAnnotation","(Ljava/lang/Class;)Ljava/lang/annotation/Annotation;",annot_tagged,&annot_arg,1,&annot_out)||dvm_exception(v)||!annot_out.l)return false;
 annot_object=dvm__obj(v,annot_out.l);
 if(!annot_object||annot_object->cls!=annot_type)return false;
 annot_element_value.l=0;
 if(!dvm_call(v,annot_element,annot_out.l,NULL,0,&annot_element_value)||dvm_exception(v)||!annot_element_value.l||strcmp(({const dvm_ref gc_payload_ref_54=(annot_element_value.l);if(!gc_operation_admit(v,gc_payload_ref_54))return 0;dvm_string_utf8(v,gc_payload_ref_54);}),"tag_value"))return false;
 annot_out.l=0;
 if(!reflection_runtime_call(v,"java/lang/reflect/Field","getAnnotation","(Ljava/lang/Class;)Ljava/lang/annotation/Annotation;",annot_plain_field,&annot_arg,1,&annot_out)||dvm_exception(v)||annot_out.l)return false;
 if(!reflection_runtime_call(v,"java/lang/reflect/Field","getAnnotation","(Ljava/lang/Class;)Ljava/lang/annotation/Annotation;",annot_tagged,&annot_wrong_arg,1,&annot_out)||dvm_exception(v)||annot_out.l)return false;
 if(!reflection_runtime_call(v,"java/lang/reflect/Field","getAnnotation","(Ljava/lang/Class;)Ljava/lang/annotation/Annotation;",annot_tagged,&annot_null_arg,1,&annot_out)||!exception_named(v,"java/lang/NullPointerException"))return false;
 dvm_clear_exception(v);
 /* Method.setAccessible and Method.getReturnType. */
 union dvm_value annot_accessible={.i=1},annot_ignored={0};
 if(!reflection_runtime_call(v,"java/lang/reflect/Method","setAccessible","(Z)V",annot_method,&annot_accessible,1,&annot_ignored)||dvm_exception(v))return false;
 annot_accessible.i=0;
 if(!reflection_runtime_call(v,"java/lang/reflect/Method","setAccessible","(Z)V",annot_method,&annot_accessible,1,&annot_ignored)||dvm_exception(v))return false;
 union dvm_value annot_return={0};
 if(!reflection_runtime_call(v,"java/lang/reflect/Method","getReturnType","()Ljava/lang/Class;",annot_method,NULL,0,&annot_return)||dvm_exception(v)||!annot_return.l)return false;
 struct dvm_object *annot_return_object=dvm__obj(v,annot_return.l);
 if(!annot_return_object||annot_return_object->kind!=DVM_OBJ_CLASS||annot_return_object->klass!=string_type)return false;
 annot_return.l=0;
 if(!reflection_runtime_call(v,"java/lang/reflect/Method","getReturnType","()Ljava/lang/Class;",annot_plain,NULL,0,&annot_return)||dvm_exception(v)||!annot_return.l)return false;
 annot_return_object=dvm__obj(v,annot_return.l);
 if(!annot_return_object||annot_return_object->kind!=DVM_OBJ_CLASS||annot_return_object->klass!=string_type)return false;
 /* Method.invoke: static methods ignore the receiver (null accepted),
  * instance methods execute against a real receiver, and the values come
  * from the fixture's bytecode. */
 dvm_ref annot_empty=dvm_new_array(v,'L',"Ljava/lang/Object;",0);
 dvm_ref annot_receiver=dvm_new_object(v,annot_owner);
 if(!annot_empty||!annot_receiver)return false;
 union dvm_value annot_invoke[2]={{.l=0},{.l=annot_empty}};
 annot_out.l=0;
 if(!reflection_runtime_call(v,"java/lang/reflect/Method","invoke","(Ljava/lang/Object;[Ljava/lang/Object;)Ljava/lang/Object;",annot_method,annot_invoke,2,&annot_out)||dvm_exception(v)||!annot_out.l||strcmp(({const dvm_ref gc_payload_ref_55=(annot_out.l);if(!gc_operation_admit(v,gc_payload_ref_55))return 0;dvm_string_utf8(v,gc_payload_ref_55);}),"static-ok"))return false;
 annot_invoke[0].l=annot_receiver;annot_out.l=0;
 if(!reflection_runtime_call(v,"java/lang/reflect/Method","invoke","(Ljava/lang/Object;[Ljava/lang/Object;)Ljava/lang/Object;",annot_plain,annot_invoke,2,&annot_out)||dvm_exception(v)||!annot_out.l||strcmp(({const dvm_ref gc_payload_ref_56=(annot_out.l);if(!gc_operation_admit(v,gc_payload_ref_56))return 0;dvm_string_utf8(v,gc_payload_ref_56);}),"plain-ok"))return false;
 annot_invoke[0].l=0;annot_out.l=0;
 if(!reflection_runtime_call(v,"java/lang/reflect/Method","invoke","(Ljava/lang/Object;[Ljava/lang/Object;)Ljava/lang/Object;",annot_plain,annot_invoke,2,&annot_out)||!exception_named(v,"java/lang/NullPointerException"))return false;
 dvm_clear_exception(v);
 dvm_ref annot_wrong_receiver=dvm_new_object(v,object_type);
 dvm_ref annot_one_arg=dvm_new_array(v,'L',"Ljava/lang/Object;",1);
 if(!annot_wrong_receiver||!annot_one_arg)return false;
 annot_invoke[0].l=annot_wrong_receiver;
 if(!reflection_runtime_call(v,"java/lang/reflect/Method","invoke","(Ljava/lang/Object;[Ljava/lang/Object;)Ljava/lang/Object;",annot_plain,annot_invoke,2,&annot_out)||!exception_named(v,"java/lang/IllegalArgumentException"))return false;
 dvm_clear_exception(v);
 annot_invoke[0].l=0;annot_invoke[1].l=annot_one_arg;
 if(!reflection_runtime_call(v,"java/lang/reflect/Method","invoke","(Ljava/lang/Object;[Ljava/lang/Object;)Ljava/lang/Object;",annot_method,annot_invoke,2,&annot_out)||!exception_named(v,"java/lang/IllegalArgumentException"))return false;
 dvm_clear_exception(v);
 /* R7: generic Class.getMethod/getDeclaredMethod against real dex methods.
  * DexAnnotated.plainValue is a public instance getter and value is a public
  * static method; both must be found by name and an empty Class[] and then
  * invoke through the same dvm_method metadata. */
 union dvm_value m7_params0={.l=dvm_new_array(v,'L',"Ljava/lang/Class;",0)};
 union dvm_value m7_args[2]={{.l=dvm_new_string(v,"plainValue")},{.l=m7_params0.l}};
 union dvm_value m7_out={0};
 if(!m7_args[0].l||!m7_params0.l)return false;
 if(!reflection_runtime_call(v,"java/lang/Class","getMethod","(Ljava/lang/String;[Ljava/lang/Class;)Ljava/lang/reflect/Method;",annot_owner_class,m7_args,2,&m7_out)||dvm_exception(v)||!m7_out.l)return false;
 dvm_ref m7_plain=m7_out.l;
 union dvm_value m7_invoke[2]={{.l=annot_receiver},{.l=annot_empty}};
 m7_out.l=0;
 if(!reflection_runtime_call(v,"java/lang/reflect/Method","invoke","(Ljava/lang/Object;[Ljava/lang/Object;)Ljava/lang/Object;",m7_plain,m7_invoke,2,&m7_out)||dvm_exception(v)||!m7_out.l||strcmp(({const dvm_ref gc_payload_ref_57=(m7_out.l);if(!gc_operation_admit(v,gc_payload_ref_57))return 0;dvm_string_utf8(v,gc_payload_ref_57);}),"plain-ok"))return false;
 m7_args[0].l=dvm_new_string(v,"value");m7_out.l=0;
 if(!reflection_runtime_call(v,"java/lang/Class","getMethod","(Ljava/lang/String;[Ljava/lang/Class;)Ljava/lang/reflect/Method;",annot_owner_class,m7_args,2,&m7_out)||dvm_exception(v)||!m7_out.l)return false;
 dvm_ref m7_static=m7_out.l;
 m7_invoke[0].l=0;m7_out.l=0;
 if(!reflection_runtime_call(v,"java/lang/reflect/Method","invoke","(Ljava/lang/Object;[Ljava/lang/Object;)Ljava/lang/Object;",m7_static,m7_invoke,2,&m7_out)||dvm_exception(v)||!m7_out.l||strcmp(({const dvm_ref gc_payload_ref_58=(m7_out.l);if(!gc_operation_admit(v,gc_payload_ref_58))return 0;dvm_string_utf8(v,gc_payload_ref_58);}),"static-ok"))return false;
 /* Method.getName/getParameterTypes read the same signature. */
 union dvm_value m7_text={0};
 if(!reflection_runtime_call(v,"java/lang/reflect/Method","getName","()Ljava/lang/String;",m7_plain,NULL,0,&m7_text)||dvm_exception(v)||!m7_text.l||strcmp(({const dvm_ref gc_payload_ref_59=(m7_text.l);if(!gc_operation_admit(v,gc_payload_ref_59))return 0;dvm_string_utf8(v,gc_payload_ref_59);}),"plainValue"))return false;
 if(!reflection_runtime_call(v,"java/lang/reflect/Method","getParameterTypes","()[Ljava/lang/Class;",m7_plain,NULL,0,&m7_text)||dvm_exception(v)||!m7_text.l||dvm_array_length(v,m7_text.l)!=0)return false;
 /* An absent name and a parameter mismatch are NoSuchMethodException. */
 m7_args[0].l=dvm_new_string(v,"plainMissing");m7_out.l=0;
 if(!reflection_runtime_call(v,"java/lang/Class","getMethod","(Ljava/lang/String;[Ljava/lang/Class;)Ljava/lang/reflect/Method;",annot_owner_class,m7_args,2,&m7_out)||m7_out.l||!exception_named(v,"java/lang/NoSuchMethodException"))return false;
 dvm_clear_exception(v);
 dvm_ref m7_string_params=dvm_new_array(v,'L',"Ljava/lang/Class;",1);
 if(!m7_string_params)return false;
 ((dvm_ref*)({const dvm_ref gc_payload_ref_60=(m7_string_params);if(!gc_operation_admit(v,gc_payload_ref_60))return 0;dvm_array_data(v,gc_payload_ref_60);}))[0]=string_type_class;
 m7_args[0].l=dvm_new_string(v,"plainValue");m7_args[1].l=m7_string_params;m7_out.l=0;
 if(!reflection_runtime_call(v,"java/lang/Class","getMethod","(Ljava/lang/String;[Ljava/lang/Class;)Ljava/lang/reflect/Method;",annot_owner_class,m7_args,2,&m7_out)||m7_out.l||!exception_named(v,"java/lang/NoSuchMethodException"))return false;
 dvm_clear_exception(v);
 /* getDeclaredMethod searches the receiver class only, while getMethod also
  * searches its superclass chain and then interfaces.  These fixtures use the
  * real super/ifaces links, not a separate lookup table. */
 struct dvm_class *m7_parent=dvm__register_builtin(v,"Lsdktrace/tests/ReflectMethodParent;");
 struct dvm_class *m7_iface=dvm__register_builtin(v,"Lsdktrace/tests/ReflectMethodIface;");
 struct dvm_class *m7_child=dvm__register_builtin(v,"Lsdktrace/tests/ReflectMethodChild;");
 if(!m7_parent||!m7_iface||!m7_child)return false;
 m7_parent->access=DEX_ACC_PUBLIC;
 m7_parent->methods=calloc(3,sizeof *m7_parent->methods);
 if(!m7_parent->methods)return false;
 m7_parent->nmethods=3;
 install_fixture_method(m7_parent,0,"getInherited","()Ljava/lang/String;",DEX_ACC_PUBLIC,fixture_parent_method,'L');
 install_fixture_method(m7_parent,1,"hiddenGetter","()Ljava/lang/String;",DEX_ACC_PRIVATE,fixture_parent_method,'L');
 install_fixture_method(m7_parent,2,"staticInherited","()Ljava/lang/String;",DEX_ACC_PUBLIC|DEX_ACC_STATIC,fixture_parent_method,'L');
 for(int i=0;i<m7_parent->nmethods;i++)if(!m7_parent->methods[i].sig)return false;
 m7_iface->access=DEX_ACC_PUBLIC|DEX_ACC_INTERFACE|DEX_ACC_ABSTRACT;
 m7_iface->methods=calloc(1,sizeof *m7_iface->methods);
 if(!m7_iface->methods)return false;
 m7_iface->nmethods=1;
 install_fixture_method(m7_iface,0,"getViaIface","()Ljava/lang/String;",DEX_ACC_PUBLIC|DEX_ACC_ABSTRACT,fixture_parent_method,'L');
 if(!m7_iface->methods[0].sig)return false;
 m7_child->access=DEX_ACC_PUBLIC;
 m7_child->super=m7_parent;
 m7_child->ifaces=calloc(1,sizeof *m7_child->ifaces);
 m7_child->methods=calloc(2,sizeof *m7_child->methods);
 if(!m7_child->ifaces||!m7_child->methods)return false;
 m7_child->nifaces=1;m7_child->ifaces[0]=m7_iface;
 m7_child->nmethods=2;
 install_fixture_method(m7_child,0,"getValue","()Ljava/lang/String;",DEX_ACC_PUBLIC,fixture_child_method,'L');
 install_fixture_method(m7_child,1,"setValue","(Ljava/lang/String;)V",DEX_ACC_PUBLIC,fixture_setter_method,'V');
 if(!m7_child->methods[0].sig||!m7_child->methods[1].sig)return false;
 dvm_ref m7_parent_class=dvm_class_object(v,m7_parent),m7_child_class=dvm_class_object(v,m7_child);
 dvm_ref m7_target=dvm_new_object(v,m7_child);
 if(!m7_parent_class||!m7_child_class||!m7_target)return false;
 /* Own public method. */
 m7_args[0].l=dvm_new_string(v,"getValue");m7_args[1].l=m7_params0.l;m7_out.l=0;
 if(!reflection_runtime_call(v,"java/lang/Class","getMethod","(Ljava/lang/String;[Ljava/lang/Class;)Ljava/lang/reflect/Method;",m7_child_class,m7_args,2,&m7_out)||dvm_exception(v)||!m7_out.l)return false;
 dvm_ref m7_getter=m7_out.l;
 m7_invoke[0].l=m7_target;m7_invoke[1].l=annot_empty;m7_out.l=0;
 if(!reflection_runtime_call(v,"java/lang/reflect/Method","invoke","(Ljava/lang/Object;[Ljava/lang/Object;)Ljava/lang/Object;",m7_getter,m7_invoke,2,&m7_out)||dvm_exception(v)||!m7_out.l||strcmp(({const dvm_ref gc_payload_ref_61=(m7_out.l);if(!gc_operation_admit(v,gc_payload_ref_61))return 0;dvm_string_utf8(v,gc_payload_ref_61);}),"child-value"))return false;
 /* Inherited public method and the declaring class's method object. */
 m7_args[0].l=dvm_new_string(v,"getInherited");m7_out.l=0;
 if(!reflection_runtime_call(v,"java/lang/Class","getMethod","(Ljava/lang/String;[Ljava/lang/Class;)Ljava/lang/reflect/Method;",m7_child_class,m7_args,2,&m7_out)||dvm_exception(v)||!m7_out.l)return false;
 dvm_ref m7_inherited=m7_out.l;
 m7_invoke[0].l=m7_target;m7_out.l=0;
 if(!reflection_runtime_call(v,"java/lang/reflect/Method","invoke","(Ljava/lang/Object;[Ljava/lang/Object;)Ljava/lang/Object;",m7_inherited,m7_invoke,2,&m7_out)||dvm_exception(v)||!m7_out.l||strcmp(({const dvm_ref gc_payload_ref_62=(m7_out.l);if(!gc_operation_admit(v,gc_payload_ref_62))return 0;dvm_string_utf8(v,gc_payload_ref_62);}),"parent-value"))return false;
 /* An inherited public static method is visible too. */
 m7_args[0].l=dvm_new_string(v,"staticInherited");m7_out.l=0;
 if(!reflection_runtime_call(v,"java/lang/Class","getMethod","(Ljava/lang/String;[Ljava/lang/Class;)Ljava/lang/reflect/Method;",m7_child_class,m7_args,2,&m7_out)||dvm_exception(v)||!m7_out.l)return false;
 /* Public interface methods reached through ifaces are visible. */
 m7_args[0].l=dvm_new_string(v,"getViaIface");m7_out.l=0;
 if(!reflection_runtime_call(v,"java/lang/Class","getMethod","(Ljava/lang/String;[Ljava/lang/Class;)Ljava/lang/reflect/Method;",m7_child_class,m7_args,2,&m7_out)||dvm_exception(v)||!m7_out.l)return false;
 /* A non-public method is not a getMethod result, and getDeclaredMethod does
  * not inherit it from the superclass. */
 m7_args[0].l=dvm_new_string(v,"hiddenGetter");m7_out.l=0;
 if(!reflection_runtime_call(v,"java/lang/Class","getMethod","(Ljava/lang/String;[Ljava/lang/Class;)Ljava/lang/reflect/Method;",m7_child_class,m7_args,2,&m7_out)||m7_out.l||!exception_named(v,"java/lang/NoSuchMethodException"))return false;
 dvm_clear_exception(v);
 m7_args[0].l=dvm_new_string(v,"getInherited");m7_out.l=0;
 if(!reflection_runtime_call(v,"java/lang/Class","getDeclaredMethod","(Ljava/lang/String;[Ljava/lang/Class;)Ljava/lang/reflect/Method;",m7_child_class,m7_args,2,&m7_out)||m7_out.l||!exception_named(v,"java/lang/NoSuchMethodException"))return false;
 dvm_clear_exception(v);
 m7_args[0].l=dvm_new_string(v,"getValue");m7_out.l=0;
 if(!reflection_runtime_call(v,"java/lang/Class","getDeclaredMethod","(Ljava/lang/String;[Ljava/lang/Class;)Ljava/lang/reflect/Method;",m7_child_class,m7_args,2,&m7_out)||dvm_exception(v)||!m7_out.l)return false;
 dvm_ref m7_declared=m7_out.l;
 m7_invoke[0].l=m7_target;m7_out.l=0;
 if(!reflection_runtime_call(v,"java/lang/reflect/Method","invoke","(Ljava/lang/Object;[Ljava/lang/Object;)Ljava/lang/Object;",m7_declared,m7_invoke,2,&m7_out)||dvm_exception(v)||!m7_out.l||strcmp(({const dvm_ref gc_payload_ref_63=(m7_out.l);if(!gc_operation_admit(v,gc_payload_ref_63))return 0;dvm_string_utf8(v,gc_payload_ref_63);}),"child-value"))return false;
 /* Exact parameter descriptors: the String setter is found with Class[] =
  * {String.class} and its parameter types report that same Class. */
 dvm_ref m7_value=dvm_new_string(v,"argument");
 if(!m7_value)return false;
 m7_args[0].l=dvm_new_string(v,"setValue");m7_args[1].l=m7_string_params;m7_out.l=0;
 if(!reflection_runtime_call(v,"java/lang/Class","getMethod","(Ljava/lang/String;[Ljava/lang/Class;)Ljava/lang/reflect/Method;",m7_child_class,m7_args,2,&m7_out)||dvm_exception(v)||!m7_out.l)return false;
 dvm_ref m7_setter=m7_out.l;
 m7_text.l=0;
 if(!reflection_runtime_call(v,"java/lang/reflect/Method","getName","()Ljava/lang/String;",m7_setter,NULL,0,&m7_text)||dvm_exception(v)||!m7_text.l||strcmp(({const dvm_ref gc_payload_ref_64=(m7_text.l);if(!gc_operation_admit(v,gc_payload_ref_64))return 0;dvm_string_utf8(v,gc_payload_ref_64);}),"setValue"))return false;
 m7_text.l=0;
 if(!reflection_runtime_call(v,"java/lang/reflect/Method","getParameterTypes","()[Ljava/lang/Class;",m7_setter,NULL,0,&m7_text)||dvm_exception(v)||!m7_text.l||dvm_array_length(v,m7_text.l)!=1)return false;
 dvm_ref *m7_setter_types=(dvm_ref*)({const dvm_ref gc_payload_ref_65=(m7_text.l);if(!gc_operation_admit(v,gc_payload_ref_65))return 0;dvm_array_data(v,gc_payload_ref_65);});
 const dvm_ref gc_ref_m7_setter_type_90=((m7_setter_types)?(m7_setter_types[0]):0); if(!gc_operation_admit(v,gc_ref_m7_setter_type_90))return false;
 if(!dvm__obj(v,gc_ref_m7_setter_type_90)||dvm__obj(v,gc_ref_m7_setter_type_90)->kind!=DVM_OBJ_CLASS||dvm__obj(v,gc_ref_m7_setter_type_90)->klass!=string_type)return false;
 dvm_ref m7_set_value_arg=dvm_new_array(v,'L',"Ljava/lang/Object;",1);
 if(!m7_set_value_arg)return false;
 ((dvm_ref*)({const dvm_ref gc_payload_ref_66=(m7_set_value_arg);if(!gc_operation_admit(v,gc_payload_ref_66))return 0;dvm_array_data(v,gc_payload_ref_66);}))[0]=m7_value;
 fixture_method_argument=0;
 m7_invoke[0].l=m7_target;m7_invoke[1].l=m7_set_value_arg;m7_out.l=0;
 bool fixture_argument_ok=reflection_runtime_call(v,"java/lang/reflect/Method","invoke","(Ljava/lang/Object;[Ljava/lang/Object;)Ljava/lang/Object;",m7_setter,m7_invoke,2,&m7_out)&&!dvm_exception(v)&&!m7_out.l&&fixture_method_argument==m7_value;
 fixture_method_argument=0;if(!fixture_argument_ok)return false;
 /* Empty Class[] does not match setValue(String). */
 m7_args[1].l=m7_params0.l;m7_out.l=0;
 if(!reflection_runtime_call(v,"java/lang/Class","getMethod","(Ljava/lang/String;[Ljava/lang/Class;)Ljava/lang/reflect/Method;",m7_child_class,m7_args,2,&m7_out)||m7_out.l||!exception_named(v,"java/lang/NoSuchMethodException"))return false;
 dvm_clear_exception(v);
 /* null name, null parameter array and a null element are NPE. */
 m7_args[0].l=0;m7_args[1].l=m7_params0.l;m7_out.l=0;
 if(!reflection_runtime_call(v,"java/lang/Class","getMethod","(Ljava/lang/String;[Ljava/lang/Class;)Ljava/lang/reflect/Method;",m7_child_class,m7_args,2,&m7_out)||!exception_named(v,"java/lang/NullPointerException"))return false;
 dvm_clear_exception(v);
 m7_args[0].l=dvm_new_string(v,"getValue");m7_args[1].l=0;m7_out.l=0;
 if(!reflection_runtime_call(v,"java/lang/Class","getMethod","(Ljava/lang/String;[Ljava/lang/Class;)Ljava/lang/reflect/Method;",m7_child_class,m7_args,2,&m7_out)||!exception_named(v,"java/lang/NullPointerException"))return false;
 dvm_clear_exception(v);
 dvm_ref m7_null_element=dvm_new_array(v,'L',"Ljava/lang/Class;",1);
 dvm_ref m7_bad_element=dvm_new_array(v,'L',"Ljava/lang/Class;",1);
 if(!m7_null_element||!m7_bad_element)return false;
 ((dvm_ref*)({const dvm_ref gc_payload_ref_67=(m7_null_element);if(!gc_operation_admit(v,gc_payload_ref_67))return 0;dvm_array_data(v,gc_payload_ref_67);}))[0]=0;
 ((dvm_ref*)({const dvm_ref gc_payload_ref_68=(m7_bad_element);if(!gc_operation_admit(v,gc_payload_ref_68))return 0;dvm_array_data(v,gc_payload_ref_68);}))[0]=dvm_new_string(v,"not a Class");
 m7_args[0].l=dvm_new_string(v,"setValue");m7_args[1].l=m7_null_element;m7_out.l=0;
 if(!reflection_runtime_call(v,"java/lang/Class","getMethod","(Ljava/lang/String;[Ljava/lang/Class;)Ljava/lang/reflect/Method;",m7_child_class,m7_args,2,&m7_out)||!exception_named(v,"java/lang/NullPointerException"))return false;
 dvm_clear_exception(v);
 m7_args[1].l=m7_bad_element;m7_out.l=0;
 if(!reflection_runtime_call(v,"java/lang/Class","getMethod","(Ljava/lang/String;[Ljava/lang/Class;)Ljava/lang/reflect/Method;",m7_child_class,m7_args,2,&m7_out)||!exception_named(v,"java/lang/IllegalArgumentException"))return false;
 dvm_clear_exception(v);
 /* A non-String name is rejected instead of being read as a string. */
 m7_args[0].l=dvm_new_object(v,object_type);m7_args[1].l=m7_params0.l;m7_out.l=0;
 if(!m7_args[0].l||!reflection_runtime_call(v,"java/lang/Class","getMethod","(Ljava/lang/String;[Ljava/lang/Class;)Ljava/lang/reflect/Method;",m7_child_class,m7_args,2,&m7_out)||!exception_named(v,"java/lang/IllegalArgumentException"))return false;
 dvm_clear_exception(v);
 /* The interface's own public method is a getMethod result and its invoke
  * reads the annotation element through the real builtin. */
 m7_args[0].l=dvm_new_string(v,"name");m7_args[1].l=m7_params0.l;m7_out.l=0;
 if(!reflection_runtime_call(v,"java/lang/Class","getMethod","(Ljava/lang/String;[Ljava/lang/Class;)Ljava/lang/reflect/Method;",annot_type_class,m7_args,2,&m7_out)||dvm_exception(v)||!m7_out.l)return false;
 dvm_ref m7_annotation_method=m7_out.l;
 annot_arg.l=annot_type_class;m7_out.l=0;
 if(!reflection_runtime_call(v,"java/lang/reflect/Method","getAnnotation","(Ljava/lang/Class;)Ljava/lang/annotation/Annotation;",annot_method,&annot_arg,1,&m7_out)||dvm_exception(v)||!m7_out.l)return false;
 m7_invoke[0].l=m7_out.l;m7_invoke[1].l=annot_empty;m7_out.l=0;
 if(!reflection_runtime_call(v,"java/lang/reflect/Method","invoke","(Ljava/lang/Object;[Ljava/lang/Object;)Ljava/lang/Object;",m7_annotation_method,m7_invoke,2,&m7_out)||dvm_exception(v)||!m7_out.l||strcmp(({const dvm_ref gc_payload_ref_69=(m7_out.l);if(!gc_operation_admit(v,gc_payload_ref_69))return 0;dvm_string_utf8(v,gc_payload_ref_69);}),"method_value"))return false;
 /* The special Editor.apply Method still answers getName/getParameterTypes;
  * existing invoke semantics are asserted above. */
 m7_text.l=0;
 if(!reflection_runtime_call(v,"java/lang/reflect/Method","getName","()Ljava/lang/String;",method,NULL,0,&m7_text)||dvm_exception(v)||!m7_text.l||strcmp(({const dvm_ref gc_payload_ref_70=(m7_text.l);if(!gc_operation_admit(v,gc_payload_ref_70))return 0;dvm_string_utf8(v,gc_payload_ref_70);}),"apply"))return false;
 m7_text.l=0;
 if(!reflection_runtime_call(v,"java/lang/reflect/Method","getParameterTypes","()[Ljava/lang/Class;",method,NULL,0,&m7_text)||dvm_exception(v)||!m7_text.l||dvm_array_length(v,m7_text.l)!=0)return false;
 /* External classes have no member metadata: fail closed, do not return null
  * or claim the method is absent. */
 struct dvm_class *m7_external=dvm_find_class_or_external(v,"Lsdktrace/tests/ReflectExternalGetMethod;");
 dvm_ref m7_external_class=m7_external?dvm_class_object(v,m7_external):0;
 if(!m7_external||!m7_external->external||!m7_external_class)return false;
 m7_args[0].l=dvm_new_string(v,"anything");m7_args[1].l=m7_params0.l;m7_out.l=0;
 if(!reflection_runtime_call(v,"java/lang/Class","getMethod","(Ljava/lang/String;[Ljava/lang/Class;)Ljava/lang/reflect/Method;",m7_external_class,m7_args,2,&m7_out)||!exception_named(v,"java/lang/UnsupportedOperationException"))return false;
 dvm_clear_exception(v);
 /* A class whose member table claims methods but has no table is malformed
  * metadata, not an empty class: report InternalError, never NoSuchMethod.
  * Run the normal probe and a forced-failure probe; both must restore the
  * table before returning, so dvm_destroy() cannot dereference NULL methods
  * even when the selftest assertion fails. */
 bool malformed_cleanup=false,malformed_forced_cleanup=false;
 bool m7_malformed_ok=malformed_method_probe(v,"Lsdktrace/tests/ReflectMalformedGetMethod;",false,&malformed_cleanup);
 bool malformed_forced=malformed_method_probe(v,"Lsdktrace/tests/ReflectMalformedFailurePath;",true,&malformed_forced_cleanup);
 (void)malformed_forced; /* false by design: exercise the failure exit */
 if(!m7_malformed_ok||!malformed_cleanup||!malformed_forced_cleanup)return false;
 /* A different signature is not this hook's business: return false without
  * setting an exception, exactly like the other unclaimed entry points. */
 m7_args[0].l=dvm_new_string(v,"plainValue");m7_args[1].l=m7_params0.l;m7_out.l=0;
 if(reflection_runtime_call(v,"java/lang/Class","getMethod","(Ljava/lang/String;)Ljava/lang/reflect/Method;",annot_owner_class,m7_args,1,&m7_out))return false;
 if(dvm_exception(v))return false;
 /* A bare Method, an unknown signature and a bad receiver fail closed. */
 dvm_ref annot_bare=dvm_new_object(v,dvm_find_class_or_external(v,"java/lang/reflect/Method"));
 if(!annot_bare)return false;
 annot_invoke[1].l=annot_empty;
 if(!reflection_runtime_call(v,"java/lang/reflect/Method","invoke","(Ljava/lang/Object;[Ljava/lang/Object;)Ljava/lang/Object;",annot_bare,annot_invoke,2,&annot_out)||!exception_named(v,"java/lang/IllegalArgumentException"))return false;
 dvm_clear_exception(v);
 if(!reflection_runtime_call(v,"java/lang/reflect/Method","setAccessible","(Z)V",annot_bare,&annot_accessible,1,&annot_ignored)||!exception_named(v,"java/lang/IllegalArgumentException"))return false;
 dvm_clear_exception(v);
 if(reflection_runtime_call(v,"java/lang/Class","getDeclaredMethods","()[Ljava/lang/reflect/Field;",annot_owner_class,NULL,0,&annot_out))return false;
 if(reflection_runtime_call(v,"java/lang/reflect/Method","isAnnotationPresent","(Ljava/lang/Class;)I",annot_method,&annot_arg,1,&annot_present))return false;
 if(reflection_runtime_call(v,"java/lang/reflect/Field","isAnnotationPresent","(Ljava/lang/Class;)I",annot_tagged,&annot_arg,1,&annot_field_present))return false;
 if(reflection_runtime_call(v,"java/lang/reflect/Method","getReturnType","()I",annot_method,NULL,0,&annot_return))return false;
 /* Arrays declare no methods; external classes fail closed; constructors
  * are not methods, so a class with only <init> yields an empty array. */
 union dvm_value annot_methods_out={0};
 if(!reflection_runtime_call(v,"java/lang/Class","getDeclaredMethods","()[Ljava/lang/reflect/Method;",int_array_class,NULL,0,&annot_methods_out)||dvm_exception(v)||!annot_methods_out.l||dvm_array_length(v,annot_methods_out.l)!=0)return false;
 if(!reflection_runtime_call(v,"java/lang/Class","getDeclaredMethods","()[Ljava/lang/reflect/Method;",fixture_class,NULL,0,&annot_methods_out)||dvm_exception(v)||!annot_methods_out.l||dvm_array_length(v,annot_methods_out.l)!=0)return false;
 struct dvm_class *annot_external=dvm_find_class_or_external(v,"Lsdktrace/tests/ReflectExternalMethods;");
 dvm_ref annot_external_class=annot_external?dvm_class_object(v,annot_external):0;
 if(!annot_external||!annot_external->external||!annot_external_class)return false;
 if(!reflection_runtime_call(v,"java/lang/Class","getDeclaredMethods","()[Ljava/lang/reflect/Method;",annot_external_class,NULL,0,&annot_methods_out)||!exception_named(v,"java/lang/UnsupportedOperationException"))return false;
 dvm_clear_exception(v);
 if(!reflection_runtime_call(v,"java/lang/Class","getDeclaredMethods","()[Ljava/lang/reflect/Method;",0,NULL,0,&annot_methods_out)||!exception_named(v,"java/lang/NullPointerException"))return false;
 dvm_clear_exception(v);
 /* Class.getPackage()/Package.getName(): the DEX callers immediately call
  * getName(), so a Package must carry the real dotted package name.  A
  * primitive, a primitive array and a default-package class yield null,
  * exactly as Android does; no Package object is fabricated for them. */
 struct dvm_class *pkg_probe=dvm__register_builtin(v,"Lcom/example/PkgProbe;");
 struct dvm_class *pkg_array=dvm__class_by_desc(v,"[Lcom/example/PkgProbe;");
 struct dvm_class *pkg_default=dvm__register_builtin(v,"LDefaultPkgProbe;");
 struct dvm_class *pkg_primitive=dvm__define_primitive(v,"I");
 struct dvm_class *pkg_prim_array=dvm__class_by_desc(v,"[I");
 if(!pkg_probe||!pkg_array||!pkg_default||!pkg_primitive||!pkg_prim_array)return false;
 union dvm_value pkg_out={0},pkg_name={0};
#define PKG_CALL(receiver,expected) do { \
 pkg_out.l=0; \
 if(!reflection_runtime_call(v,"java/lang/Class","getPackage","()Ljava/lang/Package;",(receiver),NULL,0,&pkg_out)||dvm_exception(v))return false; \
 if((expected)==NULL){if(pkg_out.l)return false;} \
 else { \
  if(!pkg_out.l)return false; \
  const dvm_ref gc_ref_pkg_object_91=(pkg_out.l); if(!gc_operation_admit(v,gc_ref_pkg_object_91))return false; \
  if(!dvm__obj(v,gc_ref_pkg_object_91)||dvm__obj(v,gc_ref_pkg_object_91)->data_destroy!=package_box_destroy)return false; \
  pkg_name.l=0; \
  if(!reflection_runtime_call(v,"java/lang/Package","getName","()Ljava/lang/String;",pkg_out.l,NULL,0,&pkg_name)||dvm_exception(v)||!pkg_name.l||strcmp(({const dvm_ref gc_payload_ref_71=(pkg_name.l);if(!gc_operation_admit(v,gc_payload_ref_71))return 0;dvm_string_utf8(v,gc_payload_ref_71);}),(expected)))return false; \
 } \
} while(0)
 PKG_CALL(dvm_class_object(v,pkg_probe),"com.example");
 PKG_CALL(dvm_class_object(v,pkg_array),"com.example");
 PKG_CALL(dvm_class_object(v,pkg_primitive),NULL);
 PKG_CALL(dvm_class_object(v,pkg_prim_array),NULL);
 PKG_CALL(dvm_class_object(v,pkg_default),NULL);
#undef PKG_CALL
 /* The builtin method table path must reach the same implementation. */
 struct dvm_method *pkg_method=dvm_find_method(v,package_class,"getName","()Ljava/lang/String;");
 if(!pkg_method||!pkg_method->builtin)return false;
 pkg_out.l=0;
 if(!reflection_runtime_call(v,"java/lang/Class","getPackage","()Ljava/lang/Package;",dvm_class_object(v,pkg_probe),NULL,0,&pkg_out)||!pkg_out.l)return false;
 pkg_name.l=0;
 if(!dvm_call(v,pkg_method,pkg_out.l,NULL,0,&pkg_name)||dvm_exception(v)||!pkg_name.l||strcmp(({const dvm_ref gc_payload_ref_72=(pkg_name.l);if(!gc_operation_admit(v,gc_payload_ref_72))return 0;dvm_string_utf8(v,gc_payload_ref_72);}),"com.example"))return false;
 if(!reflection_runtime_call(v,"java/lang/Class","getPackage","()Ljava/lang/Package;",0,NULL,0,&pkg_out)||!exception_named(v,"java/lang/NullPointerException"))return false;
 dvm_clear_exception(v);
 dvm_ref foreign_pkg=dvm_new_string(v,"not-a-package");
 if(!reflection_runtime_call(v,"java/lang/Package","getName","()Ljava/lang/String;",foreign_pkg,NULL,0,&pkg_name)||!exception_named(v,"java/lang/IllegalStateException"))return false;
 dvm_clear_exception(v);
 sdk_trace_fprintf(stderr,"REFLECTION_SELFTEST PASS apply_lookup=1 invoke_persistent_write=1 missing_method=1 suppressed=1 declared_invoke=1 constructor=1 ctor_null_ref=1 ctor_type=1 ctor_primitive=1 ctor_widening=1 ctor_narrowing=1 ctor_wrap=1 ctor_abstract=1 ctor_private=1 assignable_from=1 declared_fields=1 field_metadata=1 field_get=1 field_static=1 field_accessible=1 field_errors=1 field_external=1 field_array=1 field_signature=1 declared_methods=1 method_metadata=1 field_annotation=1 method_annotation=1 annotation_element=1 annotation_errors=1 method_accessible=1 method_return_type=1 method_invoke=1 method_errors=1 get_method=1 declared_method_lookup=1 class_is_array=1 method_inheritance=1 method_interface=1 method_params=1 method_nulls=1 method_external=1 method_name_types=1 method_malformed=1 method_malformed_failure_cleanup=1 class_package=1 package_errors=1\n");
 return method!=0;
}
bool reflection_runtime_selftest(struct dvm *v){
 struct gc_operation operation __attribute__((cleanup(gc_operation_end)));
 if(!gc_operation_begin(v,&operation))return false;
 bool result=reflection_runtime_selftest_operation(v);
 return result;
}


/* Concrete destructor discriminators distinguish metadata from guest edges. */
int reflection_gc_payload(struct dvm *v,struct dvm_object *o,dvm_gc_visit_fn visit,void *user){
 struct gc_operation_pin gc_pin_o __attribute__((cleanup(gc_operation_pin_end)));
 if(!gc_operation_pin_begin(v,&gc_pin_o,o))return DVM_GC_PAYLOAD_INVALID;
 o=dvm__obj(v,gc_pin_o.ref);

 (void)v;
 if(o->data_destroy==reflected_free||o->data_destroy==variable_free||o->data_destroy==package_box_destroy)return DVM_GC_PAYLOAD_ATOMIC;
 if(o->data_destroy==generic_free){generic_type *p=o->data;if(visit){visit(&p->raw,user);visit(&p->owner,user);visit(&p->args,user);}return DVM_GC_PAYLOAD_REFS;}
 return DVM_GC_PAYLOAD_UNKNOWN;
}

/* CLICK-UP HAP-only test: original Unity ReflectionHelper sees real Class metadata. */
static bool click_up_class_selftest_operation(struct dvm *v){
 struct dvm_class *k=dvm_find_class_or_external(v,"java/lang/Class");
 struct dvm_class *h=dvm_find_class(v,"com/unity3d/player/ReflectionHelper");
 struct dvm_method *lookup=h?dvm_find_method(v,h,"getMethodID","(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/String;Z)Ljava/lang/reflect/Method;"):NULL;
 dvm_ref cls=k?dvm_class_object(v,k):0;union dvm_value list={0},result={0};
 bool enumerated=cls&&reflection_runtime_call(v,"java/lang/Class","getDeclaredMethods","()[Ljava/lang/reflect/Method;",cls,NULL,0,&list)&&!dvm_exception(v)&&list.l&&dvm_array_length(v,list.l)==(uint32_t)k->nmethods;
 union dvm_value a[4]={{.l=cls},{.l=dvm_new_string(v,"getName")},{.l=dvm_new_string(v,"()Ljava/lang/String;")},{.i=0}};
 bool found=enumerated&&lookup&&dvm_call(v,lookup,0,a,4,&result)&&!dvm_exception(v)&&result.l;
 struct dvm_method *actual=NULL;bool exact=found&&reflection_runtime_method_handle(v,result.l,&actual)&&actual&&actual->cls==k&&!strcmp(actual->name,"getName")&&!strcmp(actual->sig,"()Ljava/lang/String;");
 union dvm_value name={0};bool invoked=exact&&dvm_call(v,actual,cls,NULL,0,&name)&&!dvm_exception(v)&&name.l&&!strcmp(({const dvm_ref gc_payload_ref_73=(name.l);if(!gc_operation_admit(v,gc_payload_ref_73))return 0;dvm_string_utf8(v,gc_payload_ref_73);}),"java.lang.Class");
 FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/click-up-selftest.txt","a");
 if(f){dprintf(fileno(f),"pid=%d Class_declarations=%d Unity_ReflectionHelper=%d exact_method=%d real_invoke=%d result=%s\n",getpid(),enumerated,found,exact,invoked,enumerated&&found&&exact&&invoked?"PASS":"FAIL");fclose(f);}
 return enumerated&&found&&exact&&invoked;
}
bool click_up_class_selftest(struct dvm *v){
 struct gc_operation operation __attribute__((cleanup(gc_operation_end)));
 if(!gc_operation_begin(v,&operation))return false;
 bool result=click_up_class_selftest_operation(v);
 return result;
}


/* HAP-only: actual DEX ReflectionHelper lookup and actual VM dispatch. */
static bool jni_gaps_selftest_operation(struct dvm *v){
 bool ok=false;unsigned checks=0;const char *stage="setup";
 FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/jni-gaps-selftest.txt","a");if(!f)return false;
#define JG_CHECK(label,expr) do{stage=label;if(!(expr)||dvm_exception(v))goto done;checks++;fprintf(f,"pid=%d PASS %s\n",getpid(),label);}while(0)
 struct dvm_class *sys=dvm_find_class_or_external(v,"java/lang/System"),*obj=dvm_find_class_or_external(v,"java/lang/Object");
 struct dvm_class *helper=dvm_find_class(v,"com/unity3d/player/ReflectionHelper");
 struct dvm_method *lookup=helper?dvm_find_method(v,helper,"getMethodID","(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/String;Z)Ljava/lang/reflect/Method;"):NULL;
 struct dvm_method *identity=sys?dvm_find_method(v,sys,"identityHashCode","(Ljava/lang/Object;)I"):NULL;
 JG_CHECK("identity_declaration",identity&&dvm_method_is_static(identity));
 union dvm_value a={0},out={0};
 JG_CHECK("null_zero",dvm_call(v,identity,0,&a,1,&out)&&out.i==0);
 dvm_ref refs[]={dvm_new_object(v,obj),dvm_new_object(v,obj),dvm_new_array(v,'I',"I",3),dvm_new_string(v,"identity"),dvm_new_string(v,"identity")};
 int32_t hashes[5];
 for(unsigned i=0;i<5;i++){
  a.l=refs[i];JG_CHECK("non_null_fixture",refs[i]);
  JG_CHECK("actual_identity_call",dvm_call(v,identity,0,&a,1,&out));hashes[i]=out.i;
  JG_CHECK("same_object_repeat",dvm_call(v,identity,0,&a,1,&out)&&out.i==hashes[i]);
  for(unsigned j=0;j<i;j++)JG_CHECK("distinct_fixture_identity",hashes[i]!=hashes[j]);
 }
 struct dvm_method *hash=dvm_find_method(v,obj,"hashCode","()I");
 JG_CHECK("default_Object_hash_agrees",hash&&dvm_call(v,hash,refs[0],NULL,0,&out)&&out.i==hashes[0]);
 union dvm_value methods={0};dvm_ref hc=dvm_class_object(v,helper);
 JG_CHECK("DEX_declared_methods",helper&&!helper->external&&reflection_runtime_call(v,"java/lang/Class","getDeclaredMethods","()[Ljava/lang/reflect/Method;",hc,NULL,0,&methods)&&methods.l&&dvm_array_length(v,methods.l)>0);
 bool named=false;dvm_ref *items=({const dvm_ref gc_payload_ref_74=(methods.l);if(!gc_operation_admit(v,gc_payload_ref_74))return 0;dvm_array_data(v,gc_payload_ref_74);});
 for(uint32_t i=0;i<dvm_array_length(v,methods.l);i++){struct dvm_method *m=NULL;if(reflection_runtime_method_handle(v,items[i],&m)&&m&&!strcmp(m->name,"getMethodID")&&m->cls==helper)named=true;}
 JG_CHECK("DEX_method_name_and_owner",named);
 dvm_ref sc=dvm_class_object(v,sys);
 JG_CHECK("System_declared_methods",reflection_runtime_call(v,"java/lang/Class","getDeclaredMethods","()[Ljava/lang/reflect/Method;",sc,NULL,0,&methods)&&methods.l&&dvm_array_length(v,methods.l)==9);
 union dvm_value args[4]={{.l=sc},{.l=dvm_new_string(v,"identityHashCode")},{.l=dvm_new_string(v,"(Ljava/lang/Object;)I")},{.i=1}},result={0};
 JG_CHECK("original_ReflectionHelper_System",lookup&&dvm_call(v,lookup,0,args,4,&result)&&result.l);
 struct dvm_method *actual=NULL;
 JG_CHECK("exact_System_method",reflection_runtime_method_handle(v,result.l,&actual)&&actual==identity);
 struct dvm_class *qk=dvm_find_class_or_external(v,"java/util/concurrent/LinkedTransferQueue");
 dvm_ref qr=dvm_new_object(v,qk);const char *qm[]={"<init>","add","take","size","isEmpty"},*qs[]={"()V","(Ljava/lang/Object;)Z","()Ljava/lang/Object;","()I","()Z"};struct dvm_method *qms[5]={0};
 for(unsigned i=0;i<5;i++)JG_CHECK("notification_queue_method",(qms[i]=dvm_find_method(v,qk,qm[i],qs[i]))!=NULL);
 JG_CHECK("notification_queue_ctor",qr&&dvm_call(v,qms[0],qr,NULL,0,&out));
 JG_CHECK("notification_queue_empty",dvm_call(v,qms[4],qr,NULL,0,&out)&&out.i==1);
 a.l=refs[0];JG_CHECK("notification_queue_add_first",dvm_call(v,qms[1],qr,&a,1,&out)&&out.i==1);
 a.l=refs[1];JG_CHECK("notification_queue_add_second",dvm_call(v,qms[1],qr,&a,1,&out)&&out.i==1);
 JG_CHECK("notification_queue_size_two",dvm_call(v,qms[3],qr,NULL,0,&out)&&out.i==2);
 JG_CHECK("notification_queue_FIFO_first",dvm_call(v,qms[2],qr,NULL,0,&out)&&out.l==refs[0]);
 JG_CHECK("notification_queue_FIFO_second",dvm_call(v,qms[2],qr,NULL,0,&out)&&out.l==refs[1]);
 JG_CHECK("notification_queue_drained",dvm_call(v,qms[4],qr,NULL,0,&out)&&out.i==1);
 a.l=0;bool rejected=dvm_call(v,qms[1],qr,&a,1,&out);dvm_ref ex=dvm_exception(v);struct dvm_object *eo=ex?dvm__obj(v,ex):NULL;
 bool null_rejected=eo&&eo->cls&&!strcmp(eo->cls->name,"java/lang/NullPointerException");(void)rejected;
 if(null_rejected)dvm_clear_exception(v);
 JG_CHECK("notification_queue_rejects_null",null_rejected);
 struct dvm_class *rk=dvm_find_class_or_external(v,"java/util/Random");dvm_ref rr=dvm_new_object(v,rk);
 struct dvm_method *rc=dvm_find_method(v,rk,"<init>","(J)V"),*rn=dvm_find_method(v,rk,"nextInt","()I"),*rb=dvm_find_method(v,rk,"nextInt","(I)I"),*rs=dvm_find_method(v,rk,"setSeed","(J)V"),*rd=dvm_find_method(v,rk,"<init>","()V");
 a.j=0;JG_CHECK("Random_seeded_ctor",rr&&rc&&rn&&rb&&rs&&rd&&dvm_call(v,rc,rr,&a,1,&out));
 JG_CHECK("Random_seed0_first",dvm_call(v,rn,rr,NULL,0,&out)&&out.i==-1155484576);
 JG_CHECK("Random_seed0_second",dvm_call(v,rn,rr,NULL,0,&out)&&out.i==-723955400);
 a.j=0;JG_CHECK("Random_setSeed",dvm_call(v,rs,rr,&a,1,&out));a.i=100;
 JG_CHECK("Random_bound100_seed0",dvm_call(v,rb,rr,&a,1,&out)&&out.i==60);a.i=1;
 JG_CHECK("Random_bound_one",dvm_call(v,rb,rr,&a,1,&out)&&out.i==0);
 dvm_ref random_default=dvm_new_object(v,rk);JG_CHECK("Random_entropy_ctor",random_default&&dvm_call(v,rd,random_default,NULL,0,&out));
 a.i=INT32_MAX;JG_CHECK("Random_large_bound",dvm_call(v,rb,random_default,&a,1,&out)&&out.i>=0&&out.i<INT32_MAX);
 a.i=0;dvm_call(v,rb,rr,&a,1,&out);ex=dvm_exception(v);eo=ex?dvm__obj(v,ex):NULL;bool bad_bound=eo&&eo->cls&&!strcmp(eo->cls->name,"java/lang/IllegalArgumentException");if(bad_bound)dvm_clear_exception(v);JG_CHECK("Random_invalid_bound",bad_bound);
 struct dvm_class *receiver=dvm_find_class_or_external(v,"android/content/BroadcastReceiver");
 JG_CHECK("BroadcastReceiver_real_fields",receiver&&receiver->nifields==2&&receiver->islots>=2&&reflection_runtime_call(v,"java/lang/Class","getDeclaredFields","()[Ljava/lang/reflect/Field;",dvm_class_object(v,receiver),NULL,0,&methods)&&dvm_array_length(v,methods.l)==2);
 struct dvm_class *manager=dvm_find_class(v,"com/unity/androidnotifications/UnityNotificationManager");
 struct dvm_method *field_lookup=dvm_find_method(v,helper,"getFieldID","(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/String;Z)Ljava/lang/reflect/Field;");
 args[0].l=dvm_class_object(v,manager);args[1].l=dvm_new_string(v,"KEY_FIRE_TIME");args[2].l=dvm_new_string(v,"Ljava/lang/Object;");args[3].i=1;
 JG_CHECK("original_ReflectionHelper_field_covariance",field_lookup&&manager&&dvm_call(v,field_lookup,0,args,4,&result)&&result.l);
 struct dvm_field *actual_field=NULL;struct dvm_class *actual_owner=NULL;bool static_field=false;
 JG_CHECK("exact_original_String_field",reflection_runtime_field_handle(v,result.l,&actual_field,&actual_owner,&static_field)&&static_field&&actual_owner==manager&&actual_field&&!strcmp(actual_field->type,"Ljava/lang/String;")&&!strcmp(actual_field->name,"KEY_FIRE_TIME"));
 struct dvm_class *nb=dvm_find_class_or_external(v,"android/app/Notification$Builder");
 dvm_ref nbref=nb?dvm_new_object(v,nb):0,ctx=dvm_new_object(v,dvm_find_class_or_external(v,"android/content/Context"));
 struct dvm_method *nbctor=nb?dvm_find_method(v,nb,"<init>","(Landroid/content/Context;Ljava/lang/String;)V"):NULL;
 args[0].l=ctx;args[1].l=dvm_new_string(v,"selftest-channel");
 JG_CHECK("Notification_builder_ctor",nbref&&nbctor&&dvm_call(v,nbctor,nbref,args,2,&out));
 struct dvm_method *extras_get=dvm_find_method(v,nb,"getExtras","()Landroid/os/Bundle;");
 JG_CHECK("Notification_real_extras",extras_get&&dvm_call(v,extras_get,nbref,NULL,0,&out)&&out.l);
 dvm_ref extras=out.l;args[0].l=dvm_new_string(v,"test");args[1].i=17;
 struct dvm_class *bundle=dvm_find_class_or_external(v,"android/os/Bundle");struct dvm_method *put=dvm_find_method(v,bundle,"putInt","(Ljava/lang/String;I)V"),*get=dvm_find_method(v,bundle,"getInt","(Ljava/lang/String;)I");
 JG_CHECK("Notification_extras_write",put&&get&&dvm_call(v,put,extras,args,2,&out));
 struct dvm_method *color=dvm_find_method(v,nb,"setColor","(I)Landroid/app/Notification$Builder;"),*build=dvm_find_method(v,nb,"build","()Landroid/app/Notification;");args[0].i=0x123456;
 JG_CHECK("Notification_fluent_setter",color&&build&&dvm_call(v,color,nbref,args,1,&out)&&out.l==nbref);
 JG_CHECK("Notification_build_value",dvm_call(v,build,nbref,NULL,0,&out)&&out.l);dvm_ref nv=out.l;
 JG_CHECK("Notification_color_retained",dvm_get_field(v,nv,"color","I",&out)&&out.i==0x123456);
 JG_CHECK("Notification_extras_snapshot",dvm_get_field(v,nv,"extras","Landroid/os/Bundle;",&out)&&out.l&&out.l!=extras);dvm_ref nex=out.l;
 args[0].l=dvm_new_string(v,"test");JG_CHECK("Notification_snapshot_content",dvm_call(v,get,nex,args,1,&out)&&out.i==17);
 args[1].i=23;JG_CHECK("Notification_builder_mutation",dvm_call(v,put,extras,args,2,&out));
 JG_CHECK("Notification_snapshot_isolation",dvm_call(v,get,nex,args,1,&out)&&out.i==17);
 struct dvm_class *nk=dvm_find_class_or_external(v,"android/app/Notification");struct dvm_method *channel=dvm_find_method(v,nk,"getChannelId","()Ljava/lang/String;");
 JG_CHECK("Notification_channel_retained",channel&&dvm_call(v,channel,nv,NULL,0,&out)&&out.l&&({const dvm_ref gc_payload_ref_75=(out.l);if(!gc_operation_admit(v,gc_payload_ref_75))return 0;dvm_string_utf8(v,gc_payload_ref_75);})&&!strcmp(({const dvm_ref gc_payload_ref_76=(out.l);if(!gc_operation_admit(v,gc_payload_ref_76))return 0;dvm_string_utf8(v,gc_payload_ref_76);}),"selftest-channel"));
 args[0].l=dvm_class_object(v,nk);args[1].l=dvm_new_string(v,"EXTRA_TITLE");args[2].l=dvm_new_string(v,"Ljava/lang/Object;");args[3].i=1;
 JG_CHECK("Notification_original_ReflectionHelper_constant",dvm_call(v,field_lookup,0,args,4,&result)&&result.l);
 JG_CHECK("Notification_constant_String_covariance",reflection_runtime_field_handle(v,result.l,&actual_field,&actual_owner,&static_field)&&static_field&&actual_owner==nk&&actual_field&&!strcmp(actual_field->type,"Ljava/lang/String;"));
 JG_CHECK("Notification_constant_value",dvm_get_static(v,nk,"EXTRA_TITLE","Ljava/lang/String;",&out)&&out.l&&!strcmp(({const dvm_ref gc_payload_ref_77=(out.l);if(!gc_operation_admit(v,gc_payload_ref_77))return 0;dvm_string_utf8(v,gc_payload_ref_77);}),"android.title"));
 JG_CHECK("Notification_FLAG_AUTO_CANCEL",dvm_get_static(v,nk,"FLAG_AUTO_CANCEL","I",&out)&&out.i==16);
 JG_CHECK("Notification_DEFAULT_ALL",dvm_get_static(v,nk,"DEFAULT_ALL","I",&out)&&out.i==-1);
 args[0].l=dvm_class_object(v,nk);args[1].l=dvm_new_string(v,"flags");args[2].l=dvm_new_string(v,"I");args[3].i=0;
 JG_CHECK("Notification_flags_JNI_metadata",dvm_call(v,field_lookup,0,args,4,&result)&&result.l&&reflection_runtime_field_handle(v,result.l,&actual_field,&actual_owner,&static_field)&&!static_field&&actual_owner==nk&&(actual_field->access&DEX_ACC_PUBLIC));
 JG_CHECK("Notification_flags_default_0",dvm_get_field(v,nv,"flags","I",&out)&&out.i==0);
 struct dvm_method *auto_cancel=dvm_find_method(v,nb,"setAutoCancel","(Z)Landroid/app/Notification$Builder;"),*ongoing=dvm_find_method(v,nb,"setOngoing","(Z)Landroid/app/Notification$Builder;");
 args[0].i=1;JG_CHECK("Notification_auto_true",auto_cancel&&ongoing&&dvm_call(v,auto_cancel,nbref,args,1,&out)&&out.l==nbref);
 JG_CHECK("Notification_auto_build_16",dvm_call(v,build,nbref,NULL,0,&out)&&out.l);dvm_ref auto_value=out.l;
 JG_CHECK("Notification_auto_flags_16",dvm_get_field(v,auto_value,"flags","I",&out)&&out.i==16);
 JG_CHECK("Notification_ongoing_true",dvm_call(v,ongoing,nbref,args,1,&out)&&out.l==nbref);
 JG_CHECK("Notification_combined_build",dvm_call(v,build,nbref,NULL,0,&out)&&out.l);dvm_ref combined_value=out.l;
 JG_CHECK("Notification_combined_flags_18",dvm_get_field(v,combined_value,"flags","I",&out)&&out.i==18);
 args[0].i=0;JG_CHECK("Notification_auto_false",dvm_call(v,auto_cancel,nbref,args,1,&out)&&out.l==nbref);
 JG_CHECK("Notification_clear_auto_build",dvm_call(v,build,nbref,NULL,0,&out)&&out.l);dvm_ref ongoing_value=out.l;
 JG_CHECK("Notification_clear_auto_preserves_ongoing_2",dvm_get_field(v,ongoing_value,"flags","I",&out)&&out.i==2);
 JG_CHECK("Notification_build_flags_snapshot_18",dvm_get_field(v,combined_value,"flags","I",&out)&&out.i==18);
 JG_CHECK("Notification_ongoing_false",dvm_call(v,ongoing,nbref,args,1,&out)&&out.l==nbref);
 JG_CHECK("Notification_clear_ongoing_build",dvm_call(v,build,nbref,NULL,0,&out)&&out.l);dvm_ref clear_value=out.l;
 JG_CHECK("Notification_clear_all_0",dvm_get_field(v,clear_value,"flags","I",&out)&&out.i==0);
 JG_CHECK("Notification_flags_public_write_96",dvm_set_field(v,clear_value,"flags","I",(union dvm_value){.i=0x20|0x40})&&dvm_get_field(v,clear_value,"flags","I",&out)&&out.i==96);
 JG_CHECK("Notification_flags_public_clear_no_clear_64",dvm_set_field(v,clear_value,"flags","I",(union dvm_value){.i=out.i&~0x20})&&dvm_get_field(v,clear_value,"flags","I",&out)&&out.i==64);
 JG_CHECK("Notification_FLAG_ONGOING_EVENT_2",dvm_get_static(v,nk,"FLAG_ONGOING_EVENT","I",&out)&&out.i==2);
 JG_CHECK("Notification_FLAG_NO_CLEAR_32",dvm_get_static(v,nk,"FLAG_NO_CLEAR","I",&out)&&out.i==32);
 JG_CHECK("Notification_FLAG_FOREGROUND_SERVICE_64",dvm_get_static(v,nk,"FLAG_FOREGROUND_SERVICE","I",&out)&&out.i==64);
 struct dvm_method *style_set=dvm_find_method(v,nb,"setStyle","(Landroid/app/Notification$Style;)Landroid/app/Notification$Builder;");args[0].l=0;
 JG_CHECK("Notification_clear_style",style_set&&dvm_call(v,style_set,nbref,args,1,&out)&&out.l==nbref);
 JG_CHECK("Notification_style_null_retained",dvm_get_field(v,nbref,"style","Landroid/app/Notification$Style;",&out)&&out.l==0);
 struct dvm_method *bool_get=dvm_find_method(v,bundle,"getBoolean","(Ljava/lang/String;Z)Z"),*bool_put=dvm_find_method(v,bundle,"putBoolean","(Ljava/lang/String;Z)V");
 args[0].l=dvm_new_string(v,"notif-boolean-missing");args[1].i=1;
 JG_CHECK("Bundle_boolean_default_true_JNI",bool_get&&bool_put&&dvm_call(v,bool_get,extras,args,2,&out)&&out.i==1);
 args[1].i=0;JG_CHECK("Bundle_boolean_default_false_JNI",dvm_call(v,bool_get,extras,args,2,&out)&&out.i==0);
 args[1].i=1;JG_CHECK("Bundle_boolean_put_true_JNI",dvm_call(v,bool_put,extras,args,2,&out));args[1].i=0;
 JG_CHECK("Bundle_boolean_stored_beats_default_JNI",dvm_call(v,bool_get,extras,args,2,&out)&&out.i==1);
 JG_CHECK("Bundle_declared_methods_metadata",reflection_runtime_call(v,"java/lang/Class","getDeclaredMethods","()[Ljava/lang/reflect/Method;",dvm_class_object(v,bundle),NULL,0,&methods)&&methods.l&&dvm_array_length(v,methods.l)>20);

 struct dvm_class *jo=dvm_find_class_or_external(v,"org/json/JSONObject"),*ja=dvm_find_class_or_external(v,"org/json/JSONArray");
 dvm_ref jor=dvm_new_object(v,jo),jar=dvm_new_object(v,ja);
 struct dvm_method *jctor=dvm_find_method(v,jo,"<init>","(Ljava/lang/String;)V"),*jkeys=dvm_find_method(v,jo,"keys","()Ljava/util/Iterator;");
 args[0].l=dvm_new_string(v,"{\"first\":1,\"second\":2,\"first\":3}");
 JG_CHECK("JSONObject_ctor_JNI",jor&&jctor&&dvm_call(v,jctor,jor,args,1,&out));
 JG_CHECK("JSONObject_keys_JNI",jkeys&&dvm_call(v,jkeys,jor,NULL,0,&out)&&out.l);dvm_ref ji=out.l;
 struct dvm_class *jik=dvm__obj(v,ji)->cls;
 struct dvm_method *jhas=dvm_find_method(v,jik,"hasNext","()Z"),*jnext=dvm_find_method(v,jik,"next","()Ljava/lang/Object;");
 JG_CHECK("JSONObject_keys_first_available",jhas&&jnext&&dvm_call(v,jhas,ji,NULL,0,&out)&&out.i);
 JG_CHECK("JSONObject_keys_first",dvm_call(v,jnext,ji,NULL,0,&out)&&out.l&&!strcmp(({const dvm_ref gc_payload_ref_78=(out.l);if(!gc_operation_admit(v,gc_payload_ref_78))return 0;dvm_string_utf8(v,gc_payload_ref_78);}),"first"));
 JG_CHECK("JSONObject_keys_second",dvm_call(v,jnext,ji,NULL,0,&out)&&out.l&&!strcmp(({const dvm_ref gc_payload_ref_79=(out.l);if(!gc_operation_admit(v,gc_payload_ref_79))return 0;dvm_string_utf8(v,gc_payload_ref_79);}),"second"));
 JG_CHECK("JSONObject_keys_distinct_complete",dvm_call(v,jhas,ji,NULL,0,&out)&&!out.i);
 struct dvm_method *jact=dvm_find_method(v,ja,"<init>","()V"),*jalen=dvm_find_method(v,ja,"length","()I"),*jastr=dvm_find_method(v,ja,"toString","()Ljava/lang/String;"),*japut=dvm_find_method(v,ja,"put","(Ljava/lang/Object;)Lorg/json/JSONArray;");
 JG_CHECK("JSONArray_empty_ctor_JNI",jar&&jact&&jalen&&jastr&&japut&&dvm_call(v,jact,jar,NULL,0,&out));
 JG_CHECK("JSONArray_length_zero",dvm_call(v,jalen,jar,NULL,0,&out)&&out.i==0);
 JG_CHECK("JSONArray_empty_serialization",dvm_call(v,jastr,jar,NULL,0,&out)&&out.l&&!strcmp(({const dvm_ref gc_payload_ref_80=(out.l);if(!gc_operation_admit(v,gc_payload_ref_80))return 0;dvm_string_utf8(v,gc_payload_ref_80);}),"[]"));
 args[0].l=dvm_new_string(v,"value");JG_CHECK("JSONArray_append",dvm_call(v,japut,jar,args,1,&out)&&out.l==jar);
 args[0].l=0;JG_CHECK("JSONArray_append_null",dvm_call(v,japut,jar,args,1,&out)&&out.l==jar);
 JG_CHECK("JSONArray_length_two",dvm_call(v,jalen,jar,NULL,0,&out)&&out.i==2);
 JG_CHECK("JSONArray_serialized_values",dvm_call(v,jastr,jar,NULL,0,&out)&&out.l&&!strcmp(({const dvm_ref gc_payload_ref_81=(out.l);if(!gc_operation_admit(v,gc_payload_ref_81))return 0;dvm_string_utf8(v,gc_payload_ref_81);}),"[\"value\",null]"));
 JG_CHECK("JSONObject_actual_declared_keys",reflection_runtime_call(v,"java/lang/Class","getDeclaredMethods","()[Ljava/lang/reflect/Method;",dvm_class_object(v,jo),NULL,0,&methods)&&dvm_array_length(v,methods.l)>0);
 bool found_keys=false,found_bool=false;
 for(int i=0;i<dvm_array_length(v,methods.l);i++){dvm_ref ref=((dvm_ref*)({const dvm_ref gc_payload_ref_82=(methods.l);if(!gc_operation_admit(v,gc_payload_ref_82))return 0;dvm_array_data(v,gc_payload_ref_82);}))[i];struct dvm_method *mm=NULL;if(reflection_runtime_method_handle(v,ref,&mm)&&mm&&!strcmp(mm->name,"keys"))found_keys=true;}
 JG_CHECK("JSONObject_reflected_keys_name",found_keys);
 JG_CHECK("Bundle_reflect_actual_methods",reflection_runtime_call(v,"java/lang/Class","getDeclaredMethods","()[Ljava/lang/reflect/Method;",dvm_class_object(v,bundle),NULL,0,&methods)&&methods.l);
 for(int i=0;i<dvm_array_length(v,methods.l);i++){dvm_ref ref=((dvm_ref*)({const dvm_ref gc_payload_ref_83=(methods.l);if(!gc_operation_admit(v,gc_payload_ref_83))return 0;dvm_array_data(v,gc_payload_ref_83);}))[i];struct dvm_method *mm=NULL;if(reflection_runtime_method_handle(v,ref,&mm)&&mm==bool_get)found_bool=true;}
 JG_CHECK("Bundle_reflected_getBoolean_name_signature",found_bool);
 struct dvm_class *setclass=dvm_find_class_or_external(v,"java/util/Set"),*hashset=dvm_find_class_or_external(v,"java/util/HashSet");
 JG_CHECK("Set_FindClass_inventory",dvm_class_exists(v,"java/util/Set"));
 JG_CHECK("Set_interface_assignability",setclass&&setclass->is_interface&&dvm__class_assignable(v,hashset,setclass));
 struct dvm_class *mk=dvm_find_class_or_external(v,"android/util/DisplayMetrics");dvm_ref mr=dvm_new_object(v,mk);
 struct dvm_method *mc=dvm_find_method(v,mk,"<init>","()V");
 JG_CHECK("DisplayMetrics_ctor_JNI",mr&&mc&&dvm_call(v,mc,mr,NULL,0,&out));
 JG_CHECK("DisplayMetrics_width_field",dvm_get_field(v,mr,"widthPixels","I",&out));int host_width=out.i;
 JG_CHECK("DisplayMetrics_height_field",dvm_get_field(v,mr,"heightPixels","I",&out));int host_height=out.i;
 JG_CHECK("DisplayMetrics_density_field",dvm_get_field(v,mr,"density","F",&out));float host_density=out.f;
 if(host_width==0||host_height==0){
  fprintf(f,"pid=%d SKIP DisplayMetrics_host_width_valid DisplayMetrics_host_height_valid DisplayMetrics_host_density_valid reason=display_snapshot_dimensions_unavailable source=%s\n",getpid(),sdk_display_source());
 }else{
  JG_CHECK("DisplayMetrics_host_width_valid",host_width>0);
  JG_CHECK("DisplayMetrics_host_height_valid",host_height>0);
  JG_CHECK("DisplayMetrics_host_density_valid",host_density>0);
 }
 fprintf(f,"pid=%d DISPLAY_METRICS width=%d height=%d density=%g source=%s\n",getpid(),host_width,host_height,(double)host_density,sdk_display_source());
 struct dvm_class *vk=dvm_find_class_or_external(v,"android/view/View");dvm_ref vr=dvm_new_object(v,vk);
 struct dvm_method *getdisplay=dvm_find_method(v,vk,"getDisplay","()Landroid/view/Display;");
 JG_CHECK("View_getDisplay_JNI",vr&&getdisplay&&dvm_call(v,getdisplay,vr,NULL,0,&out)&&out.l);dvm_ref dr=out.l;
 JG_CHECK("View_getDisplay_stable",dvm_call(v,getdisplay,vr,NULL,0,&out)&&out.l==dr);
 struct dvm_method *offset=dvm_find_method(v,dvm__obj(v,dr)->cls,"getAppVsyncOffsetNanos","()J");
 JG_CHECK("Display_vsync_zero_valid_JNI",offset&&dvm_call(v,offset,dr,NULL,0,&out)&&out.j==0);

 struct dvm_class *mapk=dvm_find_class_or_external(v,"java/util/HashMap");dvm_ref mapr=dvm_new_object(v,mapk);
 struct dvm_method *mapct=dvm_find_method(v,mapk,"<init>","()V"),*mapput=dvm_find_method(v,mapk,"put","(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;");
 JG_CHECK("JSON_map_fixture",mapr&&mapct&&mapput&&dvm_call(v,mapct,mapr,NULL,0,&out));
 args[0].l=dvm_new_string(v,"mapkey");args[1].l=dvm_new_string(v,"mapvalue");JG_CHECK("JSON_map_value",dvm_call(v,mapput,mapr,args,2,&out));
 args[0].l=dvm_new_string(v,"nil");args[1].l=0;JG_CHECK("JSON_map_null",dvm_call(v,mapput,mapr,args,2,&out));
 struct dvm_method *mapjson=dvm_find_method(v,jo,"<init>","(Ljava/util/Map;)V"),*jget=dvm_find_method(v,jo,"getString","(Ljava/lang/String;)Ljava/lang/String;");
 dvm_ref mapjo=dvm_new_object(v,jo);args[0].l=mapr;JG_CHECK("JSONObject_Map_ctor",mapjo&&mapjson&&dvm_call(v,mapjson,mapjo,args,1,&out));
 args[0].l=dvm_new_string(v,"mapkey");JG_CHECK("JSONObject_Map_copied_value",jget&&dvm_call(v,jget,mapjo,args,1,&out)&&out.l&&!strcmp(({const dvm_ref gc_payload_ref_84=(out.l);if(!gc_operation_admit(v,gc_payload_ref_84))return 0;dvm_string_utf8(v,gc_payload_ref_84);}),"mapvalue"));
 args[0].l=dvm_new_string(v,"nil");JG_CHECK("JSONObject_Map_null_is_json_null",dvm_call(v,jget,mapjo,args,1,&out)&&out.l&&!strcmp(({const dvm_ref gc_payload_ref_85=(out.l);if(!gc_operation_admit(v,gc_payload_ref_85))return 0;dvm_string_utf8(v,gc_payload_ref_85);}),"null"));
 struct dvm_method *setiter=dvm_find_method(v,setclass,"iterator","()Ljava/util/Iterator;");
 JG_CHECK("Set_iterator_JNI_declaration",setiter!=NULL);
 struct dvm_class *ctxklass=dvm_find_class_or_external(v,"android/content/Context");dvm_ref ctxr=dvm_new_object(v,ctxklass);
 struct dvm_method *ctxloader=dvm_find_method(v,ctxklass,"getClassLoader","()Ljava/lang/ClassLoader;");
 JG_CHECK("Context_getClassLoader_JNI",ctxr&&ctxloader&&dvm_call(v,ctxloader,ctxr,NULL,0,&out)&&out.l);dvm_ref clr=out.l;
 args[0].l=dvm_new_string(v,"com.unity3d.player.ReflectionHelper");struct dvm_method *loadclass=dvm_find_method(v,dvm__obj(v,clr)->cls,"loadClass","(Ljava/lang/String;)Ljava/lang/Class;");
 JG_CHECK("Context_loader_resolves_guest_class",loadclass&&dvm_call(v,loadclass,clr,args,1,&out)&&out.l==hc);
 extern bool sys_service_selftest(struct dvm *);
 JG_CHECK("SYS_SERVICE_HAP_selftest",sys_service_selftest(v));
 extern bool battery_runtime_query_selftest(struct dvm *);
 JG_CHECK("BATTERY_null_receiver_query_HAP",battery_runtime_query_selftest(v));
 ok=true;
done:
 fprintf(f,"pid=%d RESULT ok=%d checks=%u stage=%s moving_enabled=0 gc_stability=stable_reference_identity_not_body_address\n",getpid(),ok,checks,stage);fclose(f);
#undef JG_CHECK
 return ok;
}
bool jni_gaps_selftest(struct dvm *v){
 struct gc_operation operation __attribute__((cleanup(gc_operation_end)));
 if(!gc_operation_begin(v,&operation))return false;
 bool result=jni_gaps_selftest_operation(v);
 return result;
}

