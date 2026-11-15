# TLS 相关的 Java 接口（签名与行为，无反汇编）

本文件只记录兼容层需要的**类名、方法签名与可观察行为**，用于说明运行时为何需要这些实现；
不包含任何来自游戏 APK 的反汇编指令文本（早前的 `SSLUtils.smali.txt` 已从仓库删除）。

## 运行时依赖的接口

| 类 | 方法 | 行为约定 |
|---|---|---|
| `javax/net/ssl/SSLSocketFactory` | `createSocket(Socket,String,int,boolean)` | 返回已建立的 socket；运行时以 JNI 回调交给 mbedTLS 传输层 |
| `javax/net/ssl/SSLContext` | `getSocketFactory()` | 返回工厂单例；`init(null,null,null)` 后可用 |
| `javax/net/ssl/SSLSocket` | `startHandshake()` / `getSession()` | 握手由传输层驱动；`getSession()` 至少返回非空对象 |
| `java/net/Socket` | `connect(SocketAddress,int)` / `getInputStream()` / `getOutputStream()` | 运行时的 TLS 传输以这组方法为入口 |
| `java/util/concurrent/CountDownLatch` | `await()` / `countDown()` | 握手完成信号 |

## 行为要点（外部可观察，用于对拍）

1. 握手失败时抛出 `javax/net/ssl/SSLException` 或其子类，由上层转成业务错误码；
2. 连接超时与读超时分别抛出 `java/net/SocketTimeoutException` 与 `java/io/InterruptedIOException`；
3. 证书链校验失败抛 `javax/net/ssl/SSLHandshakeException`，不静默降级；
4. 以上行为均由运行时的 `ssl_runtime.c` / `tls_client.c` 提供，可在不做反汇编的前提下复现。

## 生成关系

`sdk-runtime-spike/build_tls_candidate.py` 不再读取任何 smali 文件；TLS 胶水由 `tls-java-next/` 内的 C 源文件与本文档描述的签名共同决定。

