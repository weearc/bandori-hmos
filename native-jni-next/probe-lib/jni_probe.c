/*
 * 自有 JNI 探针库：libjni_probe.so
 *
 * 用途：只测试 native-jni-next 的自有 JNI 桥，不接触、不加载任何原厂库。
 * 本文件使用 native-jni-next/jni_abi.h，而不是 NDK/JDK 的 jni.h。
 *
 * 覆盖的桥能力：
 *   - JNI_OnLoad 版本协商（返回 JNI_VERSION_1_6）；
 *   - Java_ 短名/长名符号查找（staticAdd / concat / throwProbe / fieldProbe）；
 *   - 字符串往返（GetStringUTFChars / ReleaseStringUTFChars / NewStringUTF）；
 *   - 异常（FindClass + ThrowNew）；
 *   - 字段访问（GetObjectClass + GetFieldID + GetIntField）；
 *   - RegisterNatives 注册实例方法 registeredAdd。
 *
 * 注意：本库不调用未实现槽；任何 JNI 调用失败都把异常留在 pending 状态，
 * 由自有桥在 native 返回后交回 DVM。
 */

#include "jni_abi.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if defined(__GNUC__)
#define PROBE_EXPORT __attribute__((visibility("default")))
#else
#define PROBE_EXPORT
#endif

static _Atomic int g_onload_count;

/* 记录 JNI_OnLoad 被调用，并声明只支持 JNI 1.6。 */
PROBE_EXPORT jint JNICALL
JNI_OnLoad(JavaVM *vm, void *reserved)
{
    (void)vm;
    (void)reserved;
    atomic_fetch_add_explicit(&g_onload_count, 1, memory_order_relaxed);
    return JNI_VERSION_1_6;
}

/* 可选：JNI 卸载回调。自有桥目前不调用它，仅作为标准导出存在。 */
PROBE_EXPORT void JNICALL
JNI_OnUnload(JavaVM *vm, void *reserved)
{
    (void)vm;
    (void)reserved;
}

/* 诊断：返回 JNI_OnLoad 被调用的次数。 */
PROBE_EXPORT jint JNICALL
Java_com_hmos_probe_JniProbe_onLoadCount(JNIEnv *env, jclass clazz)
{
    (void)env;
    (void)clazz;
    return (jint)atomic_load_explicit(&g_onload_count, memory_order_relaxed);
}

/* 静态方法：整数加法。 */
PROBE_EXPORT jint JNICALL
Java_com_hmos_probe_JniProbe_staticAdd(JNIEnv *env, jclass clazz, jint a,
                                       jint b)
{
    (void)env;
    (void)clazz;
    return (jint)((uint32_t)a + (uint32_t)b);
}

/*
 * 实例方法：把两个 Java String 用 UTF-8 视图拼接后返回新 String。
 * 失败时保留 pending 异常并返回 NULL，不伪造结果。
 */
PROBE_EXPORT jstring JNICALL
Java_com_hmos_probe_JniProbe_concat(JNIEnv *env, jobject thiz, jstring a,
                                    jstring b)
{
    (void)thiz;
    if (!env || !a || !b) return NULL;

    const char *sa = (*env)->GetStringUTFChars(env, a, NULL);
    if (!sa) return NULL;
    const char *sb = (*env)->GetStringUTFChars(env, b, NULL);
    if (!sb) {
        (*env)->ReleaseStringUTFChars(env, a, sa);
        return NULL;
    }

    size_t la = strlen(sa);
    size_t lb = strlen(sb);
    char *buf = (char *)malloc(la + lb + 1);
    if (!buf) {
        (*env)->ReleaseStringUTFChars(env, a, sa);
        (*env)->ReleaseStringUTFChars(env, b, sb);
        return NULL;
    }
    memcpy(buf, sa, la);
    memcpy(buf + la, sb, lb);
    buf[la + lb] = '\0';

    jstring out = (*env)->NewStringUTF(env, buf);
    free(buf);
    (*env)->ReleaseStringUTFChars(env, a, sa);
    (*env)->ReleaseStringUTFChars(env, b, sb);
    return out;
}

/* 静态方法：构造并抛出 IllegalStateException，验证 ThrowNew 路径。 */
PROBE_EXPORT void JNICALL
Java_com_hmos_probe_JniProbe_throwProbe(JNIEnv *env, jclass clazz)
{
    (void)clazz;
    if (!env) return;
    jclass ex = (*env)->FindClass(env, "java/lang/IllegalStateException");
    if (!ex) return; /* FindClass 已留下 pending 异常。 */
    (*env)->ThrowNew(env, ex, "jni_probe throwProbe");
}

/* 实例方法：读取 JniProbe.value 字段并返回。 */
PROBE_EXPORT jint JNICALL
Java_com_hmos_probe_JniProbe_fieldProbe(JNIEnv *env, jobject thiz)
{
    if (!env || !thiz) return 0;
    jclass cls = (*env)->GetObjectClass(env, thiz);
    if (!cls) return 0;
    jfieldID fid = (*env)->GetFieldID(env, cls, "value", "I");
    if (!fid) return 0;
    return (*env)->GetIntField(env, thiz, fid);
}

/* RegisterNatives 的目标函数：实例方法 registeredAdd(II)I。 */
static jint JNICALL
probe_registered_add(JNIEnv *env, jobject thiz, jint a, jint b)
{
    (void)env;
    (void)thiz;
    return (jint)((uint32_t)a + (uint32_t)b);
}

/* 静态方法：通过 RegisterNatives 注册 registeredAdd。 */
PROBE_EXPORT jint JNICALL
Java_com_hmos_probe_JniProbe_registerNativesProbe(JNIEnv *env, jclass clazz)
{
    if (!env || !clazz) return JNI_ERR;
    static const JNINativeMethod methods[] = {
        {(char *)"registeredAdd", (char *)"(II)I", (void *)probe_registered_add},
    };
    return (*env)->RegisterNatives(env, clazz, methods,
                                   (jint)(sizeof(methods) / sizeof(methods[0])));
}

/*
 * 若原生库被直接按短名绑定，registeredAdd 也需要一个 Java_ 短名符号；
 * RegisterNatives 成功后桥会优先命中注册表，两条路径都可用。
 */
PROBE_EXPORT jint JNICALL
Java_com_hmos_probe_JniProbe_registeredAdd(JNIEnv *env, jobject thiz, jint a,
                                           jint b)
{
    return probe_registered_add(env, thiz, a, b);
}
