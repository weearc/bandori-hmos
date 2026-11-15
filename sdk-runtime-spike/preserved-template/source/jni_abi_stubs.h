/* 本文件由脚本从 jni_abi.h 生成，请勿手工修改。
 * 为每个非 reserved 槽生成参数表完全匹配的占位函数，供桥初始化时先填充。
 * 这些占位函数不执行真实 JNI 操作，只设置 UnsatisfiedLinkError 并记录受限。 */
#ifndef NATIVE_JNI_NEXT_JNI_ABI_STUBS_H
#define NATIVE_JNI_NEXT_JNI_ABI_STUBS_H

#include "jni_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 由 jni_bridge.c 实现：设置 pending UnsatisfiedLinkError，返回被调用槽号。 */
int jni_abi_stub_fail(JNIEnv *env, int slot, const char *name);

static jint JNICALL jni_abi_stub_slot_4(JNIEnv *env)
{
    jni_abi_stub_fail(env, 4, "GetVersion");
    return 0;
}

static jclass JNICALL jni_abi_stub_slot_5(JNIEnv *env, const char *name, jobject loader, const jbyte *buf, jsize len)
{
    (void)name;
    (void)loader;
    (void)buf;
    (void)len;
    jni_abi_stub_fail(env, 5, "DefineClass");
    return 0;
}

static jclass JNICALL jni_abi_stub_slot_6(JNIEnv *env, const char *name)
{
    (void)name;
    jni_abi_stub_fail(env, 6, "FindClass");
    return 0;
}

static jmethodID JNICALL jni_abi_stub_slot_7(JNIEnv *env, jobject method)
{
    (void)method;
    jni_abi_stub_fail(env, 7, "FromReflectedMethod");
    return 0;
}

static jfieldID JNICALL jni_abi_stub_slot_8(JNIEnv *env, jobject field)
{
    (void)field;
    jni_abi_stub_fail(env, 8, "FromReflectedField");
    return 0;
}

static jobject JNICALL jni_abi_stub_slot_9(JNIEnv *env, jclass cls, jmethodID methodID, jboolean isStatic)
{
    (void)cls;
    (void)methodID;
    (void)isStatic;
    jni_abi_stub_fail(env, 9, "ToReflectedMethod");
    return 0;
}

static jclass JNICALL jni_abi_stub_slot_10(JNIEnv *env, jclass sub)
{
    (void)sub;
    jni_abi_stub_fail(env, 10, "GetSuperclass");
    return 0;
}

static jboolean JNICALL jni_abi_stub_slot_11(JNIEnv *env, jclass sub, jclass sup)
{
    (void)sub;
    (void)sup;
    jni_abi_stub_fail(env, 11, "IsAssignableFrom");
    return 0;
}

static jobject JNICALL jni_abi_stub_slot_12(JNIEnv *env, jclass cls, jfieldID fieldID, jboolean isStatic)
{
    (void)cls;
    (void)fieldID;
    (void)isStatic;
    jni_abi_stub_fail(env, 12, "ToReflectedField");
    return 0;
}

static jint JNICALL jni_abi_stub_slot_13(JNIEnv *env, jthrowable obj)
{
    (void)obj;
    jni_abi_stub_fail(env, 13, "Throw");
    return 0;
}

static jint JNICALL jni_abi_stub_slot_14(JNIEnv *env, jclass clazz, const char *msg)
{
    (void)clazz;
    (void)msg;
    jni_abi_stub_fail(env, 14, "ThrowNew");
    return 0;
}

static jthrowable JNICALL jni_abi_stub_slot_15(JNIEnv *env)
{
    jni_abi_stub_fail(env, 15, "ExceptionOccurred");
    return 0;
}

static void JNICALL jni_abi_stub_slot_16(JNIEnv *env)
{
    jni_abi_stub_fail(env, 16, "ExceptionDescribe");
}

static void JNICALL jni_abi_stub_slot_17(JNIEnv *env)
{
    jni_abi_stub_fail(env, 17, "ExceptionClear");
}

static void JNICALL jni_abi_stub_slot_18(JNIEnv *env, const char *msg)
{
    (void)msg;
    jni_abi_stub_fail(env, 18, "FatalError");
}

static jint JNICALL jni_abi_stub_slot_19(JNIEnv *env, jint capacity)
{
    (void)capacity;
    jni_abi_stub_fail(env, 19, "PushLocalFrame");
    return 0;
}

static jobject JNICALL jni_abi_stub_slot_20(JNIEnv *env, jobject result)
{
    (void)result;
    jni_abi_stub_fail(env, 20, "PopLocalFrame");
    return 0;
}

static jobject JNICALL jni_abi_stub_slot_21(JNIEnv *env, jobject lobj)
{
    (void)lobj;
    jni_abi_stub_fail(env, 21, "NewGlobalRef");
    return 0;
}

static void JNICALL jni_abi_stub_slot_22(JNIEnv *env, jobject gref)
{
    (void)gref;
    jni_abi_stub_fail(env, 22, "DeleteGlobalRef");
}

static void JNICALL jni_abi_stub_slot_23(JNIEnv *env, jobject obj)
{
    (void)obj;
    jni_abi_stub_fail(env, 23, "DeleteLocalRef");
}

static jboolean JNICALL jni_abi_stub_slot_24(JNIEnv *env, jobject obj1, jobject obj2)
{
    (void)obj1;
    (void)obj2;
    jni_abi_stub_fail(env, 24, "IsSameObject");
    return 0;
}

static jobject JNICALL jni_abi_stub_slot_25(JNIEnv *env, jobject ref)
{
    (void)ref;
    jni_abi_stub_fail(env, 25, "NewLocalRef");
    return 0;
}

static jint JNICALL jni_abi_stub_slot_26(JNIEnv *env, jint capacity)
{
    (void)capacity;
    jni_abi_stub_fail(env, 26, "EnsureLocalCapacity");
    return 0;
}

static jobject JNICALL jni_abi_stub_slot_27(JNIEnv *env, jclass clazz)
{
    (void)clazz;
    jni_abi_stub_fail(env, 27, "AllocObject");
    return 0;
}

static jobject JNICALL jni_abi_stub_slot_28(JNIEnv *env, jclass clazz, jmethodID methodID, ...)
{
    (void)clazz;
    (void)methodID;
    jni_abi_stub_fail(env, 28, "NewObject");
    return 0;
}

static jobject JNICALL jni_abi_stub_slot_29(JNIEnv *env, jclass clazz, jmethodID methodID, va_list args)
{
    (void)clazz;
    (void)methodID;
    (void)args;
    jni_abi_stub_fail(env, 29, "NewObjectV");
    return 0;
}

static jobject JNICALL jni_abi_stub_slot_30(JNIEnv *env, jclass clazz, jmethodID methodID, const jvalue *args)
{
    (void)clazz;
    (void)methodID;
    (void)args;
    jni_abi_stub_fail(env, 30, "NewObjectA");
    return 0;
}

static jclass JNICALL jni_abi_stub_slot_31(JNIEnv *env, jobject obj)
{
    (void)obj;
    jni_abi_stub_fail(env, 31, "GetObjectClass");
    return 0;
}

static jboolean JNICALL jni_abi_stub_slot_32(JNIEnv *env, jobject obj, jclass clazz)
{
    (void)obj;
    (void)clazz;
    jni_abi_stub_fail(env, 32, "IsInstanceOf");
    return 0;
}

static jmethodID JNICALL jni_abi_stub_slot_33(JNIEnv *env, jclass clazz, const char *name, const char *sig)
{
    (void)clazz;
    (void)name;
    (void)sig;
    jni_abi_stub_fail(env, 33, "GetMethodID");
    return 0;
}

static jobject JNICALL jni_abi_stub_slot_34(JNIEnv *env, jobject obj, jmethodID methodID, ...)
{
    (void)obj;
    (void)methodID;
    jni_abi_stub_fail(env, 34, "CallObjectMethod");
    return 0;
}

static jobject JNICALL jni_abi_stub_slot_35(JNIEnv *env, jobject obj, jmethodID methodID, va_list args)
{
    (void)obj;
    (void)methodID;
    (void)args;
    jni_abi_stub_fail(env, 35, "CallObjectMethodV");
    return 0;
}

static jobject JNICALL jni_abi_stub_slot_36(JNIEnv *env, jobject obj, jmethodID methodID, const jvalue * args)
{
    (void)obj;
    (void)methodID;
    (void)args;
    jni_abi_stub_fail(env, 36, "CallObjectMethodA");
    return 0;
}

static jboolean JNICALL jni_abi_stub_slot_37(JNIEnv *env, jobject obj, jmethodID methodID, ...)
{
    (void)obj;
    (void)methodID;
    jni_abi_stub_fail(env, 37, "CallBooleanMethod");
    return 0;
}

static jboolean JNICALL jni_abi_stub_slot_38(JNIEnv *env, jobject obj, jmethodID methodID, va_list args)
{
    (void)obj;
    (void)methodID;
    (void)args;
    jni_abi_stub_fail(env, 38, "CallBooleanMethodV");
    return 0;
}

static jboolean JNICALL jni_abi_stub_slot_39(JNIEnv *env, jobject obj, jmethodID methodID, const jvalue * args)
{
    (void)obj;
    (void)methodID;
    (void)args;
    jni_abi_stub_fail(env, 39, "CallBooleanMethodA");
    return 0;
}

static jbyte JNICALL jni_abi_stub_slot_40(JNIEnv *env, jobject obj, jmethodID methodID, ...)
{
    (void)obj;
    (void)methodID;
    jni_abi_stub_fail(env, 40, "CallByteMethod");
    return 0;
}

static jbyte JNICALL jni_abi_stub_slot_41(JNIEnv *env, jobject obj, jmethodID methodID, va_list args)
{
    (void)obj;
    (void)methodID;
    (void)args;
    jni_abi_stub_fail(env, 41, "CallByteMethodV");
    return 0;
}

