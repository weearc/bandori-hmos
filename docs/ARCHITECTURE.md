# 架构说明

## 总体

```text
ArkTS 宿主 (host/)
  EntryAbility.ets      应用生命周期、窗口、权限、hdc 侧可观测的状态文件
  Index.ets             UI 与事件面：EGL surface 创建、触摸分发、音频、输入法、RPC 响应
  cpp/probe.cpp         NAPI：NativeWindow/EGL/音频/网络快照的宿主侧实现
  cpp/payload.cpp       把宿主能力以 C ABI 暴露给 libsdktrace.so
      │
      ▼  (dlopen + dlsym)
libsdktrace.so（我们自己的运行时，源码在各 *-next 模块）
  dvm.c / dex.c         DEX 解释器：类加载、解释执行、GC、异常
  jni_bridge.c          JNI 桥：guest 调 Java → 我们的实现；反射/对象模型
  elf_loader.c          guest ELF 加载：段映射、重定位、TLS/init_array、GOT 修复
  unity_player_probe.c  Unity 播放器与生命周期驱动（GIL、线程状态、帧节拍）
  ssl_runtime.c / tls_client.c / cert_runtime.c   Java TLS + mbedTLS 传输
  sqlite_runtime.c      SQLite 适配
  network_runtime.c     ConnectivityManager / 网络状态桥
  diagnostic_log.c      有界异步日志（worker 独占 IO，热路径只入队）
      │
      ▼
guest：classes*.dex + libil2cpp.so + libunity.so + 游戏自带 so（由用户自备）
```

## 关键设计约束

| 约束 | 原因 |
|---|---|
| `moving=0`（GC 不移动对象） | guest 的 ARM64 原生代码直接持有 Java 对象地址，对象一移动就崩 |
| 热路径零文件 IO | 宿主帧线程/请求线程一旦做 `fsync` 就会拉长帧（曾经因此每 5 秒卡 2.5 秒） |
| 日志有界（2048 槽 × 4096B + 1MiB 文件 + 1 备份） | 防止诊断本身撑爆内存或磁盘 |
| GIL 只在必要处持有，且不做阻塞等待 | Java 回调与渲染线程都要抢这把锁 |
| 页大小只做 4KB 同余的段映射 | 现有 guest 库只满足 4KB；16KB 内核会报 `E-ELF-PAGE-CONGRUENCE` |

## 目录与模块职责

| 模块 | 职责 |
|---|---|
| `sdk-init-next` | 解释器核心（dvm、dex、GC、线程、Handler、系统属性、资源表、日志） |
| `native-elf-loader-next` | guest ELF 加载、符号解析、页几何检查、socket 计数、诊断日志 |
| `native-jni-next` | JNI 环境与调用桥 |
| `tls-java-next` | Java 侧 TLS（SSLSocket/证书/信任链）与 native TLS 客户端 |
| `tls-transport-next` | mbedTLS 3.6.7 的裁剪与构建 |
| `sqlite-backend-next` | SQLite 适配层 |
| `asset-reader-next` | Unity 的 AssetBundle/资源读取 |
| `cri-audio-adaptation` | CRIWARE 音频适配 |
| `unity-jni-bridge` / `unity-batch-validation` | Unity 与 JNI 的桥接与自检 |
| 其余 `*-next` | 各 Java API 的按需实现（集合、时间、反射、并发、文件、网络等） |
| `sdk-runtime-spike` | 构建编排（`build_tls_candidate.py`）与保留的基线模板 |

## 启动时序（简化）

```text
EntryAbility.onCreate
  └─ probe.init(): 加载 payload，创建 EGL/NativeWindow
Index 首帧
  └─ dlopen(libsdktrace.so) → sdk_main()
       ├─ 初始化 DVM（类表、GC1 自检、JNI、TLS、SQLite 适配）
       ├─ elf_loader 映射 guest 的 libil2cpp.so / libunity.so
       ├─ 解包 rawfile/sdkdex/base.apk → 私有目录（首次）
       └─ 进入 Unity 的 main loop（mono_ns 开始递增）
```
