package com.hmos.probe;

/**
 * libjni_probe.so 的 Java 夹具声明。
 *
 * 该文件只描述测试类，不参与 .so 构建，也不代表原厂库或真实应用类。
 * RegisterNatives 要求被注册的方法在 DEX 中确实是 native 方法，因此
 * registeredAdd 必须在这里声明为 native。
 */
public class JniProbe {
    /** fieldProbe 读取的实例字段。 */
    public int value;

    public static native int staticAdd(int a, int b);

    public native String concat(String a, String b);

    public static native void throwProbe();

    public native int fieldProbe();

    /** 由 RegisterNatives 注册 registeredAdd(II)I。 */
    public static native int registerNativesProbe();

    public native int registeredAdd(int a, int b);

    /** 诊断：JNI_OnLoad 被调用的次数。 */
    public static native int onLoadCount();

    static {
        System.loadLibrary("jni_probe");
    }
}