static jbyte JNICALL jni_abi_stub_slot_42(JNIEnv *env, jobject obj, jmethodID methodID, const jvalue *args)
{
    (void)obj;
    (void)methodID;
    (void)args;
    jni_abi_stub_fail(env, 42, "CallByteMethodA");
    return 0;
}

static jchar JNICALL jni_abi_stub_slot_43(JNIEnv *env, jobject obj, jmethodID methodID, ...)
{
    (void)obj;
    (void)methodID;
    jni_abi_stub_fail(env, 43, "CallCharMethod");
    return 0;
}

static jchar JNICALL jni_abi_stub_slot_44(JNIEnv *env, jobject obj, jmethodID methodID, va_list args)
{
    (void)obj;
    (void)methodID;
    (void)args;
    jni_abi_stub_fail(env, 44, "CallCharMethodV");
    return 0;
}

static jchar JNICALL jni_abi_stub_slot_45(JNIEnv *env, jobject obj, jmethodID methodID, const jvalue *args)
{
    (void)obj;
    (void)methodID;
    (void)args;
    jni_abi_stub_fail(env, 45, "CallCharMethodA");
    return 0;
}

static jshort JNICALL jni_abi_stub_slot_46(JNIEnv *env, jobject obj, jmethodID methodID, ...)
{
    (void)obj;
    (void)methodID;
    jni_abi_stub_fail(env, 46, "CallShortMethod");
    return 0;
}

static jshort JNICALL jni_abi_stub_slot_47(JNIEnv *env, jobject obj, jmethodID methodID, va_list args)
{
    (void)obj;
    (void)methodID;
    (void)args;
    jni_abi_stub_fail(env, 47, "CallShortMethodV");
    return 0;
}

static jshort JNICALL jni_abi_stub_slot_48(JNIEnv *env, jobject obj, jmethodID methodID, const jvalue *args)
{
    (void)obj;
    (void)methodID;
    (void)args;
    jni_abi_stub_fail(env, 48, "CallShortMethodA");
    return 0;
}

static jint JNICALL jni_abi_stub_slot_49(JNIEnv *env, jobject obj, jmethodID methodID, ...)
{
    (void)obj;
    (void)methodID;
    jni_abi_stub_fail(env, 49, "CallIntMethod");
    return 0;
}

static jint JNICALL jni_abi_stub_slot_50(JNIEnv *env, jobject obj, jmethodID methodID, va_list args)
{
    (void)obj;
    (void)methodID;
    (void)args;
    jni_abi_stub_fail(env, 50, "CallIntMethodV");
    return 0;
}

static jint JNICALL jni_abi_stub_slot_51(JNIEnv *env, jobject obj, jmethodID methodID, const jvalue *args)
{
    (void)obj;
    (void)methodID;
    (void)args;
    jni_abi_stub_fail(env, 51, "CallIntMethodA");
    return 0;
}

static jlong JNICALL jni_abi_stub_slot_52(JNIEnv *env, jobject obj, jmethodID methodID, ...)
{
    (void)obj;
    (void)methodID;
    jni_abi_stub_fail(env, 52, "CallLongMethod");
    return 0;
}

static jlong JNICALL jni_abi_stub_slot_53(JNIEnv *env, jobject obj, jmethodID methodID, va_list args)
{
    (void)obj;
    (void)methodID;
    (void)args;
    jni_abi_stub_fail(env, 53, "CallLongMethodV");
    return 0;
}

static jlong JNICALL jni_abi_stub_slot_54(JNIEnv *env, jobject obj, jmethodID methodID, const jvalue *args)
{
    (void)obj;
    (void)methodID;
    (void)args;
    jni_abi_stub_fail(env, 54, "CallLongMethodA");
    return 0;
}

static jfloat JNICALL jni_abi_stub_slot_55(JNIEnv *env, jobject obj, jmethodID methodID, ...)
{
    (void)obj;
    (void)methodID;
    jni_abi_stub_fail(env, 55, "CallFloatMethod");
    return 0;
}

static jfloat JNICALL jni_abi_stub_slot_56(JNIEnv *env, jobject obj, jmethodID methodID, va_list args)
{
    (void)obj;
    (void)methodID;
    (void)args;
    jni_abi_stub_fail(env, 56, "CallFloatMethodV");
    return 0;
}

static jfloat JNICALL jni_abi_stub_slot_57(JNIEnv *env, jobject obj, jmethodID methodID, const jvalue *args)
{
    (void)obj;
    (void)methodID;
    (void)args;
    jni_abi_stub_fail(env, 57, "CallFloatMethodA");
    return 0;
}

static jdouble JNICALL jni_abi_stub_slot_58(JNIEnv *env, jobject obj, jmethodID methodID, ...)
{
    (void)obj;
    (void)methodID;
    jni_abi_stub_fail(env, 58, "CallDoubleMethod");
    return 0;
}

static jdouble JNICALL jni_abi_stub_slot_59(JNIEnv *env, jobject obj, jmethodID methodID, va_list args)
{
    (void)obj;
    (void)methodID;
    (void)args;
    jni_abi_stub_fail(env, 59, "CallDoubleMethodV");
    return 0;
}

static jdouble JNICALL jni_abi_stub_slot_60(JNIEnv *env, jobject obj, jmethodID methodID, const jvalue *args)
{
    (void)obj;
    (void)methodID;
    (void)args;
    jni_abi_stub_fail(env, 60, "CallDoubleMethodA");
    return 0;
}

static void JNICALL jni_abi_stub_slot_61(JNIEnv *env, jobject obj, jmethodID methodID, ...)
{
    (void)obj;
    (void)methodID;
    jni_abi_stub_fail(env, 61, "CallVoidMethod");
}

static void JNICALL jni_abi_stub_slot_62(JNIEnv *env, jobject obj, jmethodID methodID, va_list args)
{
    (void)obj;
    (void)methodID;
    (void)args;
    jni_abi_stub_fail(env, 62, "CallVoidMethodV");
}

static void JNICALL jni_abi_stub_slot_63(JNIEnv *env, jobject obj, jmethodID methodID, const jvalue * args)
{
    (void)obj;
    (void)methodID;
    (void)args;
    jni_abi_stub_fail(env, 63, "CallVoidMethodA");
}

static jobject JNICALL jni_abi_stub_slot_64(JNIEnv *env, jobject obj, jclass clazz, jmethodID methodID, ...)
{
    (void)obj;
    (void)clazz;
    (void)methodID;
    jni_abi_stub_fail(env, 64, "CallNonvirtualObjectMethod");
    return 0;
}

static jobject JNICALL jni_abi_stub_slot_65(JNIEnv *env, jobject obj, jclass clazz, jmethodID methodID, va_list args)
{
    (void)obj;
    (void)clazz;
    (void)methodID;
    (void)args;
    jni_abi_stub_fail(env, 65, "CallNonvirtualObjectMethodV");
    return 0;
}

static jobject JNICALL jni_abi_stub_slot_66(JNIEnv *env, jobject obj, jclass clazz, jmethodID methodID, const jvalue * args)
{
    (void)obj;
    (void)clazz;
    (void)methodID;
    (void)args;
    jni_abi_stub_fail(env, 66, "CallNonvirtualObjectMethodA");
    return 0;
}

static jboolean JNICALL jni_abi_stub_slot_67(JNIEnv *env, jobject obj, jclass clazz, jmethodID methodID, ...)
{
    (void)obj;
    (void)clazz;
    (void)methodID;
    jni_abi_stub_fail(env, 67, "CallNonvirtualBooleanMethod");
    return 0;
}

static jboolean JNICALL jni_abi_stub_slot_68(JNIEnv *env, jobject obj, jclass clazz, jmethodID methodID, va_list args)
{
    (void)obj;
    (void)clazz;
    (void)methodID;
    (void)args;
    jni_abi_stub_fail(env, 68, "CallNonvirtualBooleanMethodV");
    return 0;
}

static jboolean JNICALL jni_abi_stub_slot_69(JNIEnv *env, jobject obj, jclass clazz, jmethodID methodID, const jvalue * args)
{
    (void)obj;
    (void)clazz;
    (void)methodID;
    (void)args;
    jni_abi_stub_fail(env, 69, "CallNonvirtualBooleanMethodA");
    return 0;
}

static jbyte JNICALL jni_abi_stub_slot_70(JNIEnv *env, jobject obj, jclass clazz, jmethodID methodID, ...)
{
    (void)obj;
    (void)clazz;
    (void)methodID;
    jni_abi_stub_fail(env, 70, "CallNonvirtualByteMethod");
    return 0;
}

static jbyte JNICALL jni_abi_stub_slot_71(JNIEnv *env, jobject obj, jclass clazz, jmethodID methodID, va_list args)
{
    (void)obj;
    (void)clazz;
    (void)methodID;
    (void)args;
    jni_abi_stub_fail(env, 71, "CallNonvirtualByteMethodV");
    return 0;
}

static jbyte JNICALL jni_abi_stub_slot_72(JNIEnv *env, jobject obj, jclass clazz, jmethodID methodID, const jvalue *args)
{
    (void)obj;
    (void)clazz;
    (void)methodID;
    (void)args;
    jni_abi_stub_fail(env, 72, "CallNonvirtualByteMethodA");
    return 0;
}

static jchar JNICALL jni_abi_stub_slot_73(JNIEnv *env, jobject obj, jclass clazz, jmethodID methodID, ...)
{
    (void)obj;
    (void)clazz;
    (void)methodID;
    jni_abi_stub_fail(env, 73, "CallNonvirtualCharMethod");
    return 0;
}

static jchar JNICALL jni_abi_stub_slot_74(JNIEnv *env, jobject obj, jclass clazz, jmethodID methodID, va_list args)
{
    (void)obj;
    (void)clazz;
    (void)methodID;
    (void)args;
    jni_abi_stub_fail(env, 74, "CallNonvirtualCharMethodV");
    return 0;
}

static jchar JNICALL jni_abi_stub_slot_75(JNIEnv *env, jobject obj, jclass clazz, jmethodID methodID, const jvalue *args)
{
    (void)obj;
    (void)clazz;
    (void)methodID;
    (void)args;
    jni_abi_stub_fail(env, 75, "CallNonvirtualCharMethodA");
    return 0;
}

