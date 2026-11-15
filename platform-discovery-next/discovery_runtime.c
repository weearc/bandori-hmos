static void discovery_logger_destroy(void *);
#include "gc_runtime.h"
#include "discovery_runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <limits.h>
#include "package_metadata.h"
#include "android_basics.h"
extern int sdk_trace_fprintf(FILE *,const char *,...);
static struct dvm *active;
static dvm_ref levels[6], class_loader;
static bool discovery_gc_roots(struct dvm *v,dvm_gc_visit_fn visit,void *user,void *context){
 (void)context;if(!dvm_gil_held(v)||active!=v)return false;for(unsigned i=0;i<6;i++)if(levels[i])visit(&levels[i],user);if(class_loader)visit(&class_loader,user);return true;
}
static const char *level_names[]={"INFO","WARNING","FINE","SEVERE","OFF","ALL"};
static bool fail(struct dvm *v,const char *type,const char *message){dvm__throw(v,type,"%s",message);return true;}
static char *copy_string(const char *s,size_t n){char *p=malloc(n+1);if(p){memcpy(p,s,n);p[n]=0;}return p;}
static bool string_arg(struct dvm *v,dvm_ref ref,const char **s,size_t *n){struct dvm_object *o=dvm__obj(v,ref);if(!o||o->kind!=DVM_OBJ_STRING)return false;*s=dvm_string_utf8(v,ref);*n=dvm_string_utf8_length(v,ref);return *s!=NULL;}
static bool typed(struct dvm *v,dvm_ref ref,const char *name){struct dvm_object *o=dvm__obj(v,ref);return o&&o->cls&&o->cls->name&&!strcmp(o->cls->name,name);}
void discovery_runtime_vm_start(struct dvm *v){active=v;memset(levels,0,sizeof levels);class_loader=0;if(!dvm_gc_root_provider_add(v,"discovery/cache",DVM_GC_ROOT_STRONG,discovery_gc_roots,&active)){dvm__throw(v,"java/lang/OutOfMemoryError","discovery GC roots");return;}struct dvm_class *missing=dvm_find_class_or_external(v,"java/lang/ClassNotFoundException"),*base=dvm_find_class_or_external(v,"java/lang/Exception");if(missing&&missing->external)missing->super=base;}
void discovery_runtime_vm_end(struct dvm *v){if(v!=active)return;dvm_gc_root_provider_remove(v,"discovery/cache",&active);for(size_t i=0;i<sizeof(levels)/sizeof(levels[0]);i++)if(levels[i])dvm_unpin(v,levels[i]);if(class_loader)dvm_unpin(v,class_loader);memset(levels,0,sizeof levels);class_loader=0;active=NULL;}
bool discovery_runtime_static(struct dvm *v,const char *c,const char *field,const char *type,union dvm_value *out){
 if(v!=active||strcmp(c,"java/util/logging/Level")||strcmp(type,"Ljava/util/logging/Level;"))return false;
 for(size_t i=0;i<sizeof(level_names)/sizeof(level_names[0]);i++)if(!strcmp(field,level_names[i])){
  if(!levels[i]){dvm_ref r=dvm_new_object(v,dvm_find_class_or_external(v,c));if(!r){return fail(v,"java/lang/OutOfMemoryError","Level");}levels[i]=r;dvm_pin(v,r);}
  out->l=levels[i];return true;
 }
 return false;
}
/* Resolve both platform inventory names and real guest DEX classes. DEX
 * classes are initialized only when the Java overload requests it; the class
 * object is the same identity used by reflection and instance-of. */
