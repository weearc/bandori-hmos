#ifndef NATIVE_JNI_NEXT_JNI_ABI_H
#define NATIVE_JNI_NEXT_JNI_ABI_H

/*
 * 自有 JNI 1.6 ABI 固定头。
 *
 * 本文件只固定 JNI 1.6 标准的 233 槽 JNINativeInterface 和 8 槽
 * JNIInvokeInterface，不引入 Android NDK 或 JDK 的 jni.h。
 * 关键前提：
 *   - 目标为 OHOS arm64（LP64），JNICALL 为空；
 *   - C 语言下 JNIEnv 是“指向函数表的指针”，native 函数参数 JNIEnv *env
 *     指向一个首字段为函数表指针的桥线程结构；
 *   - 不包含 JDK 21 在标准 1.6 表之后追加的 GetModule、
 *     IsVirtualThread、GetStringUTFLengthAsLong。
 *
 * 槽位以真实 JNI 1.6 表为准；用户原始 prompt 中的部分编号有误，
 * 差异记录在 native-jni-next/README.md 和最终报告中。
 */

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef JNIEXPORT
#define JNIEXPORT __attribute__((visibility("default")))
#endif
#ifndef JNIIMPORT
#define JNIIMPORT
#endif
#ifndef JNICALL
#define JNICALL
#endif

typedef uint8_t  jboolean;
typedef int8_t   jbyte;
typedef uint16_t jchar;
typedef int16_t  jshort;
typedef int32_t  jint;
typedef int64_t  jlong;
typedef float    jfloat;
typedef double   jdouble;
typedef jint     jsize;
typedef intptr_t jintptr;

#define JNI_FALSE 0
#define JNI_TRUE  1

#define JNI_COMMIT 1
#define JNI_ABORT  2

#define JNI_OK          0
#define JNI_ERR        (-1)
#define JNI_EDETACHED  (-2)
#define JNI_EVERSION   (-3)
#define JNI_ENOMEM     (-4)
#define JNI_EEXIST     (-5)
#define JNI_EINVAL     (-6)

#define JNI_VERSION_1_1 0x00010001
#define JNI_VERSION_1_2 0x00010002
#define JNI_VERSION_1_4 0x00010004
#define JNI_VERSION_1_6 0x00010006

struct _jobject;
typedef struct _jobject *jobject;
typedef jobject jclass;
typedef jobject jthrowable;
typedef jobject jstring;
typedef jobject jarray;
typedef jarray jbooleanArray;
typedef jarray jbyteArray;
typedef jarray jcharArray;
typedef jarray jshortArray;
typedef jarray jintArray;
typedef jarray jlongArray;
typedef jarray jfloatArray;
typedef jarray jdoubleArray;
typedef jarray jobjectArray;
typedef jobject jweak;

typedef union jvalue {
    jboolean z;
    jbyte    b;
    jchar    c;
    jshort   s;
    jint     i;
    jlong    j;
    jfloat   f;
    jdouble  d;
    jobject  l;
} jvalue;

struct _jmethodID;
typedef struct _jmethodID *jmethodID;
struct _jfieldID;
typedef struct _jfieldID *jfieldID;

typedef enum _jobjectType {
    JNIInvalidRefType = 0,
    JNILocalRefType = 1,
    JNIGlobalRefType = 2,
    JNIWeakGlobalRefType = 3
} jobjectRefType;

typedef struct JNINativeMethod {
    char *name;
    char *signature;
    void *fnPtr;
} JNINativeMethod;

struct JNINativeInterface_;
struct JNIInvokeInterface_;

typedef const struct JNINativeInterface_ *JNIEnv;
typedef const struct JNIInvokeInterface_ *JavaVM;

typedef struct JavaVMOption {
    char *optionString;
    void *extraInfo;
} JavaVMOption;

typedef struct JavaVMInitArgs {
    jint version;
    jint nOptions;
    JavaVMOption *options;
    jboolean ignoreUnrecognized;
} JavaVMInitArgs;

typedef struct JavaVMAttachArgs {
    jint version;
    char *name;
    jobject group;
} JavaVMAttachArgs;

/* ------------------------------------------------------------------ */
/* JNI 1.6 原生接口：233 个槽，0..232。                                 */
/* 字段顺序直接按标准 jni.h；不要重排、不要增删。                       */
/* ------------------------------------------------------------------ */
struct JNINativeInterface_ {

    void *reserved0;
    void *reserved1;
    void *reserved2;

    void *reserved3;
    jint (*GetVersion)(JNIEnv *env);

    jclass (*DefineClass)
      (JNIEnv *env, const char *name, jobject loader, const jbyte *buf,
       jsize len);
/* 槽 6：按内部名查找类。 */
    jclass (*FindClass)
      (JNIEnv *env, const char *name);

    jmethodID (*FromReflectedMethod)
      (JNIEnv *env, jobject method);
    jfieldID (*FromReflectedField)
      (JNIEnv *env, jobject field);