static jshort JNICALL jni_abi_stub_slot_76(JNIEnv *env, jobject obj, jclass clazz, jmethodID methodID, ...)
{
    (void)obj;
    (void)clazz;
    (void)methodID;
    jni_abi_stub_fail(env, 76, "CallNonvirtualShortMethod");
    return 0;
}

static jshort JNICALL jni_abi_stub_slot_77(JNIEnv *env, jobject obj, jclass clazz, jmethodID methodID, va_list args)
{
    (void)obj;
    (void)clazz;
    (void)methodID;
    (void)args;
    jni_abi_stub_fail(env, 77, "CallNonvirtualShortMethodV");
    return 0;
}

static jshort JNICALL jni_abi_stub_slot_78(JNIEnv *env, jobject obj, jclass clazz, jmethodID methodID, const jvalue *args)
{
    (void)obj;
    (void)clazz;
    (void)methodID;
    (void)args;
    jni_abi_stub_fail(env, 78, "CallNonvirtualShortMethodA");
    return 0;
}

static jint JNICALL jni_abi_stub_slot_79(JNIEnv *env, jobject obj, jclass clazz, jmethodID methodID, ...)
{
    (void)obj;
    (void)clazz;
    (void)methodID;
    jni_abi_stub_fail(env, 79, "CallNonvirtualIntMethod");
    return 0;
}

static jint JNICALL jni_abi_stub_slot_80(JNIEnv *env, jobject obj, jclass clazz, jmethodID methodID, va_list args)
{
    (void)obj;
    (void)clazz;
    (void)methodID;
    (void)args;
    jni_abi_stub_fail(env, 80, "CallNonvirtualIntMethodV");
    return 0;
}

static jint JNICALL jni_abi_stub_slot_81(JNIEnv *env, jobject obj, jclass clazz, jmethodID methodID, const jvalue *args)
{
    (void)obj;
    (void)clazz;
    (void)methodID;
    (void)args;
    jni_abi_stub_fail(env, 81, "CallNonvirtualIntMethodA");
    return 0;
}

static jlong JNICALL jni_abi_stub_slot_82(JNIEnv *env, jobject obj, jclass clazz, jmethodID methodID, ...)
{
    (void)obj;
    (void)clazz;
    (void)methodID;
    jni_abi_stub_fail(env, 82, "CallNonvirtualLongMethod");
    return 0;
}

static jlong JNICALL jni_abi_stub_slot_83(JNIEnv *env, jobject obj, jclass clazz, jmethodID methodID, va_list args)
{
    (void)obj;
    (void)clazz;
    (void)methodID;
    (void)args;
    jni_abi_stub_fail(env, 83, "CallNonvirtualLongMethodV");
    return 0;
}

static jlong JNICALL jni_abi_stub_slot_84(JNIEnv *env, jobject obj, jclass clazz, jmethodID methodID, const jvalue *args)
{
    (void)obj;
    (void)clazz;
    (void)methodID;
    (void)args;
    jni_abi_stub_fail(env, 84, "CallNonvirtualLongMethodA");
    return 0;
}

static jfloat JNICALL jni_abi_stub_slot_85(JNIEnv *env, jobject obj, jclass clazz, jmethodID methodID, ...)
{
    (void)obj;
    (void)clazz;
    (void)methodID;
    jni_abi_stub_fail(env, 85, "CallNonvirtualFloatMethod");
    return 0;
}

static jfloat JNICALL jni_abi_stub_slot_86(JNIEnv *env, jobject obj, jclass clazz, jmethodID methodID, va_list args)
{
    (void)obj;
    (void)clazz;
    (void)methodID;
    (void)args;
    jni_abi_stub_fail(env, 86, "CallNonvirtualFloatMethodV");
    return 0;
}

static jfloat JNICALL jni_abi_stub_slot_87(JNIEnv *env, jobject obj, jclass clazz, jmethodID methodID, const jvalue *args)
{
    (void)obj;
    (void)clazz;
    (void)methodID;
    (void)args;
    jni_abi_stub_fail(env, 87, "CallNonvirtualFloatMethodA");
    return 0;
}

static jdouble JNICALL jni_abi_stub_slot_88(JNIEnv *env, jobject obj, jclass clazz, jmethodID methodID, ...)
{
    (void)obj;
    (void)clazz;
    (void)methodID;
    jni_abi_stub_fail(env, 88, "CallNonvirtualDoubleMethod");
    return 0;
}

static jdouble JNICALL jni_abi_stub_slot_89(JNIEnv *env, jobject obj, jclass clazz, jmethodID methodID, va_list args)
{
    (void)obj;
    (void)clazz;
    (void)methodID;
    (void)args;
    jni_abi_stub_fail(env, 89, "CallNonvirtualDoubleMethodV");
    return 0;
}

static jdouble JNICALL jni_abi_stub_slot_90(JNIEnv *env, jobject obj, jclass clazz, jmethodID methodID, const jvalue *args)
{
    (void)obj;
    (void)clazz;
    (void)methodID;
    (void)args;
    jni_abi_stub_fail(env, 90, "CallNonvirtualDoubleMethodA");
    return 0;
}

static void JNICALL jni_abi_stub_slot_91(JNIEnv *env, jobject obj, jclass clazz, jmethodID methodID, ...)
{
    (void)obj;
    (void)clazz;
    (void)methodID;
    jni_abi_stub_fail(env, 91, "CallNonvirtualVoidMethod");
}

static void JNICALL jni_abi_stub_slot_92(JNIEnv *env, jobject obj, jclass clazz, jmethodID methodID, va_list args)
{
    (void)obj;
    (void)clazz;
    (void)methodID;
    (void)args;
    jni_abi_stub_fail(env, 92, "CallNonvirtualVoidMethodV");
}

static void JNICALL jni_abi_stub_slot_93(JNIEnv *env, jobject obj, jclass clazz, jmethodID methodID, const jvalue * args)
{
    (void)obj;
    (void)clazz;
    (void)methodID;
    (void)args;
    jni_abi_stub_fail(env, 93, "CallNonvirtualVoidMethodA");
}

static jfieldID JNICALL jni_abi_stub_slot_94(JNIEnv *env, jclass clazz, const char *name, const char *sig)
{
    (void)clazz;
    (void)name;
    (void)sig;
    jni_abi_stub_fail(env, 94, "GetFieldID");
    return 0;
}

static jobject JNICALL jni_abi_stub_slot_95(JNIEnv *env, jobject obj, jfieldID fieldID)
{
    (void)obj;
    (void)fieldID;
    jni_abi_stub_fail(env, 95, "GetObjectField");
    return 0;
}

static jboolean JNICALL jni_abi_stub_slot_96(JNIEnv *env, jobject obj, jfieldID fieldID)
{
    (void)obj;
    (void)fieldID;
    jni_abi_stub_fail(env, 96, "GetBooleanField");
    return 0;
}

static jbyte JNICALL jni_abi_stub_slot_97(JNIEnv *env, jobject obj, jfieldID fieldID)
{
    (void)obj;
    (void)fieldID;
    jni_abi_stub_fail(env, 97, "GetByteField");
    return 0;
}

static jchar JNICALL jni_abi_stub_slot_98(JNIEnv *env, jobject obj, jfieldID fieldID)
{
    (void)obj;
    (void)fieldID;
    jni_abi_stub_fail(env, 98, "GetCharField");
    return 0;
}

static jshort JNICALL jni_abi_stub_slot_99(JNIEnv *env, jobject obj, jfieldID fieldID)
{
    (void)obj;
    (void)fieldID;
    jni_abi_stub_fail(env, 99, "GetShortField");
    return 0;
}

static jint JNICALL jni_abi_stub_slot_100(JNIEnv *env, jobject obj, jfieldID fieldID)
{
    (void)obj;
    (void)fieldID;
    jni_abi_stub_fail(env, 100, "GetIntField");
    return 0;
}

static jlong JNICALL jni_abi_stub_slot_101(JNIEnv *env, jobject obj, jfieldID fieldID)
{
    (void)obj;
    (void)fieldID;
    jni_abi_stub_fail(env, 101, "GetLongField");
    return 0;
}

static jfloat JNICALL jni_abi_stub_slot_102(JNIEnv *env, jobject obj, jfieldID fieldID)
{
    (void)obj;
    (void)fieldID;
    jni_abi_stub_fail(env, 102, "GetFloatField");
    return 0;
}

static jdouble JNICALL jni_abi_stub_slot_103(JNIEnv *env, jobject obj, jfieldID fieldID)
{
    (void)obj;
    (void)fieldID;
    jni_abi_stub_fail(env, 103, "GetDoubleField");
    return 0;
}

static void JNICALL jni_abi_stub_slot_104(JNIEnv *env, jobject obj, jfieldID fieldID, jobject val)
{
    (void)obj;
    (void)fieldID;
    (void)val;
    jni_abi_stub_fail(env, 104, "SetObjectField");
}

static void JNICALL jni_abi_stub_slot_105(JNIEnv *env, jobject obj, jfieldID fieldID, jboolean val)
{
    (void)obj;
    (void)fieldID;
    (void)val;
    jni_abi_stub_fail(env, 105, "SetBooleanField");
}

static void JNICALL jni_abi_stub_slot_106(JNIEnv *env, jobject obj, jfieldID fieldID, jbyte val)
{
    (void)obj;
    (void)fieldID;
    (void)val;
    jni_abi_stub_fail(env, 106, "SetByteField");
}

static void JNICALL jni_abi_stub_slot_107(JNIEnv *env, jobject obj, jfieldID fieldID, jchar val)
{
    (void)obj;
    (void)fieldID;
    (void)val;
    jni_abi_stub_fail(env, 107, "SetCharField");
}

static void JNICALL jni_abi_stub_slot_108(JNIEnv *env, jobject obj, jfieldID fieldID, jshort val)
{
    (void)obj;
    (void)fieldID;
    (void)val;
    jni_abi_stub_fail(env, 108, "SetShortField");
}

