#define _POSIX_C_SOURCE 200809L
#include "native_main_load_probe.h"
#include "android_dlfcn.h"
#include "elf_loader.h"
#include "jni_bridge.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifndef LUNARIA_CONSTRUCTOR_CANDIDATE
#define LUNARIA_CONSTRUCTOR_CANDIDATE "unlabeled"
#endif
static atomic_flag attempted=ATOMIC_FLAG_INIT;
typedef struct { FILE *file;struct dvm *vm;struct dvm_method *method;union dvm_value arg,result;int call_ok;unsigned entered[2],returned[2],opened[2];int versions[2]; } context;
static int persist(context*c){return fflush(c->file)||fsync(fileno(c->file));}
static int init_event(const char*path,size_t idx,uintptr_t address,int entering,void*u){context*c=u;if(fprintf(c->file,"event=constructor_%s path=%s index=%zu address=0x%llx thread=%llu\n",entering?"enter":"returned",path,idx,(unsigned long long)address,(unsigned long long)(uintptr_t)pthread_self())<0||persist(c))abort();return 0;}
static int dl_event(const char*event,const char*path,uintptr_t address,int value,void*u){context*c=u;int i=!strcmp(path,"libunity.so")?0:(!strcmp(path,"libil2cpp.so")?1:-1);if(i>=0){if(!strcmp(event,"jni_enter"))c->entered[i]++;if(!strcmp(event,"jni_returned")){c->returned[i]++;c->versions[i]=value;}}if(strstr(path,"libunity.so")&&!strcmp(event,"open_custom"))c->opened[0]++;if(strstr(path,"libil2cpp.so")&&!strcmp(event,"open_custom"))c->opened[1]++;if(fprintf(c->file,"event=%s path=%s address=0x%llx value=%d thread=%llu\n",event,path,(unsigned long long)address,value,(unsigned long long)(uintptr_t)pthread_self())<0||persist(c))abort();return 0;}
static int invoke(void*u){context*c=u;c->call_ok=dvm_call(c->vm,c->method,0,&c->arg,1,&c->result);return c->call_ok?0:-1;}
static int with_dlopen(void*u){context*c=u;return elf_android_dlfcn_observe(dl_event,c,invoke,c);}
int native_main_load_probe_run(struct dvm*vm,const char*dir,const char*reportdir){
 if(!vm||!dir||!*dir||!reportdir||!*reportdir||atomic_flag_test_and_set(&attempted))return -1;
 char report[4096],path[4096],why[1024];int n=snprintf(report,sizeof report,"%s/native-main-load.txt",reportdir);if(n<0||(size_t)n>=sizeof report)return -1;
 FILE*f=fopen(report,"w");if(!f)return -1;context c={.file=f,.vm=vm};struct jni_bridge*b=jni_bridge_for_vm(vm);int ok=0;int graph_ok=1;
 if(fprintf(f,"event=begin candidate=%s pid=%ld thread=%llu dir=%s engine_entry_called=0\n",LUNARIA_CONSTRUCTOR_CANDIDATE,(long)getpid(),(unsigned long long)(uintptr_t)pthread_self(),dir)<0||persist(&c))goto done;
 if(!b||!jni_bridge_thread_is_attached(b)||jni_bridge_pending(b)||dvm_exception(vm)||!jni_bridge_native_registered(vm,"com/unity3d/player/NativeLoader","load","(Ljava/lang/String;)Z")||!jni_bridge_native_registered(vm,"com/unity3d/player/NativeLoader","unload","()Z")){fprintf(f,"event=failed reason=bridge_or_registration\n");goto done;}
 n=snprintf(path,sizeof path,"%s/libmain.so",dir);if(n<0||(size_t)n>=sizeof path)goto done;
 elf_loader_t *main_graph=elf_loader_acquire_initialized_cached(path,why,sizeof why);
 void *main_open=NULL,*main_sym=NULL;
 int main_domain=main_graph&&elf_loader_import_target(main_graph,"dlopen",&main_open)==1&&elf_loader_import_target(main_graph,"dlsym",&main_sym)==1&&main_open==(void*)(uintptr_t)&elf_android_dlopen&&main_sym==(void*)(uintptr_t)&elf_android_dlsym;
 if(fprintf(f,"event=main_backend custom_initialized=%d coherent_dlopen_dlsym=%d\n",main_graph!=NULL,main_domain)<0){if(main_graph)elf_loader_close(main_graph);goto done;}
 if(main_graph)elf_loader_close(main_graph);
 if(!main_domain)goto done;
 for(int i=0;i<2;i++){n=snprintf(path,sizeof path,"%s/%s",dir,i?"libil2cpp.so":"libunity.so");if(n<0||(size_t)n>=sizeof path)goto done;elf_loader_t *prior=elf_loader_acquire_initialized_cached(path,why,sizeof why);if(prior){elf_loader_close(prior);fprintf(f,"event=failed reason=already_initialized path=%s\n",path);goto done;}}
 c.method=dvm_lookup(vm,"com/unity3d/player/NativeLoader","load","(Ljava/lang/String;)Z");if(!c.method||!dvm_method_is_static(c.method)){fprintf(f,"event=failed reason=method_missing\n");goto done;}
 c.arg.l=dvm_new_string(vm,dir);if(!c.arg.l||dvm_exception(vm)){fprintf(f,"event=failed reason=string_creation\n");goto done;}
 unsigned before=jni_bridge_native_registration_count(vm),limited=jni_bridge_limited_count(b);
 if(fprintf(f,"event=load_enter registrations=%u limited=%u\n",before,limited)<0||persist(&c))goto done;
 (void)elf_loader_with_init_observer(init_event,&c,with_dlopen,&c);
 int pending=jni_bridge_pending(b);dvm_ref exception=dvm_exception(vm);unsigned after=jni_bridge_native_registration_count(vm),lim_after=jni_bridge_limited_count(b);
 if(fprintf(f,"event=load_returned call_ok=%d value=%d pending=%d exception=%u registrations_before=%u registrations_after=%u limited_before=%u limited_after=%u unity_open=%u unity_jni_enter=%u unity_jni_return=%u il2cpp_open=%u il2cpp_jni_enter=%u il2cpp_jni_return=%u engine_entry_called=0\n",c.call_ok,c.result.i,pending,exception,before,after,limited,lim_after,c.opened[0],c.entered[0],c.returned[0],c.opened[1],c.entered[1],c.returned[1])<0||persist(&c))goto done;
 for(int i=0;i<2;i++){n=snprintf(path,sizeof path,"%s/%s",dir,i?"libil2cpp.so":"libunity.so");if(n<0||(size_t)n>=sizeof path)goto done;elf_loader_t*l=elf_loader_acquire_initialized_cached(path,why,sizeof why);if(!l)graph_ok=0;if(fprintf(f,"event=graph_status path=%s initialized=%d status=%s\n",path,l!=NULL,l?elf_loader_status(l):why)<0){if(l)elf_loader_close(l);goto done;}if(l)elf_loader_close(l);}
 ok=graph_ok&&c.call_ok&&c.result.i&&!pending&&!exception&&lim_after==limited&&c.opened[0]&&c.opened[1]&&c.entered[0]==1&&c.returned[0]==1&&c.entered[1]==1&&c.returned[1]==1&&(c.versions[0]==JNI_VERSION_1_4||c.versions[0]==JNI_VERSION_1_6)&&(c.versions[1]==JNI_VERSION_1_4||c.versions[1]==JNI_VERSION_1_6);
 done: if(fprintf(f,"event=probe_result result=%s engine_entry_called=0\n",ok?"pass":"fail")<0||persist(&c))ok=0;if(fclose(f))ok=0;return ok?0:-1;
}