    jobject (*ToReflectedMethod)
      (JNIEnv *env, jclass cls, jmethodID methodID, jboolean isStatic);

    jclass (*GetSuperclass)
      (JNIEnv *env, jclass sub);
    jboolean (*IsAssignableFrom)
      (JNIEnv *env, jclass sub, jclass sup);

    jobject (*ToReflectedField)
      (JNIEnv *env, jclass cls, jfieldID fieldID, jboolean isStatic);

    jint (*Throw)
      (JNIEnv *env, jthrowable obj);
    jint (*ThrowNew)
      (JNIEnv *env, jclass clazz, const char *msg);
    jthrowable (*ExceptionOccurred)
      (JNIEnv *env);
    void (*ExceptionDescribe)
      (JNIEnv *env);
    void (*ExceptionClear)
      (JNIEnv *env);
    void (*FatalError)
      (JNIEnv *env, const char *msg);

    jint (*PushLocalFrame)
      (JNIEnv *env, jint capacity);
    jobject (*PopLocalFrame)
      (JNIEnv *env, jobject result);

    jobject (*NewGlobalRef)
      (JNIEnv *env, jobject lobj);
    void (*DeleteGlobalRef)
      (JNIEnv *env, jobject gref);
    void (*DeleteLocalRef)
      (JNIEnv *env, jobject obj);
    jboolean (*IsSameObject)
      (JNIEnv *env, jobject obj1, jobject obj2);
    jobject (*NewLocalRef)
      (JNIEnv *env, jobject ref);
    jint (*EnsureLocalCapacity)
      (JNIEnv *env, jint capacity);

    jobject (*AllocObject)
      (JNIEnv *env, jclass clazz);
    jobject (*NewObject)
      (JNIEnv *env, jclass clazz, jmethodID methodID, ...);
    jobject (*NewObjectV)
      (JNIEnv *env, jclass clazz, jmethodID methodID, va_list args);
    jobject (*NewObjectA)
      (JNIEnv *env, jclass clazz, jmethodID methodID, const jvalue *args);

/* 槽 31：取对象类。 */
    jclass (*GetObjectClass)
      (JNIEnv *env, jobject obj);
    jboolean (*IsInstanceOf)
      (JNIEnv *env, jobject obj, jclass clazz);

/* 槽 33：取实例方法 ID。 */
    jmethodID (*GetMethodID)
      (JNIEnv *env, jclass clazz, const char *name, const char *sig);

/* 槽 34：调用返回对象的方法。 */
    jobject (*CallObjectMethod)
      (JNIEnv *env, jobject obj, jmethodID methodID, ...);
    jobject (*CallObjectMethodV)
      (JNIEnv *env, jobject obj, jmethodID methodID, va_list args);
    jobject (*CallObjectMethodA)
      (JNIEnv *env, jobject obj, jmethodID methodID, const jvalue * args);

    jboolean (*CallBooleanMethod)
      (JNIEnv *env, jobject obj, jmethodID methodID, ...);
    jboolean (*CallBooleanMethodV)
      (JNIEnv *env, jobject obj, jmethodID methodID, va_list args);
    jboolean (*CallBooleanMethodA)
      (JNIEnv *env, jobject obj, jmethodID methodID, const jvalue * args);

    jbyte (*CallByteMethod)
      (JNIEnv *env, jobject obj, jmethodID methodID, ...);
    jbyte (*CallByteMethodV)
      (JNIEnv *env, jobject obj, jmethodID methodID, va_list args);
    jbyte (*CallByteMethodA)
      (JNIEnv *env, jobject obj, jmethodID methodID, const jvalue *args);

    jchar (*CallCharMethod)
      (JNIEnv *env, jobject obj, jmethodID methodID, ...);
    jchar (*CallCharMethodV)
      (JNIEnv *env, jobject obj, jmethodID methodID, va_list args);
    jchar (*CallCharMethodA)
      (JNIEnv *env, jobject obj, jmethodID methodID, const jvalue *args);

    jshort (*CallShortMethod)
      (JNIEnv *env, jobject obj, jmethodID methodID, ...);
    jshort (*CallShortMethodV)
      (JNIEnv *env, jobject obj, jmethodID methodID, va_list args);
    jshort (*CallShortMethodA)
      (JNIEnv *env, jobject obj, jmethodID methodID, const jvalue *args);

    jint (*CallIntMethod)
      (JNIEnv *env, jobject obj, jmethodID methodID, ...);
    jint (*CallIntMethodV)
      (JNIEnv *env, jobject obj, jmethodID methodID, va_list args);
    jint (*CallIntMethodA)
      (JNIEnv *env, jobject obj, jmethodID methodID, const jvalue *args);

    jlong (*CallLongMethod)
      (JNIEnv *env, jobject obj, jmethodID methodID, ...);
    jlong (*CallLongMethodV)
      (JNIEnv *env, jobject obj, jmethodID methodID, va_list args);
    jlong (*CallLongMethodA)
      (JNIEnv *env, jobject obj, jmethodID methodID, const jvalue *args);