static void JNICALL jni_abi_stub_slot_109(JNIEnv *env, jobject obj, jfieldID fieldID, jint val)
{
    (void)obj;
    (void)fieldID;
    (void)val;
    jni_abi_stub_fail(env, 109, "SetIntField");
}

static void JNICALL jni_abi_stub_slot_110(JNIEnv *env, jobject obj, jfieldID fieldID, jlong val)
{
    (void)obj;
    (void)fieldID;
    (void)val;
    jni_abi_stub_fail(env, 110, "SetLongField");
}

static void JNICALL jni_abi_stub_slot_111(JNIEnv *env, jobject obj, jfieldID fieldID, jfloat val)
{
    (void)obj;
    (void)fieldID;
    (void)val;
    jni_abi_stub_fail(env, 111, "SetFloatField");
}

static void JNICALL jni_abi_stub_slot_112(JNIEnv *env, jobject obj, jfieldID fieldID, jdouble val)
{
    (void)obj;
    (void)fieldID;
    (void)val;
    jni_abi_stub_fail(env, 112, "SetDoubleField");
}

static jmethodID JNICALL jni_abi_stub_slot_113(JNIEnv *env, jclass clazz, const char *name, const char *sig)
{
    (void)clazz;
    (void)name;
    (void)sig;
    jni_abi_stub_fail(env, 113, "GetStaticMethodID");
    return 0;
}

static jobject JNICALL jni_abi_stub_slot_114(JNIEnv *env, jclass clazz, jmethodID methodID, ...)
{
    (void)clazz;
    (void)methodID;
    jni_abi_stub_fail(env, 114, "CallStaticObjectMethod");
    return 0;
}

static jobject JNICALL jni_abi_stub_slot_115(JNIEnv *env, jclass clazz, jmethodID methodID, va_list args)
{
    (void)clazz;
    (void)methodID;
    (void)args;
    jni_abi_stub_fail(env, 115, "CallStaticObjectMethodV");
    return 0;
}

static jobject JNICALL jni_abi_stub_slot_116(JNIEnv *env, jclass clazz, jmethodID methodID, const jvalue *args)
{
    (void)clazz;
    (void)methodID;
    (void)args;
    jni_abi_stub_fail(env, 116, "CallStaticObjectMethodA");
    return 0;
}

static jboolean JNICALL jni_abi_stub_slot_117(JNIEnv *env, jclass clazz, jmethodID methodID, ...)
{
    (void)clazz;
    (void)methodID;
    jni_abi_stub_fail(env, 117, "CallStaticBooleanMethod");
    return 0;
}

static jboolean JNICALL jni_abi_stub_slot_118(JNIEnv *env, jclass clazz, jmethodID methodID, va_list args)
{
    (void)clazz;
    (void)methodID;
    (void)args;
    jni_abi_stub_fail(env, 118, "CallStaticBooleanMethodV");
    return 0;
}

static jboolean JNICALL jni_abi_stub_slot_119(JNIEnv *env, jclass clazz, jmethodID methodID, const jvalue *args)
{
    (void)clazz;
    (void)methodID;
    (void)args;
    jni_abi_stub_fail(env, 119, "CallStaticBooleanMethodA");
    return 0;
}

static jbyte JNICALL jni_abi_stub_slot_120(JNIEnv *env, jclass clazz, jmethodID methodID, ...)
{
    (void)clazz;
    (void)methodID;
    jni_abi_stub_fail(env, 120, "CallStaticByteMethod");
    return 0;
}

static jbyte JNICALL jni_abi_stub_slot_121(JNIEnv *env, jclass clazz, jmethodID methodID, va_list args)
{
    (void)clazz;
    (void)methodID;
    (void)args;
    jni_abi_stub_fail(env, 121, "CallStaticByteMethodV");
    return 0;
}

static jbyte JNICALL jni_abi_stub_slot_122(JNIEnv *env, jclass clazz, jmethodID methodID, const jvalue *args)
{
    (void)clazz;
    (void)methodID;
    (void)args;
    jni_abi_stub_fail(env, 122, "CallStaticByteMethodA");
    return 0;
}

static jchar JNICALL jni_abi_stub_slot_123(JNIEnv *env, jclass clazz, jmethodID methodID, ...)
{
    (void)clazz;
    (void)methodID;
    jni_abi_stub_fail(env, 123, "CallStaticCharMethod");
    return 0;
}

static jchar JNICALL jni_abi_stub_slot_124(JNIEnv *env, jclass clazz, jmethodID methodID, va_list args)
{
    (void)clazz;
    (void)methodID;
    (void)args;
    jni_abi_stub_fail(env, 124, "CallStaticCharMethodV");
    return 0;
}

static jchar JNICALL jni_abi_stub_slot_125(JNIEnv *env, jclass clazz, jmethodID methodID, const jvalue *args)
{
    (void)clazz;
    (void)methodID;
    (void)args;
    jni_abi_stub_fail(env, 125, "CallStaticCharMethodA");
    return 0;
}

static jshort JNICALL jni_abi_stub_slot_126(JNIEnv *env, jclass clazz, jmethodID methodID, ...)
{
    (void)clazz;
    (void)methodID;
    jni_abi_stub_fail(env, 126, "CallStaticShortMethod");
    return 0;
}

static jshort JNICALL jni_abi_stub_slot_127(JNIEnv *env, jclass clazz, jmethodID methodID, va_list args)
{
    (void)clazz;
    (void)methodID;
    (void)args;
    jni_abi_stub_fail(env, 127, "CallStaticShortMethodV");
    return 0;
}

static jshort JNICALL jni_abi_stub_slot_128(JNIEnv *env, jclass clazz, jmethodID methodID, const jvalue *args)
{
    (void)clazz;
    (void)methodID;
    (void)args;
    jni_abi_stub_fail(env, 128, "CallStaticShortMethodA");
    return 0;
}

static jint JNICALL jni_abi_stub_slot_129(JNIEnv *env, jclass clazz, jmethodID methodID, ...)
{
    (void)clazz;
    (void)methodID;
    jni_abi_stub_fail(env, 129, "CallStaticIntMethod");
    return 0;
}

static jint JNICALL jni_abi_stub_slot_130(JNIEnv *env, jclass clazz, jmethodID methodID, va_list args)
{
    (void)clazz;
    (void)methodID;
    (void)args;
    jni_abi_stub_fail(env, 130, "CallStaticIntMethodV");
    return 0;
}

static jint JNICALL jni_abi_stub_slot_131(JNIEnv *env, jclass clazz, jmethodID methodID, const jvalue *args)
{
    (void)clazz;
    (void)methodID;
    (void)args;
    jni_abi_stub_fail(env, 131, "CallStaticIntMethodA");
    return 0;
}

static jlong JNICALL jni_abi_stub_slot_132(JNIEnv *env, jclass clazz, jmethodID methodID, ...)
{
    (void)clazz;
    (void)methodID;
    jni_abi_stub_fail(env, 132, "CallStaticLongMethod");
    return 0;
}

static jlong JNICALL jni_abi_stub_slot_133(JNIEnv *env, jclass clazz, jmethodID methodID, va_list args)
{
    (void)clazz;
    (void)methodID;
    (void)args;
    jni_abi_stub_fail(env, 133, "CallStaticLongMethodV");
    return 0;
}

static jlong JNICALL jni_abi_stub_slot_134(JNIEnv *env, jclass clazz, jmethodID methodID, const jvalue *args)
{
    (void)clazz;
    (void)methodID;
    (void)args;
    jni_abi_stub_fail(env, 134, "CallStaticLongMethodA");
    return 0;
}

static jfloat JNICALL jni_abi_stub_slot_135(JNIEnv *env, jclass clazz, jmethodID methodID, ...)
{
    (void)clazz;
    (void)methodID;
    jni_abi_stub_fail(env, 135, "CallStaticFloatMethod");
    return 0;
}

static jfloat JNICALL jni_abi_stub_slot_136(JNIEnv *env, jclass clazz, jmethodID methodID, va_list args)
{
    (void)clazz;
    (void)methodID;
    (void)args;
    jni_abi_stub_fail(env, 136, "CallStaticFloatMethodV");
    return 0;
}

static jfloat JNICALL jni_abi_stub_slot_137(JNIEnv *env, jclass clazz, jmethodID methodID, const jvalue *args)
{
    (void)clazz;
    (void)methodID;
    (void)args;
    jni_abi_stub_fail(env, 137, "CallStaticFloatMethodA");
    return 0;
}

static jdouble JNICALL jni_abi_stub_slot_138(JNIEnv *env, jclass clazz, jmethodID methodID, ...)
{
    (void)clazz;
    (void)methodID;
    jni_abi_stub_fail(env, 138, "CallStaticDoubleMethod");
    return 0;
}

static jdouble JNICALL jni_abi_stub_slot_139(JNIEnv *env, jclass clazz, jmethodID methodID, va_list args)
{
    (void)clazz;
    (void)methodID;
    (void)args;
    jni_abi_stub_fail(env, 139, "CallStaticDoubleMethodV");
    return 0;
}

static jdouble JNICALL jni_abi_stub_slot_140(JNIEnv *env, jclass clazz, jmethodID methodID, const jvalue *args)
{
    (void)clazz;
    (void)methodID;
    (void)args;
    jni_abi_stub_fail(env, 140, "CallStaticDoubleMethodA");
    return 0;
}

static void JNICALL jni_abi_stub_slot_141(JNIEnv *env, jclass cls, jmethodID methodID, ...)
{
    (void)cls;
    (void)methodID;
    jni_abi_stub_fail(env, 141, "CallStaticVoidMethod");
}

static void JNICALL jni_abi_stub_slot_142(JNIEnv *env, jclass cls, jmethodID methodID, va_list args)
{
    (void)cls;
    (void)methodID;
    (void)args;
    jni_abi_stub_fail(env, 142, "CallStaticVoidMethodV");
}

