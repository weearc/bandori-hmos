#include "jni_bridge.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
extern int sdk_trace_fprintf(FILE *,const char *,...);
#include "statfs_jni_selftest.inc"

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

/* Synthetic keys/values only; no business credentials are read or logged. */
bool jni_hashmap_dispatch_selftest(struct dvm *vm)
{
 struct jni_bridge *bridge=vm?jni_bridge_for_vm(vm):NULL;
 JNIEnv *env=bridge?jni_bridge_env(bridge):NULL;
 if(!env||(*env)->ExceptionCheck(env))return false;
 if((*env)->PushLocalFrame(env,48)<0)return false;
 bool ok=false;unsigned checks=0;const char *stage="lookup";
 jclass hc=(*env)->FindClass(env,"java/util/HashMap");
 jclass mc=(*env)->FindClass(env,"java/util/Map");
 jmethodID ctor=hc?(*env)->GetMethodID(env,hc,"<init>","()V"):NULL;
 jmethodID put=hc?(*env)->GetMethodID(env,hc,"put","(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;"):NULL;
 jmethodID get=hc?(*env)->GetMethodID(env,hc,"get","(Ljava/lang/Object;)Ljava/lang/Object;"):NULL;
 jmethodID iget=mc?(*env)->GetMethodID(env,mc,"get","(Ljava/lang/Object;)Ljava/lang/Object;"):NULL;
 jmethodID size=hc?(*env)->GetMethodID(env,hc,"size","()I"):NULL;
 jmethodID contains=hc?(*env)->GetMethodID(env,hc,"containsKey","(Ljava/lang/Object;)Z"):NULL;
 jmethodID empty=hc?(*env)->GetMethodID(env,hc,"isEmpty","()Z"):NULL;
 jmethodID entries=mc?(*env)->GetMethodID(env,mc,"entrySet","()Ljava/util/Set;"):NULL;
 jmethodID remove=hc?(*env)->GetMethodID(env,hc,"remove","(Ljava/lang/Object;)Ljava/lang/Object;"):NULL;
 jmethodID clear=hc?(*env)->GetMethodID(env,hc,"clear","()V"):NULL;
 if(!ctor||!put||!get||!iget||!size||!contains||!empty||!entries||!remove||!clear||(*env)->ExceptionCheck(env))goto done;
 #define MAP_JNI_CHECK(label,expr) do{stage=label;if(!(expr)||(*env)->ExceptionCheck(env))goto done;++checks;}while(0)
 jobject map=(*env)->NewObjectA(env,hc,ctor,NULL);
 MAP_JNI_CHECK("constructor",map!=NULL);
 MAP_JNI_CHECK("initial_empty",(*env)->CallBooleanMethodA(env,map,empty,NULL)==JNI_TRUE);
 jstring key=(*env)->NewStringUTF(env,"map-selftest-key");
 jstring value=(*env)->NewStringUTF(env,"map-selftest-value");
 jstring other=(*env)->NewStringUTF(env,"map-selftest-other");
 jstring equal_key=(*env)->NewStringUTF(env,"map-selftest-key");
 MAP_JNI_CHECK("strings",key&&value&&other&&equal_key);
 jvalue a[2]={{.l=key},{.l=value}};
 MAP_JNI_CHECK("put_new_null",(*env)->CallObjectMethodA(env,map,put,a)==NULL);
 MAP_JNI_CHECK("get_identity",(*env)->IsSameObject(env,(*env)->CallObjectMethodA(env,map,get,a),value));
 a[0].l=equal_key;
 MAP_JNI_CHECK("interface_get_equal_key",(*env)->IsSameObject(env,(*env)->CallObjectMethodA(env,map,iget,a),value));
 MAP_JNI_CHECK("contains",(*env)->CallBooleanMethodA(env,map,contains,a)==JNI_TRUE);
 MAP_JNI_CHECK("size_one",(*env)->CallIntMethodA(env,map,size,NULL)==1);
 a[1].l=other;
 MAP_JNI_CHECK("put_returns_previous",(*env)->IsSameObject(env,(*env)->CallObjectMethodA(env,map,put,a),value));
 MAP_JNI_CHECK("get_replaced",(*env)->IsSameObject(env,(*env)->CallObjectMethodA(env,map,get,a),other));
 MAP_JNI_CHECK("entrySet_interface",(*env)->CallObjectMethodA(env,map,entries,NULL)!=NULL);
 a[0].l=NULL;a[1].l=NULL;
 MAP_JNI_CHECK("null_mapping_put",(*env)->CallObjectMethodA(env,map,put,a)==NULL);
 MAP_JNI_CHECK("null_mapping_present",(*env)->CallBooleanMethodA(env,map,contains,a)==JNI_TRUE);
 MAP_JNI_CHECK("null_mapping_get",(*env)->CallObjectMethodA(env,map,get,a)==NULL);
 MAP_JNI_CHECK("size_two",(*env)->CallIntMethodA(env,map,size,NULL)==2);
 a[0].l=key;
 MAP_JNI_CHECK("remove_previous",(*env)->IsSameObject(env,(*env)->CallObjectMethodA(env,map,remove,a),other));
 MAP_JNI_CHECK("missing_get_null",(*env)->CallObjectMethodA(env,map,get,a)==NULL);
 (*env)->CallVoidMethodA(env,map,clear,NULL);
 MAP_JNI_CHECK("clear",(*env)->CallIntMethodA(env,map,size,NULL)==0&&(*env)->CallBooleanMethodA(env,map,empty,NULL)==JNI_TRUE);
 ok=true;
 #undef MAP_JNI_CHECK
 done:
 sdk_trace_fprintf(stderr,"JNI_HASHMAP_DISPATCH_SELFTEST %s stage=%s checks=%u actual_GetMethodID=1 actual_CallObjectMethod=1 no_business_values=1\n",ok?"PASS":"FAIL",stage,checks);
 FILE *f=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/jni-hashmap-dispatch-selftest.txt","a");
 if(f){fprintf(f,"JNI_HASHMAP_DISPATCH_SELFTEST %s stage=%s checks=%u actual_JNI=1\n",ok?"PASS":"FAIL",stage,checks);fclose(f);}
 if((*env)->ExceptionCheck(env))(*env)->ExceptionClear(env);
 (*env)->PopLocalFrame(env,NULL);return ok;
}
