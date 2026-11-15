#define _GNU_SOURCE 1
#include "android_dlfcn.h"
#include "elf_loader.h"
#include "android_stdio_shim.h"
#include "jni_abi.h"
#include <stdlib.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <errno.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/stat.h>
#include <EGL/egl.h>
extern void *android_opensles_adapted_symbol(const char *name);

#define HANDLE_LIMIT 512
struct adapted_handle { uint32_t generation;unsigned refs;int used,open,custom,kind,gles;void *system;elf_loader_t *mapped; };
_Static_assert(sizeof(uintptr_t)==8,"opaque handle tokens require a 64-bit target");
#define HANDLE_TAG UINT64_C(0xd10c000000000000)
static struct adapted_handle handles[HANDLE_LIMIT];
static pthread_mutex_t mutex;
static pthread_once_t once=PTHREAD_ONCE_INIT;
static _Thread_local char error_text[1024];
static _Thread_local int error_pending;
static _Thread_local android_dlfcn_observer trace_observer;
static _Thread_local void *trace_userdata;
static _Thread_local jint (*trace_jni[2])(JavaVM *,void *);
static int target_kind(const char *path){if(!path)return 0;const char *base=strrchr(path,'/');base=base?base+1:path;return !strcmp(base,"libunity.so")?1:(!strcmp(base,"libil2cpp.so")?2:0);}
static const char *kind_path(int kind){return kind==1?"libunity.so":"libil2cpp.so";}
static void trace_event(const char *event,const char *path,uintptr_t address,int value){if(trace_observer && trace_observer(event,path,address,value,trace_userdata))abort();}
static jint call_jni(int kind,JavaVM *vm,void *reserved){jint (*fn)(JavaVM *,void *)=trace_jni[kind-1];if(!fn)abort();trace_event("jni_enter",kind_path(kind),(uintptr_t)fn,0);jint result=fn(vm,reserved);trace_event("jni_returned",kind_path(kind),(uintptr_t)fn,result);return result;}
static jint unity_jni(JavaVM *vm,void *r){return call_jni(1,vm,r);}
static jint il2cpp_jni(JavaVM *vm,void *r){return call_jni(2,vm,r);}
int elf_android_dlfcn_observe(android_dlfcn_observer observer,void *userdata,android_dlfcn_body body,void *bodydata){
 if(!body)return -1;android_dlfcn_observer old=trace_observer;void *data=trace_userdata;
 jint (*old0)(JavaVM *,void *)=trace_jni[0],(*old1)(JavaVM *,void *)=trace_jni[1];
 trace_observer=observer;trace_userdata=userdata;trace_jni[0]=trace_jni[1]=NULL;
 int result=body(bodydata);trace_observer=old;trace_userdata=data;trace_jni[0]=old0;trace_jni[1]=old1;return result;
}