static void JNICALL jni_abi_stub_slot_143(JNIEnv *env, jclass cls, jmethodID methodID, const jvalue * args)
{
    (void)cls;
    (void)methodID;
    (void)args;
    jni_abi_stub_fail(env, 143, "CallStaticVoidMethodA");
}

static jfieldID JNICALL jni_abi_stub_slot_144(JNIEnv *env, jclass clazz, const char *name, const char *sig)
{
    (void)clazz;
    (void)name;
    (void)sig;
    jni_abi_stub_fail(env, 144, "GetStaticFieldID");
    return 0;
}

static jobject JNICALL jni_abi_stub_slot_145(JNIEnv *env, jclass clazz, jfieldID fieldID)
{
    (void)clazz;
    (void)fieldID;
    jni_abi_stub_fail(env, 145, "GetStaticObjectField");
    return 0;
}

static jboolean JNICALL jni_abi_stub_slot_146(JNIEnv *env, jclass clazz, jfieldID fieldID)
{
    (void)clazz;
    (void)fieldID;
    jni_abi_stub_fail(env, 146, "GetStaticBooleanField");
    return 0;
}

static jbyte JNICALL jni_abi_stub_slot_147(JNIEnv *env, jclass clazz, jfieldID fieldID)
{
    (void)clazz;
    (void)fieldID;
    jni_abi_stub_fail(env, 147, "GetStaticByteField");
    return 0;
}

static jchar JNICALL jni_abi_stub_slot_148(JNIEnv *env, jclass clazz, jfieldID fieldID)
{
    (void)clazz;
    (void)fieldID;
    jni_abi_stub_fail(env, 148, "GetStaticCharField");
    return 0;
}

static jshort JNICALL jni_abi_stub_slot_149(JNIEnv *env, jclass clazz, jfieldID fieldID)
{
    (void)clazz;
    (void)fieldID;
    jni_abi_stub_fail(env, 149, "GetStaticShortField");
    return 0;
}

static jint JNICALL jni_abi_stub_slot_150(JNIEnv *env, jclass clazz, jfieldID fieldID)
{
    (void)clazz;
    (void)fieldID;
    jni_abi_stub_fail(env, 150, "GetStaticIntField");
    return 0;
}

static jlong JNICALL jni_abi_stub_slot_151(JNIEnv *env, jclass clazz, jfieldID fieldID)
{
    (void)clazz;
    (void)fieldID;
    jni_abi_stub_fail(env, 151, "GetStaticLongField");
    return 0;
}

static jfloat JNICALL jni_abi_stub_slot_152(JNIEnv *env, jclass clazz, jfieldID fieldID)
{
    (void)clazz;
    (void)fieldID;
    jni_abi_stub_fail(env, 152, "GetStaticFloatField");
    return 0;
}

static jdouble JNICALL jni_abi_stub_slot_153(JNIEnv *env, jclass clazz, jfieldID fieldID)
{
    (void)clazz;
    (void)fieldID;
    jni_abi_stub_fail(env, 153, "GetStaticDoubleField");
    return 0;
}

static void JNICALL jni_abi_stub_slot_154(JNIEnv *env, jclass clazz, jfieldID fieldID, jobject value)
{
    (void)clazz;
    (void)fieldID;
    (void)value;
    jni_abi_stub_fail(env, 154, "SetStaticObjectField");
}

static void JNICALL jni_abi_stub_slot_155(JNIEnv *env, jclass clazz, jfieldID fieldID, jboolean value)
{
    (void)clazz;
    (void)fieldID;
    (void)value;
    jni_abi_stub_fail(env, 155, "SetStaticBooleanField");
}

static void JNICALL jni_abi_stub_slot_156(JNIEnv *env, jclass clazz, jfieldID fieldID, jbyte value)
{
    (void)clazz;
    (void)fieldID;
    (void)value;
    jni_abi_stub_fail(env, 156, "SetStaticByteField");
}

static void JNICALL jni_abi_stub_slot_157(JNIEnv *env, jclass clazz, jfieldID fieldID, jchar value)
{
    (void)clazz;
    (void)fieldID;
    (void)value;
    jni_abi_stub_fail(env, 157, "SetStaticCharField");
}

static void JNICALL jni_abi_stub_slot_158(JNIEnv *env, jclass clazz, jfieldID fieldID, jshort value)
{
    (void)clazz;
    (void)fieldID;
    (void)value;
    jni_abi_stub_fail(env, 158, "SetStaticShortField");
}

static void JNICALL jni_abi_stub_slot_159(JNIEnv *env, jclass clazz, jfieldID fieldID, jint value)
{
    (void)clazz;
    (void)fieldID;
    (void)value;
    jni_abi_stub_fail(env, 159, "SetStaticIntField");
}

static void JNICALL jni_abi_stub_slot_160(JNIEnv *env, jclass clazz, jfieldID fieldID, jlong value)
{
    (void)clazz;
    (void)fieldID;
    (void)value;
    jni_abi_stub_fail(env, 160, "SetStaticLongField");
}

static void JNICALL jni_abi_stub_slot_161(JNIEnv *env, jclass clazz, jfieldID fieldID, jfloat value)
{
    (void)clazz;
    (void)fieldID;
    (void)value;
    jni_abi_stub_fail(env, 161, "SetStaticFloatField");
}

static void JNICALL jni_abi_stub_slot_162(JNIEnv *env, jclass clazz, jfieldID fieldID, jdouble value)
{
    (void)clazz;
    (void)fieldID;
    (void)value;
    jni_abi_stub_fail(env, 162, "SetStaticDoubleField");
}

static jstring JNICALL jni_abi_stub_slot_163(JNIEnv *env, const jchar *unicode, jsize len)
{
    (void)unicode;
    (void)len;
    jni_abi_stub_fail(env, 163, "NewString");
    return 0;
}

static jsize JNICALL jni_abi_stub_slot_164(JNIEnv *env, jstring str)
{
    (void)str;
    jni_abi_stub_fail(env, 164, "GetStringLength");
    return 0;
}

static const jchar * JNICALL jni_abi_stub_slot_165(JNIEnv *env, jstring str, jboolean *isCopy)
{
    (void)str;
    (void)isCopy;
    jni_abi_stub_fail(env, 165, "GetStringChars");
    return 0;
}

static void JNICALL jni_abi_stub_slot_166(JNIEnv *env, jstring str, const jchar *chars)
{
    (void)str;
    (void)chars;
    jni_abi_stub_fail(env, 166, "ReleaseStringChars");
}

static jstring JNICALL jni_abi_stub_slot_167(JNIEnv *env, const char *utf)
{
    (void)utf;
    jni_abi_stub_fail(env, 167, "NewStringUTF");
    return 0;
}

static jsize JNICALL jni_abi_stub_slot_168(JNIEnv *env, jstring str)
{
    (void)str;
    jni_abi_stub_fail(env, 168, "GetStringUTFLength");
    return 0;
}

static const char* JNICALL jni_abi_stub_slot_169(JNIEnv *env, jstring str, jboolean *isCopy)
{
    (void)str;
    (void)isCopy;
    jni_abi_stub_fail(env, 169, "GetStringUTFChars");
    return 0;
}

static void JNICALL jni_abi_stub_slot_170(JNIEnv *env, jstring str, const char* chars)
{
    (void)str;
    (void)chars;
    jni_abi_stub_fail(env, 170, "ReleaseStringUTFChars");
}

static jsize JNICALL jni_abi_stub_slot_171(JNIEnv *env, jarray array)
{
    (void)array;
    jni_abi_stub_fail(env, 171, "GetArrayLength");
    return 0;
}

static jobjectArray JNICALL jni_abi_stub_slot_172(JNIEnv *env, jsize len, jclass clazz, jobject init)
{
    (void)len;
    (void)clazz;
    (void)init;
    jni_abi_stub_fail(env, 172, "NewObjectArray");
    return 0;
}

static jobject JNICALL jni_abi_stub_slot_173(JNIEnv *env, jobjectArray array, jsize index)
{
    (void)array;
    (void)index;
    jni_abi_stub_fail(env, 173, "GetObjectArrayElement");
    return 0;
}

static void JNICALL jni_abi_stub_slot_174(JNIEnv *env, jobjectArray array, jsize index, jobject val)
{
    (void)array;
    (void)index;
    (void)val;
    jni_abi_stub_fail(env, 174, "SetObjectArrayElement");
}

static jbooleanArray JNICALL jni_abi_stub_slot_175(JNIEnv *env, jsize len)
{
    (void)len;
    jni_abi_stub_fail(env, 175, "NewBooleanArray");
    return 0;
}

static jbyteArray JNICALL jni_abi_stub_slot_176(JNIEnv *env, jsize len)
{
    (void)len;
    jni_abi_stub_fail(env, 176, "NewByteArray");
    return 0;
}

static jcharArray JNICALL jni_abi_stub_slot_177(JNIEnv *env, jsize len)
{
    (void)len;
    jni_abi_stub_fail(env, 177, "NewCharArray");
    return 0;
}

static jshortArray JNICALL jni_abi_stub_slot_178(JNIEnv *env, jsize len)
{
    (void)len;
    jni_abi_stub_fail(env, 178, "NewShortArray");
    return 0;
}

static jintArray JNICALL jni_abi_stub_slot_179(JNIEnv *env, jsize len)
{
    (void)len;
    jni_abi_stub_fail(env, 179, "NewIntArray");
    return 0;
}

static jlongArray JNICALL jni_abi_stub_slot_180(JNIEnv *env, jsize len)
{
    (void)len;
    jni_abi_stub_fail(env, 180, "NewLongArray");
    return 0;
}

static jfloatArray JNICALL jni_abi_stub_slot_181(JNIEnv *env, jsize len)
{
    (void)len;
    jni_abi_stub_fail(env, 181, "NewFloatArray");
    return 0;
}

static jdoubleArray JNICALL jni_abi_stub_slot_182(JNIEnv *env, jsize len)
{
    (void)len;
    jni_abi_stub_fail(env, 182, "NewDoubleArray");
    return 0;
}