    jfloat (*CallFloatMethod)
      (JNIEnv *env, jobject obj, jmethodID methodID, ...);
    jfloat (*CallFloatMethodV)
      (JNIEnv *env, jobject obj, jmethodID methodID, va_list args);
    jfloat (*CallFloatMethodA)
      (JNIEnv *env, jobject obj, jmethodID methodID, const jvalue *args);

    jdouble (*CallDoubleMethod)
      (JNIEnv *env, jobject obj, jmethodID methodID, ...);
    jdouble (*CallDoubleMethodV)
      (JNIEnv *env, jobject obj, jmethodID methodID, va_list args);
    jdouble (*CallDoubleMethodA)
      (JNIEnv *env, jobject obj, jmethodID methodID, const jvalue *args);

    void (*CallVoidMethod)
      (JNIEnv *env, jobject obj, jmethodID methodID, ...);
    void (*CallVoidMethodV)
      (JNIEnv *env, jobject obj, jmethodID methodID, va_list args);
    void (*CallVoidMethodA)
      (JNIEnv *env, jobject obj, jmethodID methodID, const jvalue * args);

    jobject (*CallNonvirtualObjectMethod)
      (JNIEnv *env, jobject obj, jclass clazz, jmethodID methodID, ...);
    jobject (*CallNonvirtualObjectMethodV)
      (JNIEnv *env, jobject obj, jclass clazz, jmethodID methodID,
       va_list args);
    jobject (*CallNonvirtualObjectMethodA)
      (JNIEnv *env, jobject obj, jclass clazz, jmethodID methodID,
       const jvalue * args);

    jboolean (*CallNonvirtualBooleanMethod)
      (JNIEnv *env, jobject obj, jclass clazz, jmethodID methodID, ...);
    jboolean (*CallNonvirtualBooleanMethodV)
      (JNIEnv *env, jobject obj, jclass clazz, jmethodID methodID,
       va_list args);
    jboolean (*CallNonvirtualBooleanMethodA)
      (JNIEnv *env, jobject obj, jclass clazz, jmethodID methodID,
       const jvalue * args);

    jbyte (*CallNonvirtualByteMethod)
      (JNIEnv *env, jobject obj, jclass clazz, jmethodID methodID, ...);
    jbyte (*CallNonvirtualByteMethodV)
      (JNIEnv *env, jobject obj, jclass clazz, jmethodID methodID,
       va_list args);
    jbyte (*CallNonvirtualByteMethodA)
      (JNIEnv *env, jobject obj, jclass clazz, jmethodID methodID,
       const jvalue *args);

    jchar (*CallNonvirtualCharMethod)
      (JNIEnv *env, jobject obj, jclass clazz, jmethodID methodID, ...);
    jchar (*CallNonvirtualCharMethodV)
      (JNIEnv *env, jobject obj, jclass clazz, jmethodID methodID,
       va_list args);
    jchar (*CallNonvirtualCharMethodA)
      (JNIEnv *env, jobject obj, jclass clazz, jmethodID methodID,
       const jvalue *args);

    jshort (*CallNonvirtualShortMethod)
      (JNIEnv *env, jobject obj, jclass clazz, jmethodID methodID, ...);
    jshort (*CallNonvirtualShortMethodV)
      (JNIEnv *env, jobject obj, jclass clazz, jmethodID methodID,
       va_list args);
    jshort (*CallNonvirtualShortMethodA)
      (JNIEnv *env, jobject obj, jclass clazz, jmethodID methodID,
       const jvalue *args);

    jint (*CallNonvirtualIntMethod)
      (JNIEnv *env, jobject obj, jclass clazz, jmethodID methodID, ...);
    jint (*CallNonvirtualIntMethodV)
      (JNIEnv *env, jobject obj, jclass clazz, jmethodID methodID,
       va_list args);
    jint (*CallNonvirtualIntMethodA)
      (JNIEnv *env, jobject obj, jclass clazz, jmethodID methodID,
       const jvalue *args);

    jlong (*CallNonvirtualLongMethod)
      (JNIEnv *env, jobject obj, jclass clazz, jmethodID methodID, ...);
    jlong (*CallNonvirtualLongMethodV)
      (JNIEnv *env, jobject obj, jclass clazz, jmethodID methodID,
       va_list args);
    jlong (*CallNonvirtualLongMethodA)
      (JNIEnv *env, jobject obj, jclass clazz, jmethodID methodID,
       const jvalue *args);

    jfloat (*CallNonvirtualFloatMethod)
      (JNIEnv *env, jobject obj, jclass clazz, jmethodID methodID, ...);
    jfloat (*CallNonvirtualFloatMethodV)
      (JNIEnv *env, jobject obj, jclass clazz, jmethodID methodID,
       va_list args);
    jfloat (*CallNonvirtualFloatMethodA)
      (JNIEnv *env, jobject obj, jclass clazz, jmethodID methodID,
       const jvalue *args);

