#include "jni_bridge.h"
#include "dvm/dvm_internal.h"
#include <stdarg.h>
#include <string.h>
#include <stdio.h>
extern int sdk_trace_fprintf(FILE *,const char *,...);

static jobject call_static_v(JNIEnv *env, jclass cls, jmethodID method, ...)
{
    va_list ap; va_start(ap, method);
    jobject result = (*env)->CallStaticObjectMethodV(env, cls, method, ap);
    va_end(ap); return result;
}

static jobject call_object_v(JNIEnv *env, jobject obj, jmethodID method, ...)
{
    va_list ap; va_start(ap, method);
    jobject result = (*env)->CallObjectMethodV(env, obj, method, ap);
    va_end(ap); return result;
}

/* Run in an attached thread after discovery and JNI bridge setup. */
static bool wrapper_roundtrip_selftest(JNIEnv *env){
    static const struct {const char *name;char type;const char *getter;} shapes[]={
        {"java/lang/Boolean",'Z',"booleanValue"},{"java/lang/Byte",'B',"byteValue"},
        {"java/lang/Character",'C',"charValue"},{"java/lang/Short",'S',"shortValue"},
        {"java/lang/Integer",'I',"intValue"},{"java/lang/Long",'J',"longValue"},
        {"java/lang/Float",'F',"floatValue"},{"java/lang/Double",'D',"doubleValue"}};
    for(unsigned i=0;i<sizeof shapes/sizeof shapes[0];i++){
        char ctor_sig[8],getter_sig[8],factory_sig[96];
        snprintf(ctor_sig,sizeof ctor_sig,"(%c)V",shapes[i].type);
        snprintf(getter_sig,sizeof getter_sig,"()%c",shapes[i].type);
        snprintf(factory_sig,sizeof factory_sig,"(%c)L%s;",shapes[i].type,shapes[i].name);
        jclass cls=(*env)->FindClass(env,shapes[i].name);
        jmethodID ctor=cls?(*env)->GetMethodID(env,cls,"<init>",ctor_sig):NULL;
        jmethodID getter=cls?(*env)->GetMethodID(env,cls,shapes[i].getter,getter_sig):NULL;
        jmethodID factory=cls?(*env)->GetStaticMethodID(env,cls,"valueOf",factory_sig):NULL;
        if(!ctor||!getter||!factory||(*env)->ExceptionCheck(env))return false;
        jvalue val={0};
        switch(shapes[i].type){case 'Z':val.z=JNI_TRUE;break;case 'B':val.b=-7;break;case 'C':val.c=0x4e2d;break;case 'S':val.s=-12345;break;case 'I':val.i=-1234567;break;case 'J':val.j=INT64_C(0x123456789abc);break;case 'F':val.f=1.25f;break;case 'D':val.d=-2.5;break;}
        jobject objects[2]={(*env)->NewObjectA(env,cls,ctor,&val),(*env)->CallStaticObjectMethodA(env,cls,factory,&val)};
        for(unsigned j=0;j<2;j++){
            if(!objects[j]||(*env)->ExceptionCheck(env))return false;
            bool matches=false;
            switch(shapes[i].type){
            case 'Z':matches=(*env)->CallBooleanMethodA(env,objects[j],getter,NULL)==val.z;break;
            case 'B':matches=(*env)->CallByteMethodA(env,objects[j],getter,NULL)==val.b;break;
            case 'C':matches=(*env)->CallCharMethodA(env,objects[j],getter,NULL)==val.c;break;
            case 'S':matches=(*env)->CallShortMethodA(env,objects[j],getter,NULL)==val.s;break;
            case 'I':matches=(*env)->CallIntMethodA(env,objects[j],getter,NULL)==val.i;break;
            case 'J':matches=(*env)->CallLongMethodA(env,objects[j],getter,NULL)==val.j;break;
            case 'F':matches=(*env)->CallFloatMethodA(env,objects[j],getter,NULL)==val.f;break;
            case 'D':matches=(*env)->CallDoubleMethodA(env,objects[j],getter,NULL)==val.d;break;
            }
            if(!matches||(*env)->ExceptionCheck(env))return false;
            (*env)->DeleteLocalRef(env,objects[j]);
        }
        (*env)->DeleteLocalRef(env,cls);
    }
    sdk_trace_fprintf(stderr,"JNI_WRAPPER_ROUNDTRIP PASS types=8 constructor=1 factory=1 getter=1 wide64=1\n");return true;
}
bool jni_forname_selftest(JNIEnv *env)
{
    if (!env || (*env)->ExceptionCheck(env)) return false;
    bool ok = false;
    const char *stage="lookup";
    if ((*env)->PushLocalFrame(env, 96) < 0) return false;
    jclass cls = (*env)->FindClass(env, "java/lang/Class");
    jmethodID method = cls ? (*env)->GetStaticMethodID(env, cls, "forName",
        "(Ljava/lang/String;ZLjava/lang/ClassLoader;)Ljava/lang/Class;") : NULL;
    jstring name = (*env)->NewStringUTF(env, "java.lang.String");
    if (!method || !name || (*env)->ExceptionCheck(env)) goto done;
    jvalue args[3] = {{.l = name}, {.z = JNI_FALSE}, {.l = NULL}};
    stage="invoke_A_V";
    jobject a = (*env)->CallStaticObjectMethodA(env, cls, method, args);
    jobject b = call_static_v(env, cls, method, name, (int)JNI_TRUE, (jobject)NULL);
    if (!a || !b || !(*env)->IsSameObject(env, a, b) || (*env)->ExceptionCheck(env)) goto done;
    sdk_trace_fprintf(stderr,"JNI_FORNAME_SELFTEST forName_A_V=PASS\n");
    stage="object_getClass";
    jclass object_class=(*env)->FindClass(env,"java/lang/Object");
    jmethodID get_class=object_class?(*env)->GetMethodID(env,object_class,"getClass","()Ljava/lang/Class;"):NULL;
    jobject actual_class=get_class?(*env)->CallObjectMethodA(env,name,get_class,NULL):NULL;
    jclass expected_class=(*env)->GetObjectClass(env,name);
    if(!actual_class||!expected_class||!(*env)->IsSameObject(env,actual_class,expected_class)||(*env)->ExceptionCheck(env))goto done;
    sdk_trace_fprintf(stderr,"JNI_FORNAME_SELFTEST object_getClass=PASS actual_JNI=1\n");
    stage="object_reflection_metadata";
    jmethodID declared=(*env)->GetMethodID(env,cls,"getDeclaredMethods","()[Ljava/lang/reflect/Method;");
    jclass method_class=(*env)->FindClass(env,"java/lang/reflect/Method");
    jmethodID method_name=method_class?(*env)->GetMethodID(env,method_class,"getName","()Ljava/lang/String;"):NULL;
    jmethodID return_type=method_class?(*env)->GetMethodID(env,method_class,"getReturnType","()Ljava/lang/Class;"):NULL;
    jobjectArray members=declared?(*env)->CallObjectMethodA(env,object_class,declared,NULL):NULL;
    if(!members||!method_name||!return_type||(*env)->ExceptionCheck(env))goto done;
    bool found_get_class=false;
    for(jsize i=0;i<(*env)->GetArrayLength(env,members);++i){
        jobject member=(*env)->GetObjectArrayElement(env,members,i);
        jstring text=member?(*env)->CallObjectMethodA(env,member,method_name,NULL):NULL;
        const char *chars=text?(*env)->GetStringUTFChars(env,text,NULL):NULL;
        bool match=chars&&!strcmp(chars,"getClass");
        if(chars)(*env)->ReleaseStringUTFChars(env,text,chars);
        if(match){
            jobject type=(*env)->CallObjectMethodA(env,member,return_type,NULL);
            if(!type||!(*env)->IsSameObject(env,type,cls)||(*env)->FromReflectedMethod(env,member)!=get_class)goto done;
            (*env)->DeleteLocalRef(env,type);found_get_class=true;
        }
        if(text)(*env)->DeleteLocalRef(env,text);
        if(member)(*env)->DeleteLocalRef(env,member);
        if((*env)->ExceptionCheck(env))goto done;
    }
    if(!found_get_class)goto done;
    (*env)->DeleteLocalRef(env,members);
    sdk_trace_fprintf(stderr,"JNI_OBJECT_REFLECTION_SELFTEST PASS enumerated_getClass=1 return_Class=1 canonical_ID=1\n");
    stage="class_loader";
    jmethodID get_loader = (*env)->GetMethodID(env, cls, "getClassLoader", "()Ljava/lang/ClassLoader;");
    jobject loader = get_loader ? (*env)->CallObjectMethodA(env, a, get_loader, NULL) : NULL;
    if (!loader || (*env)->ExceptionCheck(env)) goto done;
    jclass loader_class = (*env)->FindClass(env, "java/lang/ClassLoader");
    jclass activity_class = (*env)->FindClass(env, "android/app/Activity");
    if (!loader_class || !activity_class || (*env)->ExceptionCheck(env) ||
        !(*env)->IsInstanceOf(env, loader, loader_class)) goto done;
    args[2].l = loader;
    jobject from_loader = (*env)->CallStaticObjectMethodA(env, cls, method, args);
    if (!from_loader || !(*env)->IsSameObject(env, a, from_loader) || (*env)->ExceptionCheck(env)) goto done;
    args[2].l = NULL;
    stage="string_builder";
    jclass builder = (*env)->FindClass(env, "java/lang/StringBuilder");
    jmethodID init = builder ? (*env)->GetMethodID(env, builder, "<init>", "()V") : NULL;
    jmethodID append = builder ? (*env)->GetMethodID(env, builder, "append", "(Ljava/lang/String;)Ljava/lang/StringBuilder;") : NULL;
    jmethodID build = builder ? (*env)->GetMethodID(env, builder, "toString", "()Ljava/lang/String;") : NULL;
    if (!init || !append || !build || (*env)->ExceptionCheck(env)) goto done;
    jobject sb = (*env)->NewObjectA(env, builder, init, NULL);
    jvalue append_arg[1] = {{.l = name}};
    jobject appended = sb ? (*env)->CallObjectMethodA(env, sb, append, append_arg) : NULL;
    jstring result = appended ? (jstring)(*env)->CallObjectMethodA(env, appended, build, NULL) : NULL;
    if (!result || !(*env)->IsSameObject(env, sb, appended) || (*env)->ExceptionCheck(env)) goto done;
    const char *built = (*env)->GetStringUTFChars(env, result, NULL);
    if (!built) goto done;
    bool builder_ok = !strcmp(built, "java.lang.String");
    (*env)->ReleaseStringUTFChars(env, result, built);
    if (!builder_ok) goto done;
    sdk_trace_fprintf(stderr,"JNI_FORNAME_SELFTEST loader_builder=PASS\n");
    stage="null_string_without_pending";
    if ((*env)->GetStringUTFChars(env,NULL,NULL) || !(*env)->ExceptionCheck(env)) goto done;
    (*env)->ExceptionClear(env);
    stage="non_string_handle";
    if ((*env)->GetStringUTFChars(env,(jstring)a,NULL) || !(*env)->ExceptionCheck(env)) goto done;
    (*env)->ExceptionClear(env);
    stage="missing_class";
    args[0].l = (*env)->NewStringUTF(env, "not.existing.JniForNameProbe");
    if (!args[0].l) goto done;
    if ((*env)->CallStaticObjectMethodA(env, cls, method, args) || !(*env)->ExceptionCheck(env)) goto done;
    jthrowable exception = (*env)->ExceptionOccurred(env);
    if (!exception) goto done;
    /* A pending exception must survive a rejected null string conversion. */
    if ((*env)->GetStringUTFChars(env, NULL, NULL)) goto done;
    jthrowable still = (*env)->ExceptionOccurred(env);
    if (!still || !(*env)->IsSameObject(env, exception, still)) goto done;
    (*env)->ExceptionClear(env);
    stage="throwable_toString";
    jclass object = (*env)->FindClass(env, "java/lang/Object");
    jmethodID str = object ? (*env)->GetMethodID(env, object, "toString", "()Ljava/lang/String;") : NULL;
    if (!str || (*env)->ExceptionCheck(env)) goto done;
    jobject text = call_object_v(env, exception, str);
    if (!text || (*env)->ExceptionCheck(env)) goto done;
    const char *utf = (*env)->GetStringUTFChars(env, (jstring)text, NULL);
    if (!utf) goto done;
    ok = strstr(utf, "ClassNotFoundException") != NULL;
    (*env)->ReleaseStringUTFChars(env, (jstring)text, utf);
    if (!ok) goto done;
    /* External static constant reached through the JNI field path.  This is
     * the same lookup Unity's native PlayerPrefs upgrade performs; a failure
     * here must fail the probe instead of silently returning 0. */
    stage="context_mode_private";
    jclass context = (*env)->FindClass(env, "android/content/Context");
    jfieldID mode_private = context ? (*env)->GetStaticFieldID(env, context,
        "MODE_PRIVATE", "I") : NULL;
    if (!mode_private || (*env)->ExceptionCheck(env)) { ok = false; goto done; }
    jint mode_value = (*env)->GetStaticIntField(env, context, mode_private);
    if ((*env)->ExceptionCheck(env) || mode_value != 0) { ok = false; goto done; }
    sdk_trace_fprintf(stderr,"JNI_EXTERNAL_STATIC_SELFTEST MODE_PRIVATE=0 contract=PASS\n");
    /* The dispatcher and its package_metadata selftest already provide a
     * real ApplicationInfo; this verifies the JNI lookup path the native
     * engine uses to reach it. */
    stage="context_get_application_info";
    jmethodID get_app_info = context ? (*env)->GetMethodID(env, context,
        "getApplicationInfo", "()Landroid/content/pm/ApplicationInfo;") : NULL;
    if (!get_app_info || (*env)->ExceptionCheck(env)) { ok = false; goto done; }
    sdk_trace_fprintf(stderr,"JNI_CONTEXT_APPINFO_SELFTEST method_id=PASS\n");
    stage="log_stack_trace_static";
    jclass log_class=(*env)->FindClass(env,"android/util/Log");
    jmethodID get_trace=log_class?(*env)->GetStaticMethodID(env,log_class,"getStackTraceString","(Ljava/lang/Throwable;)Ljava/lang/String;"):NULL;
    jvalue null_throwable[1]={{.l=NULL}};
    jstring empty=get_trace?(*env)->CallStaticObjectMethodA(env,log_class,get_trace,null_throwable):NULL;
    if(!empty||(*env)->ExceptionCheck(env)){ok=false;goto done;}
    const char *empty_text=(*env)->GetStringUTFChars(env,empty,NULL);
    bool trace_ok=empty_text&&!*empty_text;
    if(empty_text)(*env)->ReleaseStringUTFChars(env,empty,empty_text);
    if(!trace_ok||(*env)->ExceptionCheck(env)){ok=false;goto done;}
    sdk_trace_fprintf(stderr,"JNI_LOG_STACK_SELFTEST static_null_empty=PASS\n");
    stage="reflected_method_roundtrip";
    jobject reflected=(*env)->ToReflectedMethod(env,object_class,get_class,JNI_FALSE);
    jmethodID roundtrip=reflected?(*env)->FromReflectedMethod(env,reflected):NULL;
    jobject round_class=roundtrip?(*env)->CallObjectMethodA(env,name,roundtrip,NULL):NULL;
    if(!reflected||roundtrip!=get_class||!round_class||!(*env)->IsSameObject(env,round_class,expected_class)||(*env)->ExceptionCheck(env)){ok=false;goto done;}
    jobject reflected_static=(*env)->ToReflectedMethod(env,cls,method,JNI_TRUE);
    jmethodID static_round=reflected_static?(*env)->FromReflectedMethod(env,reflected_static):NULL;
    jvalue static_args[3]={{.l=name},{.z=JNI_FALSE},{.l=NULL}};
    jobject static_result=static_round?(*env)->CallStaticObjectMethodA(env,cls,static_round,static_args):NULL;
    if(static_round!=method||!static_result||!(*env)->IsSameObject(env,static_result,a)||(*env)->ExceptionCheck(env)){ok=false;goto done;}
    stage="reflected_constructor_roundtrip";
    jobject reflected_ctor=(*env)->ToReflectedMethod(env,builder,init,JNI_FALSE);
    jclass ctor_class=(*env)->FindClass(env,"java/lang/reflect/Constructor");
    jmethodID ctor_round=reflected_ctor?(*env)->FromReflectedMethod(env,reflected_ctor):NULL;
    jobject constructed=ctor_round?(*env)->NewObjectA(env,builder,ctor_round,NULL):NULL;
    if(!ctor_class||!reflected_ctor||!(*env)->IsInstanceOf(env,reflected_ctor,ctor_class)||ctor_round!=init||!constructed||(*env)->ExceptionCheck(env)){ok=false;goto done;}
    stage="reflected_invalid_id";
    if((*env)->ToReflectedMethod(env,object_class,(jmethodID)(uintptr_t)1,JNI_FALSE)||!(*env)->ExceptionCheck(env)){ok=false;goto done;}
    (*env)->ExceptionClear(env);
    if((*env)->FromReflectedMethod(env,name)||!(*env)->ExceptionCheck(env)){ok=false;goto done;}
    (*env)->ExceptionClear(env);
    sdk_trace_fprintf(stderr,"JNI_REFLECTION_ROUNDTRIP PASS instance=1 static=1 constructor=1 identity=1 invalid_id=1\n");
    stage="primitive_wrapper_roundtrip";
    if(!wrapper_roundtrip_selftest(env)){ok=false;goto done;}
    stage="message_family";
    const char *family[]={"android/os/Handler","android/os/HandlerThread","android/os/Looper","android/os/Message","android/os/MessageQueue","java/lang/Thread","android/view/Choreographer"};
    for(unsigned i=0;i<sizeof family/sizeof family[0];i++){
        jclass type=(*env)->FindClass(env,family[i]);if(!type||(*env)->ExceptionCheck(env)){ok=false;goto done;}(*env)->DeleteLocalRef(env,type);
    }
    jclass lc=(*env)->FindClass(env,"android/os/Looper");
    jmethodID main_id=lc?(*env)->GetStaticMethodID(env,lc,"getMainLooper","()Landroid/os/Looper;"):NULL;
    jmethodID my_queue=lc?(*env)->GetStaticMethodID(env,lc,"myQueue","()Landroid/os/MessageQueue;"):NULL;
    jmethodID get_queue=lc?(*env)->GetMethodID(env,lc,"getQueue","()Landroid/os/MessageQueue;"):NULL;
    jobject main=main_id?(*env)->CallStaticObjectMethodA(env,lc,main_id,NULL):NULL;
    jobject queue=main&&get_queue?(*env)->CallObjectMethodA(env,main,get_queue,NULL):NULL;
    jobject same_queue=my_queue?(*env)->CallStaticObjectMethodA(env,lc,my_queue,NULL):NULL;
    if(!queue||!same_queue||!(*env)->IsSameObject(env,queue,same_queue)||(*env)->ExceptionCheck(env)){ok=false;goto done;}
    jclass qc=(*env)->FindClass(env,"android/os/MessageQueue");
    jmethodID idle=qc?(*env)->GetMethodID(env,qc,"isIdle","()Z"):NULL;
    if(!idle){ok=false;goto done;}(*env)->CallBooleanMethodA(env,queue,idle,NULL);
    if((*env)->ExceptionCheck(env)){ok=false;goto done;}
    sdk_trace_fprintf(stderr,"JNI_MESSAGE_FAMILY PASS classes=7 queue_identity=1 real_queue_query=1\n");
    stage="json_virtual_tostring";
    jclass json_class=(*env)->FindClass(env,"org/json/JSONObject");
    jmethodID json_ctor=json_class?(*env)->GetMethodID(env,json_class,"<init>","()V"):NULL;
    jmethodID json_put=json_class?(*env)->GetMethodID(env,json_class,"put","(Ljava/lang/String;I)Lorg/json/JSONObject;"):NULL;
    jmethodID object_text=(*env)->GetMethodID(env,object_class,"toString","()Ljava/lang/String;");
    jobject json_obj=json_ctor?(*env)->NewObjectA(env,json_class,json_ctor,NULL):NULL;
    jvalue json_args[2]={{.l=(*env)->NewStringUTF(env,"probe")},{.i=7}};
    jobject json_same=json_obj&&json_put?(*env)->CallObjectMethodA(env,json_obj,json_put,json_args):NULL;
    jstring json_string=json_same&&object_text?(*env)->CallObjectMethodA(env,json_obj,object_text,NULL):NULL;
    if(!json_string||!(*env)->IsSameObject(env,json_obj,json_same)||(*env)->ExceptionCheck(env)){ok=false;goto done;}
    const char *json_text=(*env)->GetStringUTFChars(env,json_string,NULL);
    bool json_ok=json_text&&!strcmp(json_text,"{\"probe\":7}");
    if(json_text)(*env)->ReleaseStringUTFChars(env,json_string,json_text);
    if(!json_ok||(*env)->ExceptionCheck(env)){ok=false;goto done;}
    sdk_trace_fprintf(stderr,"JNI_JSON_VIRTUAL_SELFTEST PASS Object_toString_override=1 real_serialization=1\n");
done:
    sdk_trace_fprintf(stderr,"JNI_FORNAME_SELFTEST %s stage=%s actual_JNI=1\n",ok?"PASS":"FAIL",stage);
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    (*env)->PopLocalFrame(env, NULL);
    return ok;
}

#include "jni_throwable_pressure_selftest.inc"