static jboolean * JNICALL jni_abi_stub_slot_183(JNIEnv *env, jbooleanArray array, jboolean *isCopy)
{
    (void)array;
    (void)isCopy;
    jni_abi_stub_fail(env, 183, "GetBooleanArrayElements");
    return 0;
}

static jbyte * JNICALL jni_abi_stub_slot_184(JNIEnv *env, jbyteArray array, jboolean *isCopy)
{
    (void)array;
    (void)isCopy;
    jni_abi_stub_fail(env, 184, "GetByteArrayElements");
    return 0;
}

static jchar * JNICALL jni_abi_stub_slot_185(JNIEnv *env, jcharArray array, jboolean *isCopy)
{
    (void)array;
    (void)isCopy;
    jni_abi_stub_fail(env, 185, "GetCharArrayElements");
    return 0;
}

static jshort * JNICALL jni_abi_stub_slot_186(JNIEnv *env, jshortArray array, jboolean *isCopy)
{
    (void)array;
    (void)isCopy;
    jni_abi_stub_fail(env, 186, "GetShortArrayElements");
    return 0;
}

static jint * JNICALL jni_abi_stub_slot_187(JNIEnv *env, jintArray array, jboolean *isCopy)
{
    (void)array;
    (void)isCopy;
    jni_abi_stub_fail(env, 187, "GetIntArrayElements");
    return 0;
}

static jlong * JNICALL jni_abi_stub_slot_188(JNIEnv *env, jlongArray array, jboolean *isCopy)
{
    (void)array;
    (void)isCopy;
    jni_abi_stub_fail(env, 188, "GetLongArrayElements");
    return 0;
}

static jfloat * JNICALL jni_abi_stub_slot_189(JNIEnv *env, jfloatArray array, jboolean *isCopy)
{
    (void)array;
    (void)isCopy;
    jni_abi_stub_fail(env, 189, "GetFloatArrayElements");
    return 0;
}

static jdouble * JNICALL jni_abi_stub_slot_190(JNIEnv *env, jdoubleArray array, jboolean *isCopy)
{
    (void)array;
    (void)isCopy;
    jni_abi_stub_fail(env, 190, "GetDoubleArrayElements");
    return 0;
}

static void JNICALL jni_abi_stub_slot_191(JNIEnv *env, jbooleanArray array, jboolean *elems, jint mode)
{
    (void)array;
    (void)elems;
    (void)mode;
    jni_abi_stub_fail(env, 191, "ReleaseBooleanArrayElements");
}

static void JNICALL jni_abi_stub_slot_192(JNIEnv *env, jbyteArray array, jbyte *elems, jint mode)
{
    (void)array;
    (void)elems;
    (void)mode;
    jni_abi_stub_fail(env, 192, "ReleaseByteArrayElements");
}

static void JNICALL jni_abi_stub_slot_193(JNIEnv *env, jcharArray array, jchar *elems, jint mode)
{
    (void)array;
    (void)elems;
    (void)mode;
    jni_abi_stub_fail(env, 193, "ReleaseCharArrayElements");
}

static void JNICALL jni_abi_stub_slot_194(JNIEnv *env, jshortArray array, jshort *elems, jint mode)
{
    (void)array;
    (void)elems;
    (void)mode;
    jni_abi_stub_fail(env, 194, "ReleaseShortArrayElements");
}

static void JNICALL jni_abi_stub_slot_195(JNIEnv *env, jintArray array, jint *elems, jint mode)
{
    (void)array;
    (void)elems;
    (void)mode;
    jni_abi_stub_fail(env, 195, "ReleaseIntArrayElements");
}

static void JNICALL jni_abi_stub_slot_196(JNIEnv *env, jlongArray array, jlong *elems, jint mode)
{
    (void)array;
    (void)elems;
    (void)mode;
    jni_abi_stub_fail(env, 196, "ReleaseLongArrayElements");
}

static void JNICALL jni_abi_stub_slot_197(JNIEnv *env, jfloatArray array, jfloat *elems, jint mode)
{
    (void)array;
    (void)elems;
    (void)mode;
    jni_abi_stub_fail(env, 197, "ReleaseFloatArrayElements");
}

static void JNICALL jni_abi_stub_slot_198(JNIEnv *env, jdoubleArray array, jdouble *elems, jint mode)
{
    (void)array;
    (void)elems;
    (void)mode;
    jni_abi_stub_fail(env, 198, "ReleaseDoubleArrayElements");
}

static void JNICALL jni_abi_stub_slot_199(JNIEnv *env, jbooleanArray array, jsize start, jsize l, jboolean *buf)
{
    (void)array;
    (void)start;
    (void)l;
    (void)buf;
    jni_abi_stub_fail(env, 199, "GetBooleanArrayRegion");
}

static void JNICALL jni_abi_stub_slot_200(JNIEnv *env, jbyteArray array, jsize start, jsize len, jbyte *buf)
{
    (void)array;
    (void)start;
    (void)len;
    (void)buf;
    jni_abi_stub_fail(env, 200, "GetByteArrayRegion");
}

static void JNICALL jni_abi_stub_slot_201(JNIEnv *env, jcharArray array, jsize start, jsize len, jchar *buf)
{
    (void)array;
    (void)start;
    (void)len;
    (void)buf;
    jni_abi_stub_fail(env, 201, "GetCharArrayRegion");
}

static void JNICALL jni_abi_stub_slot_202(JNIEnv *env, jshortArray array, jsize start, jsize len, jshort *buf)
{
    (void)array;
    (void)start;
    (void)len;
    (void)buf;
    jni_abi_stub_fail(env, 202, "GetShortArrayRegion");
}

static void JNICALL jni_abi_stub_slot_203(JNIEnv *env, jintArray array, jsize start, jsize len, jint *buf)
{
    (void)array;
    (void)start;
    (void)len;
    (void)buf;
    jni_abi_stub_fail(env, 203, "GetIntArrayRegion");
}

static void JNICALL jni_abi_stub_slot_204(JNIEnv *env, jlongArray array, jsize start, jsize len, jlong *buf)
{
    (void)array;
    (void)start;
    (void)len;
    (void)buf;
    jni_abi_stub_fail(env, 204, "GetLongArrayRegion");
}

static void JNICALL jni_abi_stub_slot_205(JNIEnv *env, jfloatArray array, jsize start, jsize len, jfloat *buf)
{
    (void)array;
    (void)start;
    (void)len;
    (void)buf;
    jni_abi_stub_fail(env, 205, "GetFloatArrayRegion");
}

static void JNICALL jni_abi_stub_slot_206(JNIEnv *env, jdoubleArray array, jsize start, jsize len, jdouble *buf)
{
    (void)array;
    (void)start;
    (void)len;
    (void)buf;
    jni_abi_stub_fail(env, 206, "GetDoubleArrayRegion");
}

static void JNICALL jni_abi_stub_slot_207(JNIEnv *env, jbooleanArray array, jsize start, jsize l, const jboolean *buf)
{
    (void)array;
    (void)start;
    (void)l;
    (void)buf;
    jni_abi_stub_fail(env, 207, "SetBooleanArrayRegion");
}

static void JNICALL jni_abi_stub_slot_208(JNIEnv *env, jbyteArray array, jsize start, jsize len, const jbyte *buf)
{
    (void)array;
    (void)start;
    (void)len;
    (void)buf;
    jni_abi_stub_fail(env, 208, "SetByteArrayRegion");
}

static void JNICALL jni_abi_stub_slot_209(JNIEnv *env, jcharArray array, jsize start, jsize len, const jchar *buf)
{
    (void)array;
    (void)start;
    (void)len;
    (void)buf;
    jni_abi_stub_fail(env, 209, "SetCharArrayRegion");
}

static void JNICALL jni_abi_stub_slot_210(JNIEnv *env, jshortArray array, jsize start, jsize len, const jshort *buf)
{
    (void)array;
    (void)start;
    (void)len;
    (void)buf;
    jni_abi_stub_fail(env, 210, "SetShortArrayRegion");
}

static void JNICALL jni_abi_stub_slot_211(JNIEnv *env, jintArray array, jsize start, jsize len, const jint *buf)
{
    (void)array;
    (void)start;
    (void)len;
    (void)buf;
    jni_abi_stub_fail(env, 211, "SetIntArrayRegion");
}

static void JNICALL jni_abi_stub_slot_212(JNIEnv *env, jlongArray array, jsize start, jsize len, const jlong *buf)
{
    (void)array;
    (void)start;
    (void)len;
    (void)buf;
    jni_abi_stub_fail(env, 212, "SetLongArrayRegion");
}

static void JNICALL jni_abi_stub_slot_213(JNIEnv *env, jfloatArray array, jsize start, jsize len, const jfloat *buf)
{
    (void)array;
    (void)start;
    (void)len;
    (void)buf;
    jni_abi_stub_fail(env, 213, "SetFloatArrayRegion");
}

static void JNICALL jni_abi_stub_slot_214(JNIEnv *env, jdoubleArray array, jsize start, jsize len, const jdouble *buf)
{
    (void)array;
    (void)start;
    (void)len;
    (void)buf;
    jni_abi_stub_fail(env, 214, "SetDoubleArrayRegion");
}

static jint JNICALL jni_abi_stub_slot_215(JNIEnv *env, jclass clazz, const JNINativeMethod *methods, jint nMethods)
{
    (void)clazz;
    (void)methods;
    (void)nMethods;
    jni_abi_stub_fail(env, 215, "RegisterNatives");
    return 0;
}

static jint JNICALL jni_abi_stub_slot_216(JNIEnv *env, jclass clazz)
{
    (void)clazz;
    jni_abi_stub_fail(env, 216, "UnregisterNatives");
    return 0;
}

static jint JNICALL jni_abi_stub_slot_217(JNIEnv *env, jobject obj)
{
    (void)obj;
    jni_abi_stub_fail(env, 217, "MonitorEnter");
    return 0;
}

