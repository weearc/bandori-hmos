#define _POSIX_C_SOURCE 200809L
#include "unity_jni_probe.h"
#include "elf_loader.h"
#include "jni_bridge.h"
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#ifndef LUNARIA_CONSTRUCTOR_CANDIDATE
#define LUNARIA_CONSTRUCTOR_CANDIDATE "unlabeled"
#endif
extern int sdk_trace_fprintf(FILE *, const char *, ...);
static atomic_flag attempted = ATOMIC_FLAG_INIT;
static int sync_log(FILE *f)
{
    int failed=fflush(f)!=0;
    if(fsync(fileno(f))!=0)failed=1;
    return failed?-1:0;
}
int unity_jni_onload_probe_run(struct dvm *vm,const char *directory,const char *report_dir)
{
    char path[4096], report[4096], err[1024]={0};
    if(!vm || !directory || !*directory || !report_dir || !*report_dir)return -1;
    int n=snprintf(path,sizeof(path),"%s/libunity.so",directory);
    if(n<0 || (size_t)n>=sizeof(path))return -1;
    n=snprintf(report,sizeof(report),"%s/unity-jni-onload.txt",report_dir);
    if(n<0 || (size_t)n>=sizeof(report))return -1;
    if(atomic_flag_test_and_set(&attempted))return -1;
    int fd=open(report,O_WRONLY|O_CREAT|O_TRUNC,0600);
    if(fd<0)return -1;
    FILE *f=fdopen(fd,"w");
    if(!f){close(fd);return -1;}
    int ok=0; elf_loader_t *cached=NULL,*strict=NULL;
    struct jni_bridge *bridge=jni_bridge_for_vm(vm);
    if(fprintf(f,"event=begin candidate=%s pid=%ld thread=%llu path=%s backend=custom_own_export engine_entry_called=0 system_load_library=0\n",
       LUNARIA_CONSTRUCTOR_CANDIDATE,(long)getpid(),(unsigned long long)(uintptr_t)pthread_self(),path)<0 || sync_log(f))goto done;
    cached=elf_loader_acquire_initialized_cached(path,err,sizeof(err));
    if(!cached){fprintf(f,"event=failed reason=cached_graph_unavailable detail=%s\n",err);goto done;}
    strict=elf_loader_open_initialized(path,err,sizeof(err));
    if(!strict || strict!=cached){fprintf(f,"event=failed reason=strict_map_mismatch detail=%s\n",err);goto done;}
    void *symbol=elf_loader_own_executable_symbol(strict,"JNI_OnLoad");
    if(!symbol){fprintf(f,"event=failed reason=own_executable_export_missing\n");goto done;}
    if(!bridge || !jni_bridge_thread_is_attached(bridge) || !jni_bridge_env(bridge) ||
       !jni_bridge_java_vm(bridge) || jni_bridge_pending(bridge)) {
        fprintf(f,"event=failed reason=bridge_thread_or_pending\n");goto done;
    }
    jint (*onload)(JavaVM *,void *)=NULL;
    if(sizeof(onload)!=sizeof(symbol)){fprintf(f,"event=failed reason=function_pointer_size\n");goto done;}
    memcpy(&onload,&symbol,sizeof(onload));
    uint32_t before=jni_bridge_native_registration_count(vm), limited=jni_bridge_limited_count(bridge);
    if(fprintf(f,"event=enter address=%p map=%p same_map=1 thread=%llu registrations_before=%u limited_before=%u status=%s\n",
               symbol,(void *)strict,(unsigned long long)(uintptr_t)pthread_self(),before,limited,elf_loader_status(strict))<0 || sync_log(f))goto done;
    sdk_trace_fprintf(stderr,"UNITY_JNI_ONLOAD_PROBE enter backend=custom_own_export engine_entry_called=0 report=%s\n",report);
    unsigned depth=jni_bridge_gil_drop(bridge);
    jint version=onload(jni_bridge_java_vm(bridge),NULL);
    jni_bridge_gil_restore(bridge,depth);
    int pending=jni_bridge_pending(bridge);
    uint32_t after=jni_bridge_native_registration_count(vm),limited_after=jni_bridge_limited_count(bridge);
    ok=(version==JNI_VERSION_1_4 || version==JNI_VERSION_1_6) && !pending && limited_after==limited;
    if(fprintf(f,"event=returned version=0x%08x pending=%d registrations_before=%u registrations_after=%u limited_before=%u limited_after=%u result=%s bridge_error=%s\n",
       (unsigned)version,pending,before,after,limited,limited_after,ok?"pass":"fail",jni_bridge_last_error(bridge))<0)ok=0;
    if(sync_log(f))ok=0;
 done:
    if(!ok){(void)fprintf(f,"event=probe_failed engine_entry_called=0\n");(void)sync_log(f);}
    if(strict)elf_loader_close(strict);
    if(cached)elf_loader_close(cached);
    if(fclose(f)!=0)ok=0;
    sdk_trace_fprintf(stderr,"UNITY_JNI_ONLOAD_PROBE result=%s engine_entry_called=0 report=%s\n",ok?"PASS":"FAIL",report);
    return ok?0:-1;
}