static bool class_for_name(struct dvm *v,dvm_ref name_ref,int initialize,int bootstrap_only,union dvm_value *out){
 if(!name_ref)return fail(v,"java/lang/NullPointerException","Class.forName name");
 const char *name;size_t len;if(!string_arg(v,name_ref,&name,&len))return fail(v,"java/lang/IllegalArgumentException","Class.forName name");
 if(!len||memchr(name,0,len)||len>=512)return fail(v,"java/lang/ClassNotFoundException","invalid class name");
 char normalized[512];memcpy(normalized,name,len);normalized[len]=0;
 struct dvm_class *klass=dvm_find_class(v,normalized);
 if(klass){
  struct dvm_class *component=klass;while(component->is_array&&component->elem)component=component->elem;
  if(bootstrap_only&&component->dex)return fail(v,"java/lang/ClassNotFoundException",normalized);
  if(initialize&&!dvm_init_class(v,klass))return true;
  out->l=dvm_class_object(v,klass);
  return out->l!=0||fail(v,"java/lang/OutOfMemoryError","Class.forName");
 }
 if(!dvm_class_exists(v,normalized))return fail(v,"java/lang/ClassNotFoundException",normalized);
 klass=dvm_find_class_or_external(v,normalized);if(!klass)return fail(v,"java/lang/UnsupportedOperationException","Class.forName class loading");
 out->l=dvm_class_object(v,klass);return out->l!=0||fail(v,"java/lang/OutOfMemoryError","Class.forName");
}
bool discovery_runtime_call(struct dvm *v,const char *c,const char *m,const char *sig,dvm_ref self,const union dvm_value *a,int n,union dvm_value *out){
 if(v!=active)return false;
 if(!strcmp(c,"android/content/Context")&&!strcmp(m,"getClassLoader")&&!strcmp(sig,"()Ljava/lang/ClassLoader;")&&n==0){
  if(!self)return fail(v,"java/lang/NullPointerException","Context.getClassLoader");
  if(!class_loader){class_loader=dvm_new_object(v,dvm_find_class_or_external(v,"java/lang/ClassLoader"));if(!class_loader)return fail(v,"java/lang/OutOfMemoryError","ClassLoader");dvm_pin(v,class_loader);}
  out->l=class_loader;return true;
 }

 if(!strcmp(c,"java/lang/ClassLoader")&&!strcmp(m,"loadClass")&&!strcmp(sig,"(Ljava/lang/String;)Ljava/lang/Class;")&&n==1&&a){
  if(!self||self!=class_loader)return fail(v,"java/lang/UnsupportedOperationException","foreign ClassLoader.loadClass");
  /* Java loadClass resolves an identity; it does not run <clinit>. */
  return class_for_name(v,a[0].l,0,0,out);
 }
 if(!strcmp(c,"java/lang/ClassLoader")&&!strcmp(m,"findLibrary")&&!strcmp(sig,"(Ljava/lang/String;)Ljava/lang/String;")&&n==1&&a){
  if(!self||self!=class_loader)return fail(v,"java/lang/UnsupportedOperationException","foreign ClassLoader.findLibrary");
  if(!a[0].l)return fail(v,"java/lang/NullPointerException","library name");
  const char *name=NULL;size_t len=0;
  if(!string_arg(v,a[0].l,&name,&len)||memchr(name,0,len))return fail(v,"java/lang/IllegalArgumentException","library name");
  out->l=0;
  if(!len||strchr(name,'/')||strchr(name,'\\'))return true;
  union dvm_value info={0},directory={0};
  dvm_ref app=android_basics_application(v);
  if(!package_metadata_call(v,"android/content/Context","getApplicationInfo","()Landroid/content/pm/ApplicationInfo;",app,NULL,0,&info)||dvm_exception(v))return true;
  if(!info.l||!dvm_get_field(v,info.l,"nativeLibraryDir","Ljava/lang/String;",&directory)||!directory.l)return true;
  const char *dir=dvm_string_utf8(v,directory.l);char path[PATH_MAX];
  if(!dir)return fail(v,"java/lang/IllegalStateException","native library directory");
  int count=snprintf(path,sizeof path,"%s/lib%s.so",dir,name);
  if(count<0||(size_t)count>=sizeof path)return true;
  struct stat st;if(stat(path,&st)!=0||!S_ISREG(st.st_mode))return true;
  out->l=dvm_new_string(v,path);
  sdk_trace_fprintf(stderr,"JNI_FIND_LIBRARY name=%s exists=1 path=%s\n",name,path);
  return out->l!=0||fail(v,"java/lang/OutOfMemoryError","library path");
 }
 if(!strcmp(c,"java/lang/Class")&&!strcmp(m,"getName")&&!strcmp(sig,"()Ljava/lang/String;")&&n==0){
  struct dvm_object *o=dvm__obj(v,self);if(!o||o->kind!=DVM_OBJ_CLASS||!o->klass)return fail(v,"java/lang/IllegalArgumentException","Class.getName receiver");
  /* This dispatcher precedes reflection_runtime_call: primitive descriptors
   * must become Java names here too, while array descriptors remain encoded. */
  struct dvm_class *k=o->klass;
  static const struct {const char *desc,*name;} primitives[]={
   {"V","void"},{"Z","boolean"},{"B","byte"},{"C","char"},{"S","short"},
   {"I","int"},{"J","long"},{"F","float"},{"D","double"}};
  if(k->is_primitive&&k->desc)for(size_t i=0;i<sizeof primitives/sizeof primitives[0];i++)if(!strcmp(k->desc,primitives[i].desc)){
   out->l=dvm_new_string(v,primitives[i].name);return out->l!=0||fail(v,"java/lang/OutOfMemoryError","Class.getName primitive");
  }
  const char *raw=k->is_array&&k->desc?k->desc:k->name;if(!raw)return fail(v,"java/lang/InternalError","Class.getName metadata");size_t len=strlen(raw);char *name=copy_string(raw,len);if(!name)return fail(v,"java/lang/OutOfMemoryError","Class.getName");for(char *p=name;*p;p++)if(*p=='/')*p='.';out->l=dvm_new_string(v,name);free(name);return out->l!=0||fail(v,"java/lang/OutOfMemoryError","Class.getName string");
 }
 if(!strcmp(c,"java/lang/Class")&&!strcmp(m,"getSimpleName")&&!strcmp(sig,"()Ljava/lang/String;")&&n==0){
  struct dvm_object *o=dvm__obj(v,self);if(!o||o->kind!=DVM_OBJ_CLASS||!o->klass)return fail(v,"java/lang/IllegalArgumentException","Class.getSimpleName receiver");
  const char *raw=o->klass->name;if(raw&&raw[0]=='['){out->l=dvm_new_string(v,raw);return out->l!=0||fail(v,"java/lang/OutOfMemoryError","Class.getSimpleName");}
  const char *base=raw?strrchr(raw,'/'):NULL;base=base?base+1:(raw?raw:"");
  const char *dollar=strrchr(base,'$');
  if(dollar){if(dollar[1]>='0'&&dollar[1]<='9')base="";else base=dollar+1;}
  out->l=dvm_new_string(v,base);return out->l!=0||fail(v,"java/lang/OutOfMemoryError","Class.getSimpleName");
 }
 if(!strcmp(c,"java/lang/Class")&&!strcmp(m,"forName")&&(!strcmp(sig,"(Ljava/lang/String;)Ljava/lang/Class;")||!strcmp(sig,"(Ljava/lang/String;ZLjava/lang/ClassLoader;)Ljava/lang/Class;"))) {
  bool three=!strcmp(sig,"(Ljava/lang/String;ZLjava/lang/ClassLoader;)Ljava/lang/Class;");
  if((!three&&n!=1)||(three&&n!=3))return false;
  if(three&&a[2].l&&a[2].l!=class_loader)
   return fail(v,"java/lang/UnsupportedOperationException","Class.forName custom ClassLoader");
  return class_for_name(v,a[0].l,three?a[1].i!=0:1,three&&!a[2].l,out);
 }
 if(!strcmp(c,"java/lang/Class")&&!strcmp(m,"getClassLoader")&&!strcmp(sig,"()Ljava/lang/ClassLoader;")&&n==0){
  struct dvm_object *o=dvm__obj(v,self);if(!o||o->kind!=DVM_OBJ_CLASS||!o->klass)return fail(v,"java/lang/IllegalArgumentException","Class.getClassLoader receiver");
  if(!class_loader){class_loader=dvm_new_object(v,dvm_find_class_or_external(v,"java/lang/ClassLoader"));if(!class_loader)return fail(v,"java/lang/OutOfMemoryError","ClassLoader");dvm_pin(v,class_loader);}
  out->l=class_loader;return true;
 }
 if(!strcmp(c,"java/util/logging/Logger")&&!strcmp(m,"getLogger")&&!strcmp(sig,"(Ljava/lang/String;)Ljava/util/logging/Logger;")&&n==1){
  if(!a[0].l)return fail(v,"java/lang/NullPointerException","Logger name");const char *s;size_t len;if(!string_arg(v,a[0].l,&s,&len))return fail(v,"java/lang/IllegalArgumentException","Logger name");
  char *name=copy_string(s,len);if(!name)return fail(v,"java/lang/OutOfMemoryError","Logger name");dvm_ref r=dvm_new_object(v,dvm_find_class_or_external(v,c));struct dvm_object *o=dvm__obj(v,r);if(!o){free(name);return fail(v,"java/lang/OutOfMemoryError","Logger");}o->data=name;o->data_destroy=discovery_logger_destroy;out->l=r;return true;
 }
 if(!strcmp(c,"java/util/logging/Logger")&&!strcmp(m,"log")&&((!strcmp(sig,"(Ljava/util/logging/Level;Ljava/lang/String;)V")&&n==2)||(!strcmp(sig,"(Ljava/util/logging/Level;Ljava/lang/String;Ljava/lang/Throwable;)V")&&n==3))){
  struct dvm_object *o=dvm__obj(v,self);if(!typed(v,self,c)||!o->data||o->data_destroy!=discovery_logger_destroy)return fail(v,"java/lang/IllegalArgumentException","Logger receiver");
  if(!a[0].l)return fail(v,"java/lang/NullPointerException","log level");if(!typed(v,a[0].l,"java/util/logging/Level"))return fail(v,"java/lang/IllegalArgumentException","log level");
  if(!a[1].l)return fail(v,"java/lang/NullPointerException","log message");const char *s;size_t len;if(!string_arg(v,a[1].l,&s,&len))return fail(v,"java/lang/IllegalArgumentException","log message");
  const char *level="LEVEL";for(int i=0;i<3;i++)if(a[0].l==levels[i])level=level_names[i];
  if(a[0].l==levels[2])return true; /* Default INFO threshold. */
  sdk_trace_fprintf(stderr,"JAVA_LOG %s %s: %.*s%s\n",level,(char*)o->data,(int)len,s,n==3&&a[2].l?" [throwable attached]":"");return true;
 }
 return false;
}
bool discovery_runtime_selftest(struct dvm *v){
 union dvm_value a={.l=dvm_new_string(v,"org.eclipse.jetty.alpn.ALPN")},out={0};
 if(!discovery_runtime_call(v,"java/lang/Class","forName","(Ljava/lang/String;)Ljava/lang/Class;",0,&a,1,&out)||!dvm_exception(v))return false;dvm_clear_exception(v);
 a.l=dvm_new_string(v,"java.lang.String");if(!discovery_runtime_call(v,"java/lang/Class","forName","(Ljava/lang/String;)Ljava/lang/Class;",0,&a,1,&out)||!out.l||dvm_exception(v))return false;
 union dvm_value name={0};if(!discovery_runtime_call(v,"java/lang/Class","getName","()Ljava/lang/String;",out.l,NULL,0,&name)||!name.l||strcmp(dvm_string_utf8(v,name.l),"java.lang.String"))return false;
 /* Resolve and call the real Class.getName declaration, not just this helper.
  * The current outer runtime chain must preserve primitive/array semantics. */
 static const char *descs[]={"V","Z","B","C","S","I","J","F","D","[I","[[Ljava/lang/String;"};
 static const char *expected[]={"void","boolean","byte","char","short","int","long","float","double","[I","[[Ljava.lang.String;"};
 struct dvm_method *get_name=dvm_find_method(v,dvm_find_class_or_external(v,"java/lang/Class"),"getName","()Ljava/lang/String;");
 if(!get_name)return false;
 for(size_t i=0;i<sizeof descs/sizeof descs[0];i++){
  struct dvm_class *type=dvm__class_by_desc(v,descs[i]);dvm_ref class_object=type?dvm_class_object(v,type):0;union dvm_value actual={0};
  if(!class_object||!dvm_call(v,get_name,class_object,NULL,0,&actual)||dvm_exception(v)||!actual.l||strcmp(dvm_string_utf8(v,actual.l),expected[i]))return false;
 }
 union dvm_value simple={0};if(!discovery_runtime_call(v,"java/lang/Class","getSimpleName","()Ljava/lang/String;",out.l,NULL,0,&simple)||!simple.l||strcmp(dvm_string_utf8(v,simple.l),"String"))return false;
 union dvm_value loader={0};if(!discovery_runtime_call(v,"java/lang/Class","getClassLoader","()Ljava/lang/ClassLoader;",out.l,NULL,0,&loader)||!loader.l)return false;
 union dvm_value loader2={0};if(!discovery_runtime_call(v,"java/lang/Class","getClassLoader","()Ljava/lang/ClassLoader;",out.l,NULL,0,&loader2)||loader2.l!=loader.l)return false;
 union dvm_value f3[3]={{.l=a.l},{.i=0},{.l=loader.l}};
 if(!discovery_runtime_call(v,"java/lang/Class","forName","(Ljava/lang/String;ZLjava/lang/ClassLoader;)Ljava/lang/Class;",0,f3,3,&out)||!out.l)return false;
 a.l=dvm_new_string(v,"okhttp3.internal.platform.Platform");if(!discovery_runtime_call(v,"java/util/logging/Logger","getLogger","(Ljava/lang/String;)Ljava/util/logging/Logger;",0,&a,1,&out)||!out.l)return false;
 union dvm_value level={0};if(!discovery_runtime_static(v,"java/util/logging/Level","INFO","Ljava/util/logging/Level;",&level)||!level.l)return false;
 union dvm_value message[2]={{.l=level.l},{.l=dvm_new_string(v,"discovery probe")}},ignored={0};
 if(!discovery_runtime_call(v,"java/util/logging/Logger","log","(Ljava/util/logging/Level;Ljava/lang/String;)V",out.l,message,2,&ignored)||dvm_exception(v))return false;
 sdk_trace_fprintf(stderr,"DISCOVERY_SELFTEST PASS absent_class=1 class_name=1 simple_name=1 class_loader=1 forName_three_args=1 logger_factory=1 logger_emits=1\n");return true;
}

static void discovery_logger_destroy(void *p){free(p);}

int discovery_gc_payload(struct dvm *v,struct dvm_object *o,dvm_gc_visit_fn visit,void *u){
 (void)v;(void)visit;(void)u;return o->data_destroy==discovery_logger_destroy?DVM_GC_PAYLOAD_ATOMIC:DVM_GC_PAYLOAD_UNKNOWN;
}