    jdouble (*CallNonvirtualDoubleMethod)
      (JNIEnv *env, jobject obj, jclass clazz, jmethodID methodID, ...);
    jdouble (*CallNonvirtualDoubleMethodV)
      (JNIEnv *env, jobject obj, jclass clazz, jmethodID methodID,
       va_list args);
    jdouble (*CallNonvirtualDoubleMethodA)
      (JNIEnv *env, jobject obj, jclass clazz, jmethodID methodID,
       const jvalue *args);

    void (*CallNonvirtualVoidMethod)
      (JNIEnv *env, jobject obj, jclass clazz, jmethodID methodID, ...);
    void (*CallNonvirtualVoidMethodV)
      (JNIEnv *env, jobject obj, jclass clazz, jmethodID methodID,
       va_list args);
    void (*CallNonvirtualVoidMethodA)
      (JNIEnv *env, jobject obj, jclass clazz, jmethodID methodID,
       const jvalue * args);

/* 槽 94：取实例字段 ID。 */
    jfieldID (*GetFieldID)
      (JNIEnv *env, jclass clazz, const char *name, const char *sig);

/* 槽 95：读对象字段。 */
    jobject (*GetObjectField)
      (JNIEnv *env, jobject obj, jfieldID fieldID);
    jboolean (*GetBooleanField)
      (JNIEnv *env, jobject obj, jfieldID fieldID);
    jbyte (*GetByteField)
      (JNIEnv *env, jobject obj, jfieldID fieldID);
    jchar (*GetCharField)
      (JNIEnv *env, jobject obj, jfieldID fieldID);
    jshort (*GetShortField)
      (JNIEnv *env, jobject obj, jfieldID fieldID);
/* 槽 100：读 int 字段。 */
    jint (*GetIntField)
      (JNIEnv *env, jobject obj, jfieldID fieldID);
    jlong (*GetLongField)
      (JNIEnv *env, jobject obj, jfieldID fieldID);
    jfloat (*GetFloatField)
      (JNIEnv *env, jobject obj, jfieldID fieldID);
    jdouble (*GetDoubleField)
      (JNIEnv *env, jobject obj, jfieldID fieldID);

    void (*SetObjectField)
      (JNIEnv *env, jobject obj, jfieldID fieldID, jobject val);
    void (*SetBooleanField)
      (JNIEnv *env, jobject obj, jfieldID fieldID, jboolean val);
    void (*SetByteField)
      (JNIEnv *env, jobject obj, jfieldID fieldID, jbyte val);
    void (*SetCharField)
      (JNIEnv *env, jobject obj, jfieldID fieldID, jchar val);
    void (*SetShortField)
      (JNIEnv *env, jobject obj, jfieldID fieldID, jshort val);
    void (*SetIntField)
      (JNIEnv *env, jobject obj, jfieldID fieldID, jint val);
    void (*SetLongField)
      (JNIEnv *env, jobject obj, jfieldID fieldID, jlong val);
    void (*SetFloatField)
      (JNIEnv *env, jobject obj, jfieldID fieldID, jfloat val);
    void (*SetDoubleField)
      (JNIEnv *env, jobject obj, jfieldID fieldID, jdouble val);

    jmethodID (*GetStaticMethodID)
      (JNIEnv *env, jclass clazz, const char *name, const char *sig);

    jobject (*CallStaticObjectMethod)
      (JNIEnv *env, jclass clazz, jmethodID methodID, ...);
    jobject (*CallStaticObjectMethodV)
      (JNIEnv *env, jclass clazz, jmethodID methodID, va_list args);
    jobject (*CallStaticObjectMethodA)
      (JNIEnv *env, jclass clazz, jmethodID methodID, const jvalue *args);

    jboolean (*CallStaticBooleanMethod)
      (JNIEnv *env, jclass clazz, jmethodID methodID, ...);
    jboolean (*CallStaticBooleanMethodV)
      (JNIEnv *env, jclass clazz, jmethodID methodID, va_list args);
    jboolean (*CallStaticBooleanMethodA)
      (JNIEnv *env, jclass clazz, jmethodID methodID, const jvalue *args);

    jbyte (*CallStaticByteMethod)
      (JNIEnv *env, jclass clazz, jmethodID methodID, ...);
    jbyte (*CallStaticByteMethodV)
      (JNIEnv *env, jclass clazz, jmethodID methodID, va_list args);
    jbyte (*CallStaticByteMethodA)
      (JNIEnv *env, jclass clazz, jmethodID methodID, const jvalue *args);

    jchar (*CallStaticCharMethod)
      (JNIEnv *env, jclass clazz, jmethodID methodID, ...);
    jchar (*CallStaticCharMethodV)
      (JNIEnv *env, jclass clazz, jmethodID methodID, va_list args);
    jchar (*CallStaticCharMethodA)
      (JNIEnv *env, jclass clazz, jmethodID methodID, const jvalue *args);