static jint JNICALL jni_abi_stub_slot_218(JNIEnv *env, jobject obj)
{
    (void)obj;
    jni_abi_stub_fail(env, 218, "MonitorExit");
    return 0;
}

static jint JNICALL jni_abi_stub_slot_219(JNIEnv *env, JavaVM **vm)
{
    (void)vm;
    jni_abi_stub_fail(env, 219, "GetJavaVM");
    return 0;
}

static void JNICALL jni_abi_stub_slot_220(JNIEnv *env, jstring str, jsize start, jsize len, jchar *buf)
{
    (void)str;
    (void)start;
    (void)len;
    (void)buf;
    jni_abi_stub_fail(env, 220, "GetStringRegion");
}

static void JNICALL jni_abi_stub_slot_221(JNIEnv *env, jstring str, jsize start, jsize len, char *buf)
{
    (void)str;
    (void)start;
    (void)len;
    (void)buf;
    jni_abi_stub_fail(env, 221, "GetStringUTFRegion");
}

static void * JNICALL jni_abi_stub_slot_222(JNIEnv *env, jarray array, jboolean *isCopy)
{
    (void)array;
    (void)isCopy;
    jni_abi_stub_fail(env, 222, "GetPrimitiveArrayCritical");
    return 0;
}

static void JNICALL jni_abi_stub_slot_223(JNIEnv *env, jarray array, void *carray, jint mode)
{
    (void)array;
    (void)carray;
    (void)mode;
    jni_abi_stub_fail(env, 223, "ReleasePrimitiveArrayCritical");
}

static const jchar * JNICALL jni_abi_stub_slot_224(JNIEnv *env, jstring string, jboolean *isCopy)
{
    (void)string;
    (void)isCopy;
    jni_abi_stub_fail(env, 224, "GetStringCritical");
    return 0;
}

static void JNICALL jni_abi_stub_slot_225(JNIEnv *env, jstring string, const jchar *cstring)
{
    (void)string;
    (void)cstring;
    jni_abi_stub_fail(env, 225, "ReleaseStringCritical");
}

static jweak JNICALL jni_abi_stub_slot_226(JNIEnv *env, jobject obj)
{
    (void)obj;
    jni_abi_stub_fail(env, 226, "NewWeakGlobalRef");
    return 0;
}

static void JNICALL jni_abi_stub_slot_227(JNIEnv *env, jweak ref)
{
    (void)ref;
    jni_abi_stub_fail(env, 227, "DeleteWeakGlobalRef");
}

static jboolean JNICALL jni_abi_stub_slot_228(JNIEnv *env)
{
    jni_abi_stub_fail(env, 228, "ExceptionCheck");
    return 0;
}

static jobject JNICALL jni_abi_stub_slot_229(JNIEnv* env, void* address, jlong capacity)
{
    (void)address;
    (void)capacity;
    jni_abi_stub_fail(env, 229, "NewDirectByteBuffer");
    return 0;
}

static void* JNICALL jni_abi_stub_slot_230(JNIEnv* env, jobject buf)
{
    (void)buf;
    jni_abi_stub_fail(env, 230, "GetDirectBufferAddress");
    return 0;
}

static jlong JNICALL jni_abi_stub_slot_231(JNIEnv* env, jobject buf)
{
    (void)buf;
    jni_abi_stub_fail(env, 231, "GetDirectBufferCapacity");
    return 0;
}

static jobjectRefType JNICALL jni_abi_stub_slot_232(JNIEnv* env, jobject obj)
{
    (void)obj;
    jni_abi_stub_fail(env, 232, "GetObjectRefType");
    return 0;
}

