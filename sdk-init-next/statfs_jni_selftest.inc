/* Include once in jni_activity_selftest.c (which already includes jni_bridge.h).
 * Call after bridge + original DEX loading, before the first engine StatFs
 * lookup, with the existing private cache directory. No synthetic SDK path,
 * substitute class, or direct file_runtime_call is used. Caller holds GIL. */
#include <sys/statvfs.h>
#include <unistd.h>

bool jni_statfs_reflection_selftest(struct dvm *vm,const char *path){
 struct jni_bridge *bridge=vm?jni_bridge_for_vm(vm):NULL;
 JNIEnv *env=bridge?jni_bridge_env(bridge):NULL;
 FILE *report=fopen("/data/storage/el2/base/haps/entry/files/sdkdex/statfs-jni-selftest.txt","w");
 if(report){fprintf(report,"STATFS_JNI_REFLECTION_SELFTEST BEGIN env=%d path_present=%d\n",env!=NULL,path!=NULL);fflush(report);}
 if(!env||!path||(*env)->ExceptionCheck(env)){
  if(report){fprintf(report,"FAIL stage=entry pending=%d\n",env?(*env)->ExceptionCheck(env):0);fclose(report);}return false;
 }
 if((*env)->PushLocalFrame(env,160)<0){if(report){fprintf(report,"FAIL stage=local_frame pending=%d\n",(*env)->ExceptionCheck(env));fclose(report);}return false;}
 bool ok=false,cold_enumeration=false,helper_resolved=false;unsigned checks=0;const char *stage="class_lookup";
 struct statvfs before,after;
 static const char *names[]={"restat","getBlockSizeLong","getBlockCountLong",
  "getAvailableBlocksLong","getFreeBlocksLong","getAvailableBytes","getFreeBytes",
  "getTotalBytes","getBlockSize","getBlockCount","getAvailableBlocks","getFreeBlocks"};
 static const char *returns[]={"void","long","long","long","long","long","long","long","int","int","int","int"};
 unsigned seen=0;
#define STATFS_JNI_CHECK(label,expression) do{stage=label;if(report){fprintf(report,"STAGE name=%s checks=%u\n",stage,checks);fflush(report);}if(!(expression)||(*env)->ExceptionCheck(env))goto done;++checks;}while(0)
 jclass cls=(*env)->FindClass(env,"android/os/StatFs");
 jclass cc=(*env)->FindClass(env,"java/lang/Class");
 jclass ctor_class=(*env)->FindClass(env,"java/lang/reflect/Constructor");
 jclass method_class=(*env)->FindClass(env,"java/lang/reflect/Method");
 STATFS_JNI_CHECK("classes",cls&&cc&&ctor_class&&method_class);
 jmethodID constructors=(*env)->GetMethodID(env,cc,"getConstructors","()[Ljava/lang/reflect/Constructor;");
 STATFS_JNI_CHECK("getConstructors_id",constructors!=NULL);
 jmethodID methods=(*env)->GetMethodID(env,cc,"getDeclaredMethods","()[Ljava/lang/reflect/Method;");
 STATFS_JNI_CHECK("getDeclaredMethods_id",methods!=NULL);
 jmethodID class_name=(*env)->GetMethodID(env,cc,"getName","()Ljava/lang/String;");
 STATFS_JNI_CHECK("class_getName_id",class_name!=NULL);
 jmethodID ctor_params=(*env)->GetMethodID(env,ctor_class,"getParameterTypes","()[Ljava/lang/Class;");
 STATFS_JNI_CHECK("constructor_getParameterTypes_id",ctor_params!=NULL);
 jmethodID ctor_mods=(*env)->GetMethodID(env,ctor_class,"getModifiers","()I");
 STATFS_JNI_CHECK("constructor_getModifiers_id",ctor_mods!=NULL);
 jmethodID method_name=(*env)->GetMethodID(env,method_class,"getName","()Ljava/lang/String;");
 STATFS_JNI_CHECK("method_getName_id",method_name!=NULL);
 jmethodID method_params=(*env)->GetMethodID(env,method_class,"getParameterTypes","()[Ljava/lang/Class;");
 STATFS_JNI_CHECK("method_getParameterTypes_id",method_params!=NULL);
 jmethodID method_return=(*env)->GetMethodID(env,method_class,"getReturnType","()Ljava/lang/Class;");
 STATFS_JNI_CHECK("method_getReturnType_id",method_return!=NULL);
 jmethodID method_mods=(*env)->GetMethodID(env,method_class,"getModifiers","()I");
 STATFS_JNI_CHECK("method_getModifiers_id",method_mods!=NULL);
 STATFS_JNI_CHECK("reflection_methods",constructors&&methods&&class_name&&ctor_params&&ctor_mods&&method_name&&method_params&&method_return&&method_mods);
 /* Enumerate before StatFs GetMethodID: lazy lookup must not mask absence. */
 jobjectArray ctors=(jobjectArray)(*env)->CallObjectMethodA(env,cls,constructors,NULL);
 STATFS_JNI_CHECK("cold_constructor_enumeration",ctors&&(*env)->GetArrayLength(env,ctors)==1);
 cold_enumeration=true;
 jobject ctor=(*env)->GetObjectArrayElement(env,ctors,0);
 jobjectArray params=(jobjectArray)(*env)->CallObjectMethodA(env,ctor,ctor_params,NULL);
 jint modifiers=(*env)->CallIntMethodA(env,ctor,ctor_mods,NULL);
 STATFS_JNI_CHECK("constructor_shape",ctor&&params&&(*env)->GetArrayLength(env,params)==1&&(modifiers&1)&&!(modifiers&8));
 jobject p=(*env)->GetObjectArrayElement(env,params,0);
 jstring pname=(jstring)(*env)->CallObjectMethodA(env,p,class_name,NULL);
 STATFS_JNI_CHECK("constructor_parameter_class",pname!=NULL);
 const char *pn=(*env)->GetStringUTFChars(env,pname,NULL);
 bool string_parameter=pn&&!strcmp(pn,"java.lang.String");
 if(pn)(*env)->ReleaseStringUTFChars(env,pname,pn);
 STATFS_JNI_CHECK("constructor_string_parameter",string_parameter);
 jobjectArray declared=(jobjectArray)(*env)->CallObjectMethodA(env,cls,methods,NULL);
 STATFS_JNI_CHECK("declared_method_count",declared&&(*env)->GetArrayLength(env,declared)==12);
 for(jsize i=0;i<12;i++){
  jobject method=(*env)->GetObjectArrayElement(env,declared,i);
  jstring name=(jstring)(*env)->CallObjectMethodA(env,method,method_name,NULL);
  STATFS_JNI_CHECK("declared_name",name!=NULL);
  const char *mn=(*env)->GetStringUTFChars(env,name,NULL);int match=-1;
  if(mn)for(int j=0;j<12;j++)if(!strcmp(mn,names[j])){match=j;break;}
  if(mn)(*env)->ReleaseStringUTFChars(env,name,mn);
  STATFS_JNI_CHECK("declared_unique_name",match>=0&&!(seen&(1u<<match)));
  jobjectArray mp=(jobjectArray)(*env)->CallObjectMethodA(env,method,method_params,NULL);
  jobject rt=(*env)->CallObjectMethodA(env,method,method_return,NULL);
  jint flags=(*env)->CallIntMethodA(env,method,method_mods,NULL);
  STATFS_JNI_CHECK("declared_public_instance",mp&&rt&&(flags&1)&&!(flags&8)&&(*env)->GetArrayLength(env,mp)==(match==0?1:0));
  jstring rn=(jstring)(*env)->CallObjectMethodA(env,rt,class_name,NULL);
  STATFS_JNI_CHECK("declared_return_name",rn!=NULL);
  const char *rname=(*env)->GetStringUTFChars(env,rn,NULL);
  bool return_ok=rname&&!strcmp(rname,returns[match]);
  if(report){fprintf(report,"RETURN_TYPE class=android.os.StatFs method=%s expected=%s actual=%s\n",names[match],returns[match],rname?rname:"(null)");fflush(report);}
  if(rname)(*env)->ReleaseStringUTFChars(env,rn,rname);
  STATFS_JNI_CHECK("declared_return_type",return_ok);
  if(match==0){
   jobject param=(*env)->GetObjectArrayElement(env,mp,0);
   jstring type=(jstring)(*env)->CallObjectMethodA(env,param,class_name,NULL);
   STATFS_JNI_CHECK("restat_parameter_name",type!=NULL);
   const char *tn=(*env)->GetStringUTFChars(env,type,NULL);
   bool type_ok=tn&&!strcmp(tn,"java.lang.String");
   if(tn)(*env)->ReleaseStringUTFChars(env,type,tn);
   STATFS_JNI_CHECK("restat_parameter_string",type_ok);
  }
  seen|=1u<<match;
 }
 STATFS_JNI_CHECK("all_declarations",seen==0xfffu);
 jclass helper=(*env)->FindClass(env,"com/unity3d/player/ReflectionHelper");
 jmethodID resolve=helper?(*env)->GetStaticMethodID(env,helper,"getConstructorID","(Ljava/lang/Class;Ljava/lang/String;)Ljava/lang/reflect/Constructor;"):NULL;
 STATFS_JNI_CHECK("original_reflection_helper",helper&&resolve);
 jvalue a[2]={{.l=cls},{.l=(*env)->NewStringUTF(env,"(Ljava/lang/String;)V")}};
 jobject resolved=(*env)->CallStaticObjectMethodA(env,helper,resolve,a);
 STATFS_JNI_CHECK("original_constructor_resolution",resolved!=NULL);
 helper_resolved=true;
 jmethodID reflected_id=(*env)->FromReflectedMethod(env,resolved);
 jmethodID direct_id=(*env)->GetMethodID(env,cls,"<init>","(Ljava/lang/String;)V");
 STATFS_JNI_CHECK("stable_constructor_identity",reflected_id&&direct_id==reflected_id&&(*env)->FromReflectedMethod(env,ctor)==reflected_id);
 a[0].l=(*env)->NewStringUTF(env,path);
 STATFS_JNI_CHECK("host_statvfs_before",a[0].l&&statvfs(path,&before)==0);
 jobject object=(*env)->NewObjectA(env,cls,reflected_id,a);
 STATFS_JNI_CHECK("real_constructor",object!=NULL);
 jmethodID available=(*env)->GetMethodID(env,cls,"getAvailableBytes","()J");
 jmethodID blocks=(*env)->GetMethodID(env,cls,"getAvailableBlocksLong","()J");
 jmethodID block_size=(*env)->GetMethodID(env,cls,"getBlockSizeLong","()J");
 STATFS_JNI_CHECK("real_getters",available&&blocks&&block_size);
 jlong bytes=(*env)->CallLongMethodA(env,object,available,NULL);
 jlong count=(*env)->CallLongMethodA(env,object,blocks,NULL);
 jlong size=(*env)->CallLongMethodA(env,object,block_size,NULL);
 STATFS_JNI_CHECK("host_statvfs_after",statvfs(path,&after)==0);
 uint64_t b=(uint64_t)before.f_frsize*before.f_bavail,e=(uint64_t)after.f_frsize*after.f_bavail;
 STATFS_JNI_CHECK("real_filesystem_snapshot",size>0&&(uint64_t)size==before.f_frsize&&(uint64_t)size==after.f_frsize&&count>=0&&bytes>=0&&(uint64_t)bytes==(uint64_t)count*(uint64_t)size&&(uint64_t)bytes>=(b<e?b:e)&&(uint64_t)bytes<=(b>e?b:e));
 ok=true;
done:
 /* Consume pending only at the existing ExceptionClear cleanup boundary.
  * No guest calls are made to format the original failure. */
 {bool pending=(*env)->ExceptionCheck(env);char detail[1024]="-";
  dvm_ref exception=dvm_exception(vm),jni_exception=pending?jni_bridge_take_pending_exception(NULL,vm):0;
  if(!exception)exception=jni_exception;
  if(exception)dvm_describe_exception(vm,exception,detail,sizeof detail);
  if(report){fprintf(report,"STATFS_JNI_REFLECTION_SELFTEST %s stage=%s checks=%u pending=%d exception=%s actual_JNI=1 original_ReflectionHelper=%d cold_enumeration=%d\n",ok?"PASS":"FAIL",stage,checks,pending?1:0,detail,helper_resolved?1:0,cold_enumeration?1:0);fflush(report);fsync(fileno(report));fclose(report);}
 }
 sdk_trace_fprintf(stderr,"STATFS_JNI_REFLECTION_SELFTEST %s stage=%s checks=%u actual_JNI=1 original_ReflectionHelper=%d cold_enumeration=%d\n",ok?"PASS":"FAIL",stage,checks,helper_resolved?1:0,cold_enumeration?1:0);
 if((*env)->ExceptionCheck(env))(*env)->ExceptionClear(env);
 (*env)->PopLocalFrame(env,NULL);
 return ok;
#undef STATFS_JNI_CHECK
}