    jshort (*CallStaticShortMethod)
      (JNIEnv *env, jclass clazz, jmethodID methodID, ...);
    jshort (*CallStaticShortMethodV)
      (JNIEnv *env, jclass clazz, jmethodID methodID, va_list args);
    jshort (*CallStaticShortMethodA)
      (JNIEnv *env, jclass clazz, jmethodID methodID, const jvalue *args);

    jint (*CallStaticIntMethod)
      (JNIEnv *env, jclass clazz, jmethodID methodID, ...);
    jint (*CallStaticIntMethodV)
      (JNIEnv *env, jclass clazz, jmethodID methodID, va_list args);
    jint (*CallStaticIntMethodA)
      (JNIEnv *env, jclass clazz, jmethodID methodID, const jvalue *args);

    jlong (*CallStaticLongMethod)
      (JNIEnv *env, jclass clazz, jmethodID methodID, ...);
    jlong (*CallStaticLongMethodV)
      (JNIEnv *env, jclass clazz, jmethodID methodID, va_list args);
    jlong (*CallStaticLongMethodA)
      (JNIEnv *env, jclass clazz, jmethodID methodID, const jvalue *args);

    jfloat (*CallStaticFloatMethod)
      (JNIEnv *env, jclass clazz, jmethodID methodID, ...);
    jfloat (*CallStaticFloatMethodV)
      (JNIEnv *env, jclass clazz, jmethodID methodID, va_list args);
    jfloat (*CallStaticFloatMethodA)
      (JNIEnv *env, jclass clazz, jmethodID methodID, const jvalue *args);

    jdouble (*CallStaticDoubleMethod)
      (JNIEnv *env, jclass clazz, jmethodID methodID, ...);
    jdouble (*CallStaticDoubleMethodV)
      (JNIEnv *env, jclass clazz, jmethodID methodID, va_list args);
    jdouble (*CallStaticDoubleMethodA)
      (JNIEnv *env, jclass clazz, jmethodID methodID, const jvalue *args);

    void (*CallStaticVoidMethod)
      (JNIEnv *env, jclass cls, jmethodID methodID, ...);
    void (*CallStaticVoidMethodV)
      (JNIEnv *env, jclass cls, jmethodID methodID, va_list args);
    void (*CallStaticVoidMethodA)
      (JNIEnv *env, jclass cls, jmethodID methodID, const jvalue * args);

    jfieldID (*GetStaticFieldID)
      (JNIEnv *env, jclass clazz, const char *name, const char *sig);
    jobject (*GetStaticObjectField)
      (JNIEnv *env, jclass clazz, jfieldID fieldID);
    jboolean (*GetStaticBooleanField)
      (JNIEnv *env, jclass clazz, jfieldID fieldID);
    jbyte (*GetStaticByteField)
      (JNIEnv *env, jclass clazz, jfieldID fieldID);
    jchar (*GetStaticCharField)
      (JNIEnv *env, jclass clazz, jfieldID fieldID);
    jshort (*GetStaticShortField)
      (JNIEnv *env, jclass clazz, jfieldID fieldID);
    jint (*GetStaticIntField)
      (JNIEnv *env, jclass clazz, jfieldID fieldID);
    jlong (*GetStaticLongField)
      (JNIEnv *env, jclass clazz, jfieldID fieldID);
    jfloat (*GetStaticFloatField)
      (JNIEnv *env, jclass clazz, jfieldID fieldID);
    jdouble (*GetStaticDoubleField)
      (JNIEnv *env, jclass clazz, jfieldID fieldID);

    void (*SetStaticObjectField)
      (JNIEnv *env, jclass clazz, jfieldID fieldID, jobject value);
    void (*SetStaticBooleanField)
      (JNIEnv *env, jclass clazz, jfieldID fieldID, jboolean value);
    void (*SetStaticByteField)
      (JNIEnv *env, jclass clazz, jfieldID fieldID, jbyte value);
    void (*SetStaticCharField)
      (JNIEnv *env, jclass clazz, jfieldID fieldID, jchar value);
    void (*SetStaticShortField)
      (JNIEnv *env, jclass clazz, jfieldID fieldID, jshort value);
    void (*SetStaticIntField)
      (JNIEnv *env, jclass clazz, jfieldID fieldID, jint value);
    void (*SetStaticLongField)
      (JNIEnv *env, jclass clazz, jfieldID fieldID, jlong value);
    void (*SetStaticFloatField)
      (JNIEnv *env, jclass clazz, jfieldID fieldID, jfloat value);
    void (*SetStaticDoubleField)
      (JNIEnv *env, jclass clazz, jfieldID fieldID, jdouble value);

