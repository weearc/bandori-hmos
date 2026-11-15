#define _GNU_SOURCE 1
#include "android_dlfcn.h"
#include "android_dlfcn_probe.h"
#include "elf_loader.h"
#include "window_runtime.h"
#include <errno.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
extern int sdk_trace_fprintf(FILE*,const char*,...);
struct thread_result {int ok;};
int window_runtime_probe_run(const char *directory){
 char path[2048];void *window=NULL,*replacement=NULL,*library=NULL;int rc=0;int32_t width=0,height=0;
#define WINDOW_CHECK(expr,code) do{if(!(expr)){rc=code;goto finish;}}while(0)
 WINDOW_CHECK(directory&&snprintf(path,sizeof path,"%s/libnested_leaf.so",directory)<(int)sizeof path,1);
 WINDOW_CHECK(window_runtime_get_width(NULL)==-EINVAL&&window_runtime_get_height((void*)(uintptr_t)0x1234)==-EINVAL,2);
 window=window_runtime_acquire_current();WINDOW_CHECK(window,3);
 width=window_runtime_get_width(window);height=window_runtime_get_height(window);
 WINDOW_CHECK(width>0&&height>0&&window_runtime_ref_count(window)==1,4);
 WINDOW_CHECK(window_runtime_set_host(NULL)==-EBUSY,5);
 window_runtime_acquire(window);WINDOW_CHECK(window_runtime_ref_count(window)==2,6);
 window_runtime_release(window);WINDOW_CHECK(window_runtime_ref_count(window)==1,7);
 library=elf_android_dlopen(path,RTLD_NOW|RTLD_LOCAL);WINDOW_CHECK(library,8);
 int(*check)(void*,int,int)=(int(*)(void*,int,int))(uintptr_t)elf_android_dlsym(library,"leaf_window_check");
 WINDOW_CHECK(check&&check(window,width,height)==0&&window_runtime_ref_count(window)==1,9);
 void *old=window;window_runtime_release(window);window=NULL;
 WINDOW_CHECK(window_runtime_get_width(old)==-EINVAL,10);
 replacement=window_runtime_acquire_current();WINDOW_CHECK(replacement&&replacement!=old,11);
 WINDOW_CHECK(window_runtime_get_width(old)==-EINVAL&&window_runtime_get_width(replacement)>0,12);
finish:
 if(window)window_runtime_release(window);
 if(replacement)window_runtime_release(replacement);
 if(library)elf_android_dlclose(library);
 sdk_trace_fprintf(stderr,"NATIVE_WINDOW_BRIDGE_RESULT %s check=%d width=%d height=%d real_host=1 imported_calls=%d geometry=%d default_reset=%d invalid_rejected=%d stale_rejected=%d probe_layout=partial engine_started=0\n",rc?"FAIL":"PASS",rc,width,height,!rc,!rc,!rc,!rc,!rc);
 return rc;
#undef WINDOW_CHECK
}
void unity_library_preflight(const char *directory){
 char path[2048],why[1024];
 if(!directory||snprintf(path,sizeof path,"%s/libunity.so",directory)>=(int)sizeof path)return;
 /* Inspection mapping uses the existing constructor-skipping mode. This is
  * not dlopen success or an engine start. A later real load needs a fresh
  * process if this diagnostic cached an uninitialized/failed mapping. */
 elf_loader_t *library=elf_loader_open(path,why,sizeof why);
 sdk_trace_fprintf(stderr,"UNITY_LIBRARY_PREFLIGHT name=libunity.so mapped=%d initialization_requested=0 engine_started=0 status=%s\n",library!=NULL,library?elf_loader_status(library):why);
 if(library)elf_loader_close(library);
}
static void *error_worker(void *user){
 struct thread_result *r=user;
 r->ok=elf_android_dlerror()==NULL;
 r->ok=r->ok&&!elf_android_dlopen("unused",RTLD_NOW|RTLD_GLOBAL);
 char *e=elf_android_dlerror();r->ok=r->ok&&e&&strstr(e,"flags")&&elf_android_dlerror()==NULL;
 return NULL;
}
int android_dlfcn_probe_run(const char *dir){
 char caller_path[2048],leaf_path[2048],skip_path[2048],why[1024];
 void *caller=NULL,*system=NULL;elf_loader_t *skipped=NULL;int rc=0;
 if(!dir)return 1;
 if(snprintf(caller_path,sizeof caller_path,"%s/libnested_caller.so",dir)>=(int)sizeof caller_path||
    snprintf(leaf_path,sizeof leaf_path,"%s/libnested_leaf.so",dir)>=(int)sizeof leaf_path||
    snprintf(skip_path,sizeof skip_path,"%s/libnested_skipped.so",dir)>=(int)sizeof skip_path)return 2;
#define CHECK(expr,code) do{if(!(expr)){rc=(code);goto done;}}while(0)
 (void)elf_android_dlerror();
 caller=elf_android_dlopen(caller_path,RTLD_NOW|RTLD_LOCAL);
 CHECK(caller,3);CHECK(elf_android_handle_is_custom(caller)==1,4);
 int(*run)(const char*)=(int(*)(const char*))(uintptr_t)elf_android_dlsym(caller,"nested_caller_run");
 CHECK(run,5);
 int nested=run(leaf_path);sdk_trace_fprintf(stderr,"NESTED_DLOPEN_CALLER result=%d\n",nested);CHECK(nested==0,6);
 (void)elf_android_dlerror();
 CHECK(!elf_android_dlsym(caller,"leaf_fixed_value"),7);
 CHECK(elf_android_dlerror()!=NULL,8);
 CHECK(!elf_android_dlsym(caller,"JNI_OnLoad"),9);
 CHECK(elf_android_dlerror()!=NULL,10);
 CHECK(elf_android_dlsym(RTLD_DEFAULT,"malloc")!=NULL,11);
 system=elf_android_dlopen("libm.so",RTLD_NOW|RTLD_LOCAL);CHECK(system,12);
 CHECK(elf_android_handle_is_custom(system)==0,13);
 double(*cosine)(double)=(double(*)(double))(uintptr_t)elf_android_dlsym(system,"cos");
 CHECK(cosine&&cosine(0.0)==1.0,14);
 CHECK(elf_android_dlclose(system)==0,15);system=NULL;
 /* A failed lookup on one thread must remain pending while another thread
  * creates and consumes its own independent loader error. */
 CHECK(!elf_android_dlsym(caller,"thread_specific_missing_symbol"),16);
 struct thread_result result={0};pthread_t thread;
 CHECK(pthread_create(&thread,NULL,error_worker,&result)==0,17);
 pthread_join(thread,NULL);CHECK(result.ok,18);
 char *e=elf_android_dlerror();CHECK(e&&strstr(e,"thread_specific_missing_symbol"),19);
 CHECK(elf_android_dlerror()==NULL,20);
 skipped=elf_loader_open(skip_path,why,sizeof why);CHECK(skipped,21);
 int(*get_count)(void)=(int(*)(void))(uintptr_t)elf_loader_symbol_scoped(skipped,"leaf_get_constructor_count");
 CHECK(get_count&&get_count()==0,22);
 CHECK(!elf_android_dlopen(skip_path,RTLD_NOW|RTLD_LOCAL),23);
 e=elf_android_dlerror();CHECK(e&&strstr(e,"cached diagnostic mapping"),24);
 CHECK(!elf_android_dlsym(caller,"__sF"),25);CHECK(elf_android_dlerror()!=NULL,26);
 void *old=caller;CHECK(elf_android_dlclose(caller)==0,27);caller=NULL;
 void *replacement=elf_android_dlopen(leaf_path,RTLD_NOW|RTLD_LOCAL);CHECK(replacement,28);
 CHECK(elf_android_handle_is_custom(old)==-1,29);
 CHECK(!elf_android_dlsym(old,"leaf_fixed_value"),30);(void)elf_android_dlerror();
 CHECK(elf_android_dlsym(replacement,"leaf_fixed_value")!=NULL,31);
 CHECK(elf_android_dlclose(replacement)==0,32);
 for(unsigned i=0;i<1025;++i){
  void *recycled=elf_android_dlopen(leaf_path,RTLD_NOW|RTLD_LOCAL);CHECK(recycled,33);
  CHECK(elf_android_dlclose(recycled)==0,34);
 }
 CHECK(elf_android_dlclose((void*)(uintptr_t)0x1234)==-1,35);
 (void)elf_android_dlerror();
done:
 if(rc){const char *error=elf_android_dlerror();sdk_trace_fprintf(stderr,"NESTED_DLOPEN_RESULT FAIL check=%d error=%s\n",rc,error?error:"none");}
 else sdk_trace_fprintf(stderr,"NESTED_DLOPEN_RESULT PASS custom_nested=1 constructor_once=1 scoped_lookup=1 shim_scope=1 missing_jni_onload_not_borrowed=1 system_backend=1 rtld_default=1 tls_errors=1 closed_rejected=1 stale_generation_rejected=1 recycle_cycles=1025 skipped_ctor_rejected=1 engine_started=0\n");
 if(caller)elf_android_dlclose(caller);
 if(system)elf_android_dlclose(system);
 if(skipped)elf_loader_close(skipped);
 return rc;
#undef CHECK
}
