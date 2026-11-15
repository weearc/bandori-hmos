# UnityPlayer 初始化前置：本地证据复用

2026-11-10 update: the table below is the historical 72ee snapshot. Real72fd/72fe runs have passed the listed pre-init sequence and initJni returned. 72fe now stops afterward at i0.a Context.getTheme. Use root README/handoff and codex-72fe-status.md for current evidence. The static ordering below remains a useful index, not a current missing-API list.


2026-11-10。依据既有 `runtime-route-review/rea-original-runtime/method-UnityPlayer-init-context-lifecycle.json` 的 JADX 构造函数正文及原 APK 身份；没有重新逆向。动态最新点：72ee 停在 FrameLayout(Context)，因此后续列表均是静态顺序，不能当作已通过。

| 顺序 | 原构造阶段 | 实现/证据边界 |
| --- | --- | --- |
| 1 | super(context)：FrameLayout(Context) | 72ee 真机缺方法；新增 view_state_runtime 仅建立 Java 对象/Context 状态，尚待设备验收；不代表布局绘图 |
| 2 | new Handler() | 原主 guest Looper 已通过身份检查；完整宿主 UI 队列及事件泵未绑定，不能等同真实 ArkUI 主线程 |
| 3 | W0、ConcurrentLinkedQueue、U0/R0/Q0 对象初始化 | 各 Java 初始化器可能继续触发 API；Thread 类构造与真实启动是不同能力 |
| 4 | getUnityNativeLibraryPath(context)、g0.a(...) | 显式配置的 ApplicationInfo.nativeLibraryDir 已加入；此处尚未动态验收，不能预判 g0 插件行为 |
| 5 | Activity.getRequestedOrientation() | 当前 Activity runtime 没有明确该方法的实现；不能返回固定横屏冒充真实宿主方向 |
| 6 | Activity.getIntent().getData() | driver 提供真实 Intent 对象、空 extras，并明确 original_launcher_route=0；不代表 SDK 生产导航 |
| 7 | EarlyEnableFullScreenIfEnabled() | 依赖真实窗口/元数据；窗口 facade 和标题请求不代表全屏成功；最终目标必须横屏全屏 |
| 8 | getResources().getConfiguration()、自然方向计算 | Context/资源/显示配置后端已有实现，新 View 必须转发真实同一资源对象；Unity 具体使用仍待验收 |
| 9 | splash 配置、可选 splash View 创建和 addView | 不能硬改 splash metadata 跳过；当前无真实 Android View 渲染树，addView 必须明确能力边界 |
| 10 | preloadJavaPlugins() → loadNative(libdir) | 72ea 单独原 NativeLoader.load 整链通过；原构造内的 plugin/preload 及路径组合尚未验收 |
| 11 | W0.d() 成功分支 → initJni(context) | 失败分支原代码创建 AlertDialog 并返回；必须记录真实分支，不得只以 ctor 返回认定 native 初始化 |

initJni 之后仍有 i0 GL View/addView/bringChildToFront、getWindow/hideStatusBar、phone/clipboard/Camera2/HFP、m_MainThread.start 和返回键处理。不能用 initJni 返回代替这些阶段、IL2CPP runtime 初始化或游戏首帧。

当前窗口请求的精确结论：72ed 的 OH setter 返回，getter 报设备不支持；72ee 把明确 unsupported 映射 Java false，原 onCreate 忽略此值并继续。没有把 setter 无异常当作 effective=true。Android guest title decor 与 OH 自由窗口系统 caption 不是同一层，仍需真正的宿主窗口/内容策略。

官方语义参考：[Android Activity.requestWindowFeature](https://developer.android.com/reference/android/app/Activity#requestWindowFeature(int))、[HarmonyOS Window 标题接口](https://developer.huawei.com/consumer/en/doc/harmonyos-references/arkts-apis-window-window/)。查询日期 2026-11-10。前者 true 要求支持且启用；后者 setter 在部分设备/窗口模式可能不生效且不报错。因此必须区分请求、实际状态及可见帧。

后续开发原则：先用本文件及原证据定位第一缺口，再补语义真实的窄模块；side table 按 VM/对象区分，VM 销毁前清理；不覆盖原 DEX 字段或 native payload。宿主渲染/线程/资源下载必须独立实证，不做大批无依据的空 API。