    jstring (*NewString)
      (JNIEnv *env, const jchar *unicode, jsize len);
    jsize (*GetStringLength)
      (JNIEnv *env, jstring str);
    const jchar *(*GetStringChars)
      (JNIEnv *env, jstring str, jboolean *isCopy);
    void (*ReleaseStringChars)
      (JNIEnv *env, jstring str, const jchar *chars);

/* 槽 167：由修改版 UTF-8 创建字符串。 */
    jstring (*NewStringUTF)
      (JNIEnv *env, const char *utf);
/* 槽 168：取修改版 UTF-8 字节长度。 */
    jsize (*GetStringUTFLength)
      (JNIEnv *env, jstring str);
/* 槽 169：取修改版 UTF-8 字符指针。 */
    const char* (*GetStringUTFChars)
      (JNIEnv *env, jstring str, jboolean *isCopy);
/* 槽 170：释放 UTF 字符指针。 */
    void (*ReleaseStringUTFChars)
      (JNIEnv *env, jstring str, const char* chars);


    jsize (*GetArrayLength)
      (JNIEnv *env, jarray array);

    jobjectArray (*NewObjectArray)
      (JNIEnv *env, jsize len, jclass clazz, jobject init);
    jobject (*GetObjectArrayElement)
      (JNIEnv *env, jobjectArray array, jsize index);
    void (*SetObjectArrayElement)
      (JNIEnv *env, jobjectArray array, jsize index, jobject val);

    jbooleanArray (*NewBooleanArray)
      (JNIEnv *env, jsize len);
    jbyteArray (*NewByteArray)
      (JNIEnv *env, jsize len);
    jcharArray (*NewCharArray)
      (JNIEnv *env, jsize len);
    jshortArray (*NewShortArray)
      (JNIEnv *env, jsize len);
    jintArray (*NewIntArray)
      (JNIEnv *env, jsize len);
    jlongArray (*NewLongArray)
      (JNIEnv *env, jsize len);
    jfloatArray (*NewFloatArray)
      (JNIEnv *env, jsize len);
    jdoubleArray (*NewDoubleArray)
      (JNIEnv *env, jsize len);

    jboolean * (*GetBooleanArrayElements)
      (JNIEnv *env, jbooleanArray array, jboolean *isCopy);
    jbyte * (*GetByteArrayElements)
      (JNIEnv *env, jbyteArray array, jboolean *isCopy);
    jchar * (*GetCharArrayElements)
      (JNIEnv *env, jcharArray array, jboolean *isCopy);
    jshort * (*GetShortArrayElements)
      (JNIEnv *env, jshortArray array, jboolean *isCopy);
    jint * (*GetIntArrayElements)
      (JNIEnv *env, jintArray array, jboolean *isCopy);
    jlong * (*GetLongArrayElements)
      (JNIEnv *env, jlongArray array, jboolean *isCopy);
    jfloat * (*GetFloatArrayElements)
      (JNIEnv *env, jfloatArray array, jboolean *isCopy);
    jdouble * (*GetDoubleArrayElements)
      (JNIEnv *env, jdoubleArray array, jboolean *isCopy);

    void (*ReleaseBooleanArrayElements)
      (JNIEnv *env, jbooleanArray array, jboolean *elems, jint mode);
    void (*ReleaseByteArrayElements)
      (JNIEnv *env, jbyteArray array, jbyte *elems, jint mode);
    void (*ReleaseCharArrayElements)
      (JNIEnv *env, jcharArray array, jchar *elems, jint mode);
    void (*ReleaseShortArrayElements)
      (JNIEnv *env, jshortArray array, jshort *elems, jint mode);
    void (*ReleaseIntArrayElements)
      (JNIEnv *env, jintArray array, jint *elems, jint mode);
    void (*ReleaseLongArrayElements)
      (JNIEnv *env, jlongArray array, jlong *elems, jint mode);
    void (*ReleaseFloatArrayElements)
      (JNIEnv *env, jfloatArray array, jfloat *elems, jint mode);
    void (*ReleaseDoubleArrayElements)
      (JNIEnv *env, jdoubleArray array, jdouble *elems, jint mode);

    void (*GetBooleanArrayRegion)
      (JNIEnv *env, jbooleanArray array, jsize start, jsize l, jboolean *buf);
    void (*GetByteArrayRegion)
      (JNIEnv *env, jbyteArray array, jsize start, jsize len, jbyte *buf);
    void (*GetCharArrayRegion)
      (JNIEnv *env, jcharArray array, jsize start, jsize len, jchar *buf);
    void (*GetShortArrayRegion)
      (JNIEnv *env, jshortArray array, jsize start, jsize len, jshort *buf);
    void (*GetIntArrayRegion)
      (JNIEnv *env, jintArray array, jsize start, jsize len, jint *buf);
    void (*GetLongArrayRegion)
      (JNIEnv *env, jlongArray array, jsize start, jsize len, jlong *buf);
    void (*GetFloatArrayRegion)
      (JNIEnv *env, jfloatArray array, jsize start, jsize len, jfloat *buf);
    void (*GetDoubleArrayRegion)
      (JNIEnv *env, jdoubleArray array, jsize start, jsize len, jdouble *buf);

