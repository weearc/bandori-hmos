# Unity initJni 原生入口：定点反汇编

只读静态证据，未执行代码或断言72ey异常原因。目标 `rea-analysis-20261104/libunity.so` SHA-256 `25c511a3f769b875de870a6baaf789fa894f90079fc48446e0bfc3ce488c6e8a`，AArch64；命令为 SDK `llvm-objdump -d --start-address=... --stop-address=...`，原 ELF 程序头将下列代码 VA 映射到文件 offset `VA - 0x4000`。objdump 展示的最近导出名只是地址标注，不代表这些内部函数的名称或用途。

72ey 观察到自己库的 JNI_OnLoad 运行地址 `0x5e517146b4`，原 ELF dynsym 值 `0x9fe6b4`，因此该次推算 load bias `0x5e50d16000`。实际 observer 记录 initJni 地址 `0x5e517138b0`，减 bias 得 VA `0x9fd8b0`，file offset `0x9f98b0`；这些绝对地址只属于72ey，其他进程必须独立重算并校验映射区间。`batch-validation/unity-bootstrap-20261109/initjni-entry-9fd8b0-objdump.txt` 保存入口反汇编。

入口只有：

```asm
9fd8b0  mov x0, x1
9fd8b4  mov x1, x2
9fd8b8  b   0x9e4988
```

按照 JNI 非静态方法 ABI，这将 player receiver/context 移到后续函数的前两个参数，并未传入原 `JNIEnv*`。VA `0x9e4988`（文件 offset `0x9e0988`）保存 receiver (`x20`) 和 context (`x19`)，首两个直接调用分别是 `0x16a63c8`、`0x16a4130`；后续间接槽位如下。路径会因指针、异常和返回值分支而不同：这张表是可达静态顺序，不证明任何步骤在72ey运行成功。

| VA | 指令/语义（JNI槽位按项目 `jni_abi.h` 验证） |
|---|---|
| `0x16a63c8` | 由全局 VA `0x18c4cc8` 取指针；若非空，`0x16a6400` 间接调用 JavaVM表 `+0x30`：GetEnv，参数版本 `0x10006`。若结果环境为空，转到后续环境获取路径。 |
| `0x16a4324` | 对同一全局 JavaVM 调 GetEnv；环境为空时 `0x16a438c` 经 JavaVM表 `+0x20` 调 AttachCurrentThread。 |
| `0x16a6430` | 获取到环境后，JNI表 `+0x98`（第19槽）PushLocalFrame，capacity=64；后调用 `0x16a44f4`，其首个JNI调用为表 `+0x720`（第228槽）ExceptionCheck。 |
| `0x16a4130` | 调 pthread_getspecific；若无TLS数据，malloc 260字节并清零，再pthread_setspecific；依分支可能继续使用JNI。此处的TLS key、返回值及内存必须按真实结果观察，不能仅由导入存在推断失败。 |
| `0x9e49bc` | JNI表 `+0xa8`（第21槽）NewGlobalRef，参数为 UnityPlayer receiver，结果保存于库全局。 |
| `0x9e49e8`, `0x9e4a04` | JNI表 `+0xf8`（第31槽）GetObjectClass 两次，第二次传第一次结果。 |
| `0x9e4a24` | JNI表 `+0x30`（第6槽）FindClass，字符串 `java/lang/ClassLoader`（VA `0x1ea983`）。 |
| `0x9e4a4c` | JNI表 `+0x108`（第33槽）GetMethodID，传前面获取的class及 `getClassLoader` / `()Ljava/lang/ClassLoader;`（字符串VA `0x2311c8`、`0x228253`）。 |
| `0x9d6d0c` | 由 `0x9e4a6c` 调用的包装函数，经 JNI表 `+0x118`（第35槽）CallObjectMethodV 调用刚获取的method ID；这是变参/va_list ABI边界。 |
| `0x9e4a80` | JNI表 `+0xa8` NewGlobalRef，参数为上一调用返回值。 |
| `0x9e4ab0` | JNI表 `+0x388`（第113槽）GetStaticMethodID，名称 `forName`，描述符 `(Ljava/lang/String;ZLjava/lang/ClassLoader;)Ljava/lang/Class;`（字符串VA `0x19c299`、`0x1d1366`）。之后直接调用 `0x9cb900`、`0x9cba00`、`0x66a544`、`0xf12f2c`，此处未追踪其功能。 |

项目桥已有对应 GetEnv、PushLocalFrame、ExceptionCheck、NewGlobalRef、GetObjectClass、FindClass、GetMethodID、CallObjectMethodV 和 GetStaticMethodID 入口；`discovery_runtime.c` 也有针对 Class.getClassLoader/forName 的测试，但这不证明 native 实际调用路径可用或类型解析正确。`NewGlobalRef` 使用受控JNI token，`GetObjectClass` 连续两次、ClassLoader查找、method ID、CallObjectMethodV参数与异常状态均值得观察。不得提前断言缺类、va_list、TLS或其他任一项就是当前故障。

下一次若得到72ez信号 PC/LR，先从该进程自己的映射范围/实际 bias 验证并换算，再比较上表和真实 fault log；72ey 地址不可跨进程复用。若缺signal且进程消失，还须区分主动exit、abort、系统杀进程、超时或控制器 force-stop。可在各实际JNI槽入口/返回添加有界、持久化的诊断标记和真实返回/异常状态，避免持内部锁调用可重入日志；监测不应吞异常或修改返回。当前只是定位后续可观测的边界。