static void jni_abi_stubs_install(struct JNINativeInterface_ *t)
{
    /* reserved0..3 保持 NULL：标准保留槽不允许被调用。 */
    t->GetVersion = jni_abi_stub_slot_4;
    t->DefineClass = jni_abi_stub_slot_5;
    t->FindClass = jni_abi_stub_slot_6;
    t->FromReflectedMethod = jni_abi_stub_slot_7;
    t->FromReflectedField = jni_abi_stub_slot_8;
    t->ToReflectedMethod = jni_abi_stub_slot_9;
    t->GetSuperclass = jni_abi_stub_slot_10;
    t->IsAssignableFrom = jni_abi_stub_slot_11;
    t->ToReflectedField = jni_abi_stub_slot_12;
    t->Throw = jni_abi_stub_slot_13;
    t->ThrowNew = jni_abi_stub_slot_14;
    t->ExceptionOccurred = jni_abi_stub_slot_15;
    t->ExceptionDescribe = jni_abi_stub_slot_16;
    t->ExceptionClear = jni_abi_stub_slot_17;
    t->FatalError = jni_abi_stub_slot_18;
    t->PushLocalFrame = jni_abi_stub_slot_19;
    t->PopLocalFrame = jni_abi_stub_slot_20;
    t->NewGlobalRef = jni_abi_stub_slot_21;
    t->DeleteGlobalRef = jni_abi_stub_slot_22;
    t->DeleteLocalRef = jni_abi_stub_slot_23;
    t->IsSameObject = jni_abi_stub_slot_24;
    t->NewLocalRef = jni_abi_stub_slot_25;
    t->EnsureLocalCapacity = jni_abi_stub_slot_26;
    t->AllocObject = jni_abi_stub_slot_27;
    t->NewObject = jni_abi_stub_slot_28;
    t->NewObjectV = jni_abi_stub_slot_29;
    t->NewObjectA = jni_abi_stub_slot_30;
    t->GetObjectClass = jni_abi_stub_slot_31;
    t->IsInstanceOf = jni_abi_stub_slot_32;
    t->GetMethodID = jni_abi_stub_slot_33;
    t->CallObjectMethod = jni_abi_stub_slot_34;
    t->CallObjectMethodV = jni_abi_stub_slot_35;
    t->CallObjectMethodA = jni_abi_stub_slot_36;
    t->CallBooleanMethod = jni_abi_stub_slot_37;
    t->CallBooleanMethodV = jni_abi_stub_slot_38;
    t->CallBooleanMethodA = jni_abi_stub_slot_39;
    t->CallByteMethod = jni_abi_stub_slot_40;
    t->CallByteMethodV = jni_abi_stub_slot_41;
    t->CallByteMethodA = jni_abi_stub_slot_42;
    t->CallCharMethod = jni_abi_stub_slot_43;
    t->CallCharMethodV = jni_abi_stub_slot_44;
    t->CallCharMethodA = jni_abi_stub_slot_45;
    t->CallShortMethod = jni_abi_stub_slot_46;
    t->CallShortMethodV = jni_abi_stub_slot_47;
    t->CallShortMethodA = jni_abi_stub_slot_48;
    t->CallIntMethod = jni_abi_stub_slot_49;
    t->CallIntMethodV = jni_abi_stub_slot_50;
    t->CallIntMethodA = jni_abi_stub_slot_51;
    t->CallLongMethod = jni_abi_stub_slot_52;
    t->CallLongMethodV = jni_abi_stub_slot_53;
    t->CallLongMethodA = jni_abi_stub_slot_54;
    t->CallFloatMethod = jni_abi_stub_slot_55;
    t->CallFloatMethodV = jni_abi_stub_slot_56;
    t->CallFloatMethodA = jni_abi_stub_slot_57;
    t->CallDoubleMethod = jni_abi_stub_slot_58;
    t->CallDoubleMethodV = jni_abi_stub_slot_59;
    t->CallDoubleMethodA = jni_abi_stub_slot_60;
    t->CallVoidMethod = jni_abi_stub_slot_61;
    t->CallVoidMethodV = jni_abi_stub_slot_62;
    t->CallVoidMethodA = jni_abi_stub_slot_63;
    t->CallNonvirtualObjectMethod = jni_abi_stub_slot_64;
    t->CallNonvirtualObjectMethodV = jni_abi_stub_slot_65;
    t->CallNonvirtualObjectMethodA = jni_abi_stub_slot_66;
    t->CallNonvirtualBooleanMethod = jni_abi_stub_slot_67;
    t->CallNonvirtualBooleanMethodV = jni_abi_stub_slot_68;
    t->CallNonvirtualBooleanMethodA = jni_abi_stub_slot_69;
    t->CallNonvirtualByteMethod = jni_abi_stub_slot_70;
    t->CallNonvirtualByteMethodV = jni_abi_stub_slot_71;
    t->CallNonvirtualByteMethodA = jni_abi_stub_slot_72;
    t->CallNonvirtualCharMethod = jni_abi_stub_slot_73;
    t->CallNonvirtualCharMethodV = jni_abi_stub_slot_74;
    t->CallNonvirtualCharMethodA = jni_abi_stub_slot_75;
    t->CallNonvirtualShortMethod = jni_abi_stub_slot_76;
    t->CallNonvirtualShortMethodV = jni_abi_stub_slot_77;
    t->CallNonvirtualShortMethodA = jni_abi_stub_slot_78;
    t->CallNonvirtualIntMethod = jni_abi_stub_slot_79;
    t->CallNonvirtualIntMethodV = jni_abi_stub_slot_80;
    t->CallNonvirtualIntMethodA = jni_abi_stub_slot_81;
    t->CallNonvirtualLongMethod = jni_abi_stub_slot_82;
    t->CallNonvirtualLongMethodV = jni_abi_stub_slot_83;
    t->CallNonvirtualLongMethodA = jni_abi_stub_slot_84;
    t->CallNonvirtualFloatMethod = jni_abi_stub_slot_85;
    t->CallNonvirtualFloatMethodV = jni_abi_stub_slot_86;
    t->CallNonvirtualFloatMethodA = jni_abi_stub_slot_87;
    t->CallNonvirtualDoubleMethod = jni_abi_stub_slot_88;
    t->CallNonvirtualDoubleMethodV = jni_abi_stub_slot_89;
    t->CallNonvirtualDoubleMethodA = jni_abi_stub_slot_90;
    t->CallNonvirtualVoidMethod = jni_abi_stub_slot_91;
    t->CallNonvirtualVoidMethodV = jni_abi_stub_slot_92;
    t->CallNonvirtualVoidMethodA = jni_abi_stub_slot_93;
    t->GetFieldID = jni_abi_stub_slot_94;
    t->GetObjectField = jni_abi_stub_slot_95;
    t->GetBooleanField = jni_abi_stub_slot_96;
    t->GetByteField = jni_abi_stub_slot_97;
    t->GetCharField = jni_abi_stub_slot_98;
    t->GetShortField = jni_abi_stub_slot_99;
    t->GetIntField = jni_abi_stub_slot_100;
    t->GetLongField = jni_abi_stub_slot_101;
    t->GetFloatField = jni_abi_stub_slot_102;
    t->GetDoubleField = jni_abi_stub_slot_103;
    t->SetObjectField = jni_abi_stub_slot_104;
    t->SetBooleanField = jni_abi_stub_slot_105;
    t->SetByteField = jni_abi_stub_slot_106;
    t->SetCharField = jni_abi_stub_slot_107;
    t->SetShortField = jni_abi_stub_slot_108;
    t->SetIntField = jni_abi_stub_slot_109;
    t->SetLongField = jni_abi_stub_slot_110;
    t->SetFloatField = jni_abi_stub_slot_111;
    t->SetDoubleField = jni_abi_stub_slot_112;
    t->GetStaticMethodID = jni_abi_stub_slot_113;
    t->CallStaticObjectMethod = jni_abi_stub_slot_114;
    t->CallStaticObjectMethodV = jni_abi_stub_slot_115;
    t->CallStaticObjectMethodA = jni_abi_stub_slot_116;
    t->CallStaticBooleanMethod = jni_abi_stub_slot_117;
    t->CallStaticBooleanMethodV = jni_abi_stub_slot_118;
    t->CallStaticBooleanMethodA = jni_abi_stub_slot_119;
    t->CallStaticByteMethod = jni_abi_stub_slot_120;
    t->CallStaticByteMethodV = jni_abi_stub_slot_121;
    t->CallStaticByteMethodA = jni_abi_stub_slot_122;
    t->CallStaticCharMethod = jni_abi_stub_slot_123;
    t->CallStaticCharMethodV = jni_abi_stub_slot_124;
    t->CallStaticCharMethodA = jni_abi_stub_slot_125;
    t->CallStaticShortMethod = jni_abi_stub_slot_126;
    t->CallStaticShortMethodV = jni_abi_stub_slot_127;
    t->CallStaticShortMethodA = jni_abi_stub_slot_128;
    t->CallStaticIntMethod = jni_abi_stub_slot_129;
    t->CallStaticIntMethodV = jni_abi_stub_slot_130;
    t->CallStaticIntMethodA = jni_abi_stub_slot_131;
    t->CallStaticLongMethod = jni_abi_stub_slot_132;
    t->CallStaticLongMethodV = jni_abi_stub_slot_133;
    t->CallStaticLongMethodA = jni_abi_stub_slot_134;
    t->CallStaticFloatMethod = jni_abi_stub_slot_135;
    t->CallStaticFloatMethodV = jni_abi_stub_slot_136;
    t->CallStaticFloatMethodA = jni_abi_stub_slot_137;
    t->CallStaticDoubleMethod = jni_abi_stub_slot_138;
    t->CallStaticDoubleMethodV = jni_abi_stub_slot_139;
    t->CallStaticDoubleMethodA = jni_abi_stub_slot_140;
    t->CallStaticVoidMethod = jni_abi_stub_slot_141;
    t->CallStaticVoidMethodV = jni_abi_stub_slot_142;
    t->CallStaticVoidMethodA = jni_abi_stub_slot_143;
    t->GetStaticFieldID = jni_abi_stub_slot_144;
    t->GetStaticObjectField = jni_abi_stub_slot_145;
    t->GetStaticBooleanField = jni_abi_stub_slot_146;
    t->GetStaticByteField = jni_abi_stub_slot_147;
    t->GetStaticCharField = jni_abi_stub_slot_148;
    t->GetStaticShortField = jni_abi_stub_slot_149;
    t->GetStaticIntField = jni_abi_stub_slot_150;
    t->GetStaticLongField = jni_abi_stub_slot_151;
    t->GetStaticFloatField = jni_abi_stub_slot_152;
    t->GetStaticDoubleField = jni_abi_stub_slot_153;
    t->SetStaticObjectField = jni_abi_stub_slot_154;
    t->SetStaticBooleanField = jni_abi_stub_slot_155;
    t->SetStaticByteField = jni_abi_stub_slot_156;
    t->SetStaticCharField = jni_abi_stub_slot_157;
    t->SetStaticShortField = jni_abi_stub_slot_158;
    t->SetStaticIntField = jni_abi_stub_slot_159;
    t->SetStaticLongField = jni_abi_stub_slot_160;
    t->SetStaticFloatField = jni_abi_stub_slot_161;
    t->SetStaticDoubleField = jni_abi_stub_slot_162;
    t->NewString = jni_abi_stub_slot_163;
    t->GetStringLength = jni_abi_stub_slot_164;
    t->GetStringChars = jni_abi_stub_slot_165;
    t->ReleaseStringChars = jni_abi_stub_slot_166;
    t->NewStringUTF = jni_abi_stub_slot_167;
    t->GetStringUTFLength = jni_abi_stub_slot_168;
    t->GetStringUTFChars = jni_abi_stub_slot_169;
    t->ReleaseStringUTFChars = jni_abi_stub_slot_170;
    t->GetArrayLength = jni_abi_stub_slot_171;
    t->NewObjectArray = jni_abi_stub_slot_172;
    t->GetObjectArrayElement = jni_abi_stub_slot_173;
    t->SetObjectArrayElement = jni_abi_stub_slot_174;
    t->NewBooleanArray = jni_abi_stub_slot_175;
    t->NewByteArray = jni_abi_stub_slot_176;
    t->NewCharArray = jni_abi_stub_slot_177;
    t->NewShortArray = jni_abi_stub_slot_178;
    t->NewIntArray = jni_abi_stub_slot_179;
    t->NewLongArray = jni_abi_stub_slot_180;
    t->NewFloatArray = jni_abi_stub_slot_181;
    t->NewDoubleArray = jni_abi_stub_slot_182;
    t->GetBooleanArrayElements = jni_abi_stub_slot_183;
    t->GetByteArrayElements = jni_abi_stub_slot_184;
    t->GetCharArrayElements = jni_abi_stub_slot_185;
    t->GetShortArrayElements = jni_abi_stub_slot_186;
    t->GetIntArrayElements = jni_abi_stub_slot_187;
    t->GetLongArrayElements = jni_abi_stub_slot_188;
    t->GetFloatArrayElements = jni_abi_stub_slot_189;
    t->GetDoubleArrayElements = jni_abi_stub_slot_190;
    t->ReleaseBooleanArrayElements = jni_abi_stub_slot_191;
    t->ReleaseByteArrayElements = jni_abi_stub_slot_192;
    t->ReleaseCharArrayElements = jni_abi_stub_slot_193;
    t->ReleaseShortArrayElements = jni_abi_stub_slot_194;
    t->ReleaseIntArrayElements = jni_abi_stub_slot_195;
    t->ReleaseLongArrayElements = jni_abi_stub_slot_196;
    t->ReleaseFloatArrayElements = jni_abi_stub_slot_197;
    t->ReleaseDoubleArrayElements = jni_abi_stub_slot_198;
    t->GetBooleanArrayRegion = jni_abi_stub_slot_199;
    t->GetByteArrayRegion = jni_abi_stub_slot_200;
    t->GetCharArrayRegion = jni_abi_stub_slot_201;
    t->GetShortArrayRegion = jni_abi_stub_slot_202;
    t->GetIntArrayRegion = jni_abi_stub_slot_203;
    t->GetLongArrayRegion = jni_abi_stub_slot_204;
    t->GetFloatArrayRegion = jni_abi_stub_slot_205;
    t->GetDoubleArrayRegion = jni_abi_stub_slot_206;
    t->SetBooleanArrayRegion = jni_abi_stub_slot_207;
    t->SetByteArrayRegion = jni_abi_stub_slot_208;
    t->SetCharArrayRegion = jni_abi_stub_slot_209;
    t->SetShortArrayRegion = jni_abi_stub_slot_210;
    t->SetIntArrayRegion = jni_abi_stub_slot_211;
    t->SetLongArrayRegion = jni_abi_stub_slot_212;
    t->SetFloatArrayRegion = jni_abi_stub_slot_213;
    t->SetDoubleArrayRegion = jni_abi_stub_slot_214;
    t->RegisterNatives = jni_abi_stub_slot_215;
    t->UnregisterNatives = jni_abi_stub_slot_216;
    t->MonitorEnter = jni_abi_stub_slot_217;
    t->MonitorExit = jni_abi_stub_slot_218;
    t->GetJavaVM = jni_abi_stub_slot_219;
    t->GetStringRegion = jni_abi_stub_slot_220;
    t->GetStringUTFRegion = jni_abi_stub_slot_221;
    t->GetPrimitiveArrayCritical = jni_abi_stub_slot_222;
    t->ReleasePrimitiveArrayCritical = jni_abi_stub_slot_223;
    t->GetStringCritical = jni_abi_stub_slot_224;
    t->ReleaseStringCritical = jni_abi_stub_slot_225;
    t->NewWeakGlobalRef = jni_abi_stub_slot_226;
    t->DeleteWeakGlobalRef = jni_abi_stub_slot_227;
    t->ExceptionCheck = jni_abi_stub_slot_228;
    t->NewDirectByteBuffer = jni_abi_stub_slot_229;
    t->GetDirectBufferAddress = jni_abi_stub_slot_230;
    t->GetDirectBufferCapacity = jni_abi_stub_slot_231;
    t->GetObjectRefType = jni_abi_stub_slot_232;
}

#ifdef __cplusplus
}
#endif

#endif /* NATIVE_JNI_NEXT_JNI_ABI_STUBS_H */
