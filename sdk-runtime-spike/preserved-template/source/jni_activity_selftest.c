#include "jni_bridge.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
extern int sdk_trace_fprintf(FILE *,const char *,...);

/* Invoke after the original Activity has been constructed and its diagnostic
 * Intent bound. Does not create or bind either object. Caller holds the GIL. */
bool jni_activity_get_intent_selftest(struct dvm *vm,dvm_ref activity,dvm_ref intent)
{
    struct jni_bridge *bridge=vm?jni_bridge_for_vm(vm):NULL;
    JNIEnv *env=bridge?jni_bridge_env(bridge):NULL;
    if(!env||!activity||!intent||(*env)->ExceptionCheck(env))return false;
    if((*env)->PushLocalFrame(env,32)<0)return false;
    bool ok=false;
    const char *stage="local_refs";
    jobject receiver=jni_bridge_new_local_object_for_env(env,activity);
    jobject expected=jni_bridge_new_local_object_for_env(env,intent);
    if(!receiver||!expected||(*env)->ExceptionCheck(env))goto done;
    stage="lookup";
    jclass cls=(*env)->FindClass(env,"android/app/Activity");
    jmethodID method=cls?(*env)->GetMethodID(env,cls,"getIntent","()Landroid/content/Intent;"):NULL;
    if(!method||(*env)->ExceptionCheck(env))goto done;
    stage="call";
    jobject actual=(*env)->CallObjectMethodA(env,receiver,method,NULL);
    if(!actual||(*env)->ExceptionCheck(env))goto done;
    stage="intent_type";
    jclass intent_class=(*env)->FindClass(env,"android/content/Intent");
    if(!intent_class||(*env)->ExceptionCheck(env))goto done;
    if(!(*env)->IsSameObject(env,actual,expected)||
       !(*env)->IsInstanceOf(env,actual,intent_class)||(*env)->ExceptionCheck(env))goto done;
    stage="intent_get_extras";
    jmethodID get_extras=(*env)->GetMethodID(env,intent_class,"getExtras","()Landroid/os/Bundle;");
    if(!get_extras||(*env)->ExceptionCheck(env))goto done;
    jobject extras=(*env)->CallObjectMethodA(env,actual,get_extras,NULL);
    ok=!extras&&!(*env)->ExceptionCheck(env);
    if(!ok)goto done;
    ok=false;
    stage="context_private_directories";
    jclass context=(*env)->FindClass(env,"android/content/Context");
    jclass file=(*env)->FindClass(env,"java/io/File");
    jclass info=(*env)->FindClass(env,"android/content/pm/ApplicationInfo");
    jmethodID absolute=file?(*env)->GetMethodID(env,file,"getAbsolutePath","()Ljava/lang/String;"):NULL;
    jmethodID getpath=file?(*env)->GetMethodID(env,file,"getPath","()Ljava/lang/String;"):NULL;
    if(!context||!file||!info||!absolute||!getpath||(*env)->ExceptionCheck(env))goto done;
    const char *getters[]={"getFilesDir","getCacheDir"};
    const char *expected_dirs[]={"/data/storage/el2/base/haps/entry/files/guest-files",
                            "/data/storage/el2/base/haps/entry/files/guest-cache"};
    for(int i=0;i<2;i++){
        jmethodID getter=(*env)->GetMethodID(env,context,getters[i],"()Ljava/io/File;");
        jobject dir=getter?(*env)->CallObjectMethodA(env,receiver,getter,NULL):NULL;
        if(!dir||(*env)->ExceptionCheck(env)||!(*env)->IsInstanceOf(env,dir,file))goto done;
        jstring path=(jstring)(*env)->CallObjectMethodA(env,dir,absolute,NULL);
        jstring raw=(jstring)(*env)->CallObjectMethodA(env,dir,getpath,NULL);
        if(!path||!raw||(*env)->ExceptionCheck(env))goto done;
        const char *a=(*env)->GetStringUTFChars(env,path,NULL);
        const char *b=(*env)->GetStringUTFChars(env,raw,NULL);
        struct stat st;
        bool valid=a&&b&&!strcmp(a,expected_dirs[i])&&!strcmp(a,b)&&!stat(a,&st)&&S_ISDIR(st.st_mode);
        if(b)(*env)->ReleaseStringUTFChars(env,raw,b);
        if(a)(*env)->ReleaseStringUTFChars(env,path,a);
        if(!valid||(*env)->ExceptionCheck(env))goto done;
    }
    sdk_trace_fprintf(stderr,"JNI_CONTEXT_DIRECTORIES_SELFTEST PASS actual_JNI=1 files_cache_distinct=1 host_stat=1\n");
    ok=true;
done:
    sdk_trace_fprintf(stderr,"JNI_ACTIVITY_GET_INTENT_SELFTEST %s stage=%s actual_JNI=1 bound_identity=%d intent_type=%d empty_extras_null=%d\n",ok?"PASS":"FAIL",stage,ok?1:0,ok?1:0,ok?1:0);
    if((*env)->ExceptionCheck(env))(*env)->ExceptionClear(env);
    (*env)->PopLocalFrame(env,NULL);
    return ok;
}