    void (*SetBooleanArrayRegion)
      (JNIEnv *env, jbooleanArray array, jsize start, jsize l, const jboolean *buf);
    void (*SetByteArrayRegion)
      (JNIEnv *env, jbyteArray array, jsize start, jsize len, const jbyte *buf);
    void (*SetCharArrayRegion)
      (JNIEnv *env, jcharArray array, jsize start, jsize len, const jchar *buf);
    void (*SetShortArrayRegion)
      (JNIEnv *env, jshortArray array, jsize start, jsize len, const jshort *buf);
    void (*SetIntArrayRegion)
      (JNIEnv *env, jintArray array, jsize start, jsize len, const jint *buf);
    void (*SetLongArrayRegion)
      (JNIEnv *env, jlongArray array, jsize start, jsize len, const jlong *buf);
    void (*SetFloatArrayRegion)
      (JNIEnv *env, jfloatArray array, jsize start, jsize len, const jfloat *buf);
    void (*SetDoubleArrayRegion)
      (JNIEnv *env, jdoubleArray array, jsize start, jsize len, const jdouble *buf);

/* 槽 215：注册 native 方法。 */
    jint (*RegisterNatives)
      (JNIEnv *env, jclass clazz, const JNINativeMethod *methods,
       jint nMethods);
/* 槽 216：注销 native 方法。 */
    jint (*UnregisterNatives)
      (JNIEnv *env, jclass clazz);

    jint (*MonitorEnter)
      (JNIEnv *env, jobject obj);
    jint (*MonitorExit)
      (JNIEnv *env, jobject obj);

/* 槽 219：取 JavaVM。 */
    jint (*GetJavaVM)
      (JNIEnv *env, JavaVM **vm);

    void (*GetStringRegion)
      (JNIEnv *env, jstring str, jsize start, jsize len, jchar *buf);
    void (*GetStringUTFRegion)
      (JNIEnv *env, jstring str, jsize start, jsize len, char *buf);

    void * (*GetPrimitiveArrayCritical)
      (JNIEnv *env, jarray array, jboolean *isCopy);
    void (*ReleasePrimitiveArrayCritical)
      (JNIEnv *env, jarray array, void *carray, jint mode);

    const jchar * (*GetStringCritical)
      (JNIEnv *env, jstring string, jboolean *isCopy);
    void (*ReleaseStringCritical)
      (JNIEnv *env, jstring string, const jchar *cstring);

    jweak (*NewWeakGlobalRef)
       (JNIEnv *env, jobject obj);
    void (*DeleteWeakGlobalRef)
       (JNIEnv *env, jweak ref);

/* 槽 228：检查 pending 异常。 */
    jboolean (*ExceptionCheck)
       (JNIEnv *env);

    jobject (*NewDirectByteBuffer)
       (JNIEnv* env, void* address, jlong capacity);
    void* (*GetDirectBufferAddress)
       (JNIEnv* env, jobject buf);
    jlong (*GetDirectBufferCapacity)
       (JNIEnv* env, jobject buf);

    /* New JNI 1.6 Features */

/* 槽 232：取引用类型。 */
    jobjectRefType (*GetObjectRefType)
        (JNIEnv* env, jobject obj);
};

/* ------------------------------------------------------------------ */
/* JNI 1.6 调用接口：8 个槽，0..7。                                     */
/* ------------------------------------------------------------------ */
struct JNIInvokeInterface_ {
    void *reserved0;
    void *reserved1;
    void *reserved2;

    jint (JNICALL *DestroyJavaVM)(JavaVM *vm);
    jint (JNICALL *AttachCurrentThread)(JavaVM *vm, void **penv, void *args);
    jint (JNICALL *DetachCurrentThread)(JavaVM *vm);
    jint (JNICALL *GetEnv)(JavaVM *vm, void **penv, jint version);
    jint (JNICALL *AttachCurrentThreadAsDaemon)(JavaVM *vm, void **penv, void *args);
};

/* 编译期 ABI 校验：本任务要求的关键槽位。 */
#if defined(__cplusplus)
#define JNI_STATIC_ASSERT(cond, msg) static_assert(cond, msg)
#else
#define JNI_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#endif

JNI_STATIC_ASSERT(sizeof(struct JNINativeInterface_) == 233 * sizeof(void *),
                  "JNINativeInterface 必须是 233 槽");
JNI_STATIC_ASSERT(sizeof(struct JNIInvokeInterface_) == 8 * sizeof(void *),
                  "JNIInvokeInterface 必须是 8 槽");

JNI_STATIC_ASSERT(offsetof(struct JNINativeInterface_, FindClass) == 6 * sizeof(void *),
                  "FindClass 槽位必须是 6");
JNI_STATIC_ASSERT(offsetof(struct JNINativeInterface_, GetObjectClass) == 31 * sizeof(void *),
                  "GetObjectClass 槽位必须是 31");
JNI_STATIC_ASSERT(offsetof(struct JNINativeInterface_, GetMethodID) == 33 * sizeof(void *),
                  "GetMethodID 槽位必须是 33");