static void init_mutex(void){
 pthread_mutexattr_t a;pthread_mutexattr_init(&a);
 pthread_mutexattr_settype(&a,PTHREAD_MUTEX_RECURSIVE);
 pthread_mutex_init(&mutex,&a);pthread_mutexattr_destroy(&a);
}
static void lock_handles(void){pthread_once(&once,init_mutex);pthread_mutex_lock(&mutex);}
static void errorf(const char *fmt,...){
 va_list a;va_start(a,fmt);vsnprintf(error_text,sizeof error_text,fmt,a);va_end(a);error_pending=1;
 int saved_errno=errno;static _Atomic unsigned count;
 if(atomic_fetch_add(&count,1)<128){
  FILE *file=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt","a");
  if(file){fprintf(file,"ANDROID_DLERROR thread=%llu text=%s\n",(unsigned long long)(uintptr_t)pthread_self(),error_text);fclose(file);}
 }
 errno=saved_errno;
}
char *elf_android_dlerror(void){
 if(!error_pending)return NULL;
 error_pending=0;return error_text;
}
static struct adapted_handle *find_handle(void *p){
 uint64_t token=(uint64_t)(uintptr_t)p;
 if((token&UINT64_C(0xffff000000000000))!=HANDLE_TAG)return NULL;
 unsigned index=(unsigned)(token&0xffffu);
 uint32_t generation=(uint32_t)((token>>16)&UINT32_MAX);
 if(!index||index>HANDLE_LIMIT)return NULL;
 struct adapted_handle *h=&handles[index-1];
 return h->used&&h->generation==generation?h:NULL;
}
static void *token_for(struct adapted_handle *h){
 return (void*)(uintptr_t)(HANDLE_TAG|((uint64_t)h->generation<<16)|(uint64_t)((h-handles)+1));
}
static struct adapted_handle *reserve_handle(void){
 lock_handles();
 for(unsigned i=0;i<HANDLE_LIMIT;++i)if(!handles[i].used&&handles[i].generation!=UINT32_MAX){
  ++handles[i].generation;handles[i].used=1;pthread_mutex_unlock(&mutex);return &handles[i];
 }
 pthread_mutex_unlock(&mutex);errorf("dlopen adapter handle limit reached");return NULL;
}
static void *abandon_handle(struct adapted_handle *h){
 lock_handles();uint32_t generation=h->generation;memset(h,0,sizeof *h);h->generation=generation;
 pthread_mutex_unlock(&mutex);return NULL;
}
static void *adapted_symbol(const char *name){
 void *audio=android_opensles_adapted_symbol(name);if(audio)return audio;
 void *abi=elf_loader_android_abi_symbol(name);if(abi)return abi;
 if(!strcmp(name,"dlopen"))return (void*)(uintptr_t)&elf_android_dlopen;
 if(!strcmp(name,"dlsym"))return (void*)(uintptr_t)&elf_android_dlsym;
 if(!strcmp(name,"dlclose"))return (void*)(uintptr_t)&elf_android_dlclose;
 if(!strcmp(name,"dlerror"))return (void*)(uintptr_t)&elf_android_dlerror;
 return NULL;
}
/* Shared by JNI System.load and translated dlopen. Each successful call owns
 * exactly one loader reference. Search every initialized cache candidate first;
 * never rerun constructors merely because the caller used a bare name. */
elf_loader_t *elf_android_open_bundled_library(const char *name,int cached_only,char *err,size_t errlen){
 char paths[3][2048],attempts[6200]="",detail[512]="";unsigned count=0;
 if(err&&errlen)err[0]=0;
 if(!name||!*name){if(err&&errlen)snprintf(err,errlen,"empty bundled library name");return NULL;}
 if(strchr(name,'/')){
  if(snprintf(paths[0],sizeof paths[0],"%s",name)>=(int)sizeof paths[0])goto too_long;
  count=1;
 }else{
  static const char *formats[]={"/data/storage/el1/bundle/libs/arm64/%s","/data/storage/el1/bundle/libs/arm64/lib%s.so","/data/storage/el1/bundle/libs/arm64/%s.so"};
  for(unsigned i=0;i<3;i++)if(snprintf(paths[count++],sizeof paths[0],formats[i],name)>=(int)sizeof paths[0])goto too_long;
 }
 for(unsigned i=0;i<count;i++){
  size_t n=strlen(attempts);snprintf(attempts+n,sizeof attempts-n,"%s%s",i?",":"",paths[i]);
  elf_loader_t *l=elf_loader_acquire_initialized_cached(paths[i],detail,sizeof detail);
  if(l){static _Atomic unsigned hits;if(atomic_fetch_add(&hits,1)<48){FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt","a");if(f){fprintf(f,"BUNDLED_DLOPEN request=%s candidate=%s cached=1 mapped=%p\n",name,paths[i],(void*)l);fclose(f);}}return l;}
 }
 if(cached_only)return NULL;
 int regular=0;
 for(unsigned i=0;i<count;i++){
  struct stat st;if(stat(paths[i],&st)||!S_ISREG(st.st_mode))continue;regular=1;
  elf_loader_t *l=elf_loader_open_initialized(paths[i],detail,sizeof detail);if(l)return l;
 }
 if(err&&errlen)snprintf(err,errlen,"%s%s; candidates=[%s]",regular?"custom load failed: ":"no regular bundled candidate",regular?detail:"",attempts);
 return NULL;
too_long:
 if(err&&errlen)snprintf(err,errlen,"bundled library candidate too long");return NULL;
}
static void *publish_custom(struct adapted_handle *h,elf_loader_t *mapped){
 lock_handles();
 for(unsigned i=0;i<HANDLE_LIMIT;i++)if(&handles[i]!=h&&handles[i].used&&handles[i].open&&handles[i].custom&&handles[i].mapped==mapped&&handles[i].kind==h->kind){
  /* The acquisition already incremented the loader refcount. dlclose releases
   * one reference per call, while the generation token stays live until zero. */
  if(handles[i].refs==UINT32_MAX){pthread_mutex_unlock(&mutex);elf_loader_close(mapped);errorf("dlopen reference count overflow");return abandon_handle(h);}
  ++handles[i].refs;void *token=token_for(&handles[i]);abandon_handle(h);pthread_mutex_unlock(&mutex);return token;
 }
 h->custom=1;h->mapped=mapped;h->open=1;h->refs=1;void *token=token_for(h);pthread_mutex_unlock(&mutex);return token;
}
void *elf_android_dlopen(const char *path,int flags){
 int binding=flags&(RTLD_NOW|RTLD_LAZY);
 if((binding!=RTLD_NOW&&binding!=RTLD_LAZY)||(flags&~(RTLD_NOW|RTLD_LAZY))){
  errorf("unsupported dlopen flags=0x%x (LOCAL LAZY/NOW required)",flags);return NULL;
 }
 int kind=trace_observer?target_kind(path):0;
 if(kind)trace_event("open_enter",path,0,flags);
 struct adapted_handle *h=reserve_handle();if(!h)return NULL;
 h->kind=kind;
 /* Eager binding is deliberate: lazy system binding may accept an Android
  * DSO then abort on its first Bionic import, preventing a real fallback. */
 /* These verified Android targets require guest ABI overrides. Select the
  * custom backend before a system dlopen could execute unadapted constructors. */
 if(kind)trace_event("custom_backend_selected",path,0,1);
 char custom_error[7168];
 elf_loader_t *mapped=elf_android_open_bundled_library(path,1,custom_error,sizeof custom_error);
 if(mapped){if(kind)trace_event("open_custom",path,(uintptr_t)mapped,1);return publish_custom(h,mapped);}
 dlerror();void *system=kind?NULL:dlopen(path,RTLD_NOW|RTLD_LOCAL);
 const char *e=dlerror();char system_error[512];
 snprintf(system_error,sizeof system_error,"%s",e?e:"unknown error");
 if(system){
  const char *leaf=path?strrchr(path,'/'):NULL;
  leaf=leaf?leaf+1:path;
  lock_handles();h->system=system;h->open=1;h->refs=1;
  h->gles=leaf&&(!strcmp(leaf,"libGLESv2.so")||!strcmp(leaf,"libGLESv3.so"));
  void *token=token_for(h);pthread_mutex_unlock(&mutex);return token;
 }
 if(!path||!*path){errorf("system dlopen failed: %s",system_error);return abandon_handle(h);}
 mapped=elf_android_open_bundled_library(path,0,custom_error,sizeof custom_error);
 if(!mapped){if(kind)trace_event("open_failed",path,0,0);errorf("dlopen %s failed: %s; custom: %s",path,system_error,custom_error);return abandon_handle(h);}
 if(kind)trace_event("open_custom",path,(uintptr_t)mapped,1);
 return publish_custom(h,mapped);
}
void *elf_android_dlsym(void *handle,const char *name){
 if(!name||!*name){errorf("dlsym requires a symbol name");return NULL;}
 if(handle==RTLD_DEFAULT){
  if(!strcmp(name,"__sF"))return &android_stdio_sF[0][0];
  void *adapted=adapted_symbol(name);if(adapted)return adapted;
  dlerror();void *p=dlsym(RTLD_DEFAULT,name);const char *e=dlerror();
  if(e){errorf("dlsym DEFAULT %s: %s",name,e);return NULL;}return p;
 }
 if(handle==RTLD_NEXT){errorf("RTLD_NEXT is unsupported for translated callers");return NULL;}
 lock_handles();struct adapted_handle *h=find_handle(handle);
 if(!h||!h->open){pthread_mutex_unlock(&mutex);errorf("dlsym invalid or closed adapter handle");return NULL;}
 void *p;
 if(trace_observer && h->kind && !strcmp(name,"JNI_OnLoad")){
  int kind=h->kind;void *own=h->custom?elf_loader_own_executable_symbol(h->mapped,name):NULL;
  pthread_mutex_unlock(&mutex);
  trace_event(own?"jni_own_found":"jni_own_missing",kind_path(kind),(uintptr_t)own,0);
  if(!own){errorf("own JNI_OnLoad missing or unsupported");abort();}
  memcpy(&trace_jni[kind-1],&own,sizeof(own));
  return kind==1?(void *)(uintptr_t)&unity_jni:(void *)(uintptr_t)&il2cpp_jni;
 }
 /* 诊断：谁在查询 JNI_OnLoad。Unity 的 libmain 加载 libunity 后应查询并
  * 调用它（RegisterNatives 注册 nativeInjectEvent 等输入入口）；72qt 只看到
  * NativeLoader 的注册表，因此需要确认查询是否发生。 */
 if(!strcmp(name,"JNI_OnLoad")){
  static _Atomic unsigned jni_query;
  if(atomic_fetch_add(&jni_query,1)<8){
   int saved_errno=errno;FILE*f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt","a");
   if(f){fprintf(f,"JNI_ONLOAD_QUERY handle=%p kind=%d custom=%d\n",(void *)handle,h->kind,h->custom);fclose(f);}
   errno=saved_errno;
  }
 }
 if(h->custom){
  p=adapted_symbol(name);if(!p)p=elf_loader_symbol_scoped(h->mapped,name);
  if(!p)errorf("dlsym custom symbol not found: %s",name);
 }else{
  p=!strcmp(name,"__sF")?(void*)&android_stdio_sF[0][0]:adapted_symbol(name);
  /* The OHOS NDK GLES entry returned null for GL_RENDERER despite a current
   * vendor context. Resolve GL entry points through that EGL driver's
   * dispatch, retaining the system-library fallback for absent symbols. */
  if(!p&&h->gles&&name[0]=='g'&&name[1]=='l')
   p=(void*)eglGetProcAddress(name);
  if(!p){dlerror();p=dlsym(h->system,name);const char *e=dlerror();
   if(e){errorf("dlsym system %s: %s",name,e);p=NULL;}}
 }
 if(!strcmp(name,"glGetString")){
  FILE *file=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/engine-probe.txt","a");
  Dl_info info={0};if(p)dladdr(p,&info);
  if(file){fprintf(file,"GL_SYMBOL api=dlsym name=%s symbol=%p module=%s context=%p thread=%llu\n",
   name,p,info.dli_fname?info.dli_fname:"?",(void*)eglGetCurrentContext(),
   (unsigned long long)(uintptr_t)pthread_self());fclose(file);}
 }
 pthread_mutex_unlock(&mutex);return p;
}
int elf_android_dlclose(void *handle){
 lock_handles();struct adapted_handle *h=find_handle(handle);
 if(!h||!h->open){pthread_mutex_unlock(&mutex);errorf("dlclose invalid or closed adapter handle");return -1;}
 int r;
 if(h->custom)r=elf_loader_close(h->mapped);
 else{dlerror();r=dlclose(h->system);if(r){const char *e=dlerror();errorf("system dlclose: %s",e?e:"unknown error");}}
 if(!r&&!--h->refs){h->open=0;h->used=0;h->custom=0;h->system=NULL;h->mapped=NULL;}
 else if(r&&h->custom)errorf("custom dlclose failed");
 pthread_mutex_unlock(&mutex);return r;
}
int elf_android_handle_is_custom(void *handle){
 lock_handles();struct adapted_handle *h=find_handle(handle);
 int r=h&&h->open?h->custom:-1;pthread_mutex_unlock(&mutex);return r;
}