JNI_STATIC_ASSERT(offsetof(struct JNINativeInterface_, CallObjectMethod) == 34 * sizeof(void *),
                  "CallObjectMethod 槽位必须是 34");
JNI_STATIC_ASSERT(offsetof(struct JNINativeInterface_, GetFieldID) == 94 * sizeof(void *),
                  "GetFieldID 槽位必须是 94");
JNI_STATIC_ASSERT(offsetof(struct JNINativeInterface_, GetObjectField) == 95 * sizeof(void *),
                  "GetObjectField 槽位必须是 95");
JNI_STATIC_ASSERT(offsetof(struct JNINativeInterface_, GetIntField) == 100 * sizeof(void *),
                  "GetIntField 槽位必须是 100");
JNI_STATIC_ASSERT(offsetof(struct JNINativeInterface_, NewStringUTF) == 167 * sizeof(void *),
                  "NewStringUTF 槽位必须是 167");
JNI_STATIC_ASSERT(offsetof(struct JNINativeInterface_, GetStringUTFLength) == 168 * sizeof(void *),
                  "GetStringUTFLength 槽位必须是 168");
JNI_STATIC_ASSERT(offsetof(struct JNINativeInterface_, GetStringUTFChars) == 169 * sizeof(void *),
                  "GetStringUTFChars 槽位必须是 169");
JNI_STATIC_ASSERT(offsetof(struct JNINativeInterface_, ReleaseStringUTFChars) == 170 * sizeof(void *),
                  "ReleaseStringUTFChars 槽位必须是 170");
JNI_STATIC_ASSERT(offsetof(struct JNINativeInterface_, RegisterNatives) == 215 * sizeof(void *),
                  "RegisterNatives 槽位必须是 215");
JNI_STATIC_ASSERT(offsetof(struct JNINativeInterface_, UnregisterNatives) == 216 * sizeof(void *),
                  "UnregisterNatives 槽位必须是 216");
JNI_STATIC_ASSERT(offsetof(struct JNINativeInterface_, GetJavaVM) == 219 * sizeof(void *),
                  "GetJavaVM 槽位必须是 219");
JNI_STATIC_ASSERT(offsetof(struct JNINativeInterface_, ExceptionCheck) == 228 * sizeof(void *),
                  "ExceptionCheck 槽位必须是 228");
JNI_STATIC_ASSERT(offsetof(struct JNINativeInterface_, GetObjectRefType) == 232 * sizeof(void *),
                  "GetObjectRefType 槽位必须是 232");

JNI_STATIC_ASSERT(offsetof(struct JNIInvokeInterface_, AttachCurrentThread) == 4 * sizeof(void *),
                  "AttachCurrentThread 槽位必须是 4");
JNI_STATIC_ASSERT(offsetof(struct JNIInvokeInterface_, GetEnv) == 6 * sizeof(void *),
                  "GetEnv 槽位必须是 6");

/* 运行期自检：初始化时逐项核对关键槽，返回第一个失败槽号，-1 表示通过。 */
static inline int jni_abi_check(void)
{
    if (sizeof(struct JNINativeInterface_) != 233 * sizeof(void *)) return 0;
    if (sizeof(struct JNIInvokeInterface_) != 8 * sizeof(void *)) return 3;
    if (offsetof(struct JNINativeInterface_, FindClass) != 6 * sizeof(void *)) return 6;
    if (offsetof(struct JNINativeInterface_, GetMethodID) != 33 * sizeof(void *)) return 33;
    if (offsetof(struct JNINativeInterface_, CallObjectMethod) != 34 * sizeof(void *)) return 34;
    if (offsetof(struct JNINativeInterface_, GetFieldID) != 94 * sizeof(void *)) return 94;
    if (offsetof(struct JNINativeInterface_, GetObjectField) != 95 * sizeof(void *)) return 95;
    if (offsetof(struct JNINativeInterface_, GetIntField) != 100 * sizeof(void *)) return 100;
    if (offsetof(struct JNINativeInterface_, NewStringUTF) != 167 * sizeof(void *)) return 167;
    if (offsetof(struct JNINativeInterface_, GetStringUTFChars) != 169 * sizeof(void *)) return 169;
    if (offsetof(struct JNINativeInterface_, RegisterNatives) != 215 * sizeof(void *)) return 215;
    if (offsetof(struct JNINativeInterface_, GetJavaVM) != 219 * sizeof(void *)) return 219;
    if (offsetof(struct JNINativeInterface_, GetStringUTFLength) != 168 * sizeof(void *)) return 168;
    if (offsetof(struct JNINativeInterface_, ExceptionCheck) != 228 * sizeof(void *)) return 228;
    if (offsetof(struct JNIInvokeInterface_, AttachCurrentThread) != 4 * sizeof(void *)) return 4;
    return -1;
}

#ifdef __cplusplus
}
#endif

#endif /* NATIVE_JNI_NEXT_JNI_ABI_H */
