# CRI 音频兼容接口规格（候选需求，非实现）

版本：2026-10-31  
目标：为 Bang Dream 国服 APK（Unity 2022.3.62f3c1，arm64）在 HarmonyOS 7 的原生音频兼容层提供可验证需求。本文不声称已实现 OpenSL ES/AAudio，也不把静态符号字符串等同于运行时调用。

## 已有证据与边界

- 本地 `native-requirements.json` 记录 `libcri_ware_unity.so` 含 59 个 AAudio API 名称字符串；CRI 版本字符串为 Atom Android_ARMv8A 2.29.310（Jun 12 2025）、Unity 插件 1.04.15。另有回退字符串 `AAudio is not supported,use OpenSLES.`（任务给定的二进制检查结果）。这些证据支持“库中存在 AAudio 路径/回退逻辑”，不证明 APK 初始化时启用该选项，也不证明设备运行时成功打开 AAudio。
- CRI 官方 Unity 手册将 `androidUsesAAudio` 标为 Beta，Android 8+可用，默认 false。应优先确认实际初始化配置，并在目标设备验证流建立和回退行为。见 [CRI Atom 配置](https://game.criware.jp/manual/unity_plugin_en/latest/contents/classCriWare_1_1CriAtomConfig.html) 与 [CRI 初始化器手册](https://www.criware.cn/sdk_manual/chinese/contents/cri4u_component_initializer.html)。
- `libcri_ware_unity.so` 的 DT_NEEDED 含 OpenSL ES（以本地分析记录为准）。即使 CRI 选择 AAudio，动态装载器仍可能要求该依赖可解析；实际装载时机/强弱依赖须在目标运行环境核实。Unity `libunity.so` 自己也有平台音频原生依赖风险，不能由 CRI 走 AAudio 推导其不再需要 OpenSL ES。
- `candidate-symbols.json` 中 59 个 AAudio 名称是字符串候选清单；其中 25 个在被比较的参考导出表中缺失。这 25 个只说明参考表未导出，**不是** 25 个全部必需，也不是全部会在该 APK 执行。先用运行时符号解析/调用跟踪和失败日志缩减真实 ABI 面。
- 华为建议游戏使用 OHAudio，并设置游戏流用途 `STREAM_USAGE_GAME`；OHAudio 原生播放为 PCM。参见[华为游戏音视频适配](https://developer.huawei.com/consumer/cn/doc/doccenter-games/games-universal-video-0000002630264564)、[OHAudio builder API](https://developer.huawei.com/consumer/en/doc/harmonyos-references-V13/native__audiostreambuilder_8h-V13)。

## 推荐实现顺序

### P0：运行证据与直接 AAudio 可行性

1. 静态核对 APK 的 CRI 配置/初始化路径，找到 `androidUsesAAudio` 实际取值来源；不要因 so 中含 API 名称就改判启用。
2. 在获授权目标设备记录：AAudio builder 创建/设参/open 结果、实际流状态、实际格式/采样率/声道、buffer size/capacity、frames-per-burst、XRun 变化、回退日志。对照 CRI 默认配置与启用 AAudio 配置。
3. 单独检查 HAP ELF 装载：CRI DT_NEEDED 的 OpenSL ES 依赖和 Unity 的原生导入分别验证。确认 `dlopen`/重定位阶段是否需要 OpenSL provider，即使 CRI 后端选 AAudio 也不能省略这项。
4. Unity 动态音频路径单独归因。若 Unity 自身调用 OpenSL ES，须以 Unity 日志/运行跟踪证明；CRI AAudio 不会自动接管 Unity 的引擎音频。

### P1：OpenSL ES 到 OHAudio 的最小兼容面（仅在目标调用证据需要时）

- 导出/动态解析：`slCreateEngine` 与 APK 实际导入的所有 `SL_IID_*` 数据符号。候选 IID 包括 ENGINE、PLAY、RECORD、VOLUME、BUFFERQUEUE、ANDROIDSIMPLEBUFFERQUEUE、ANDROIDCONFIGURATION、ANDROIDACOUSTICECHOCANCELLATION、ANDROIDAUTOMATICGAINCONTROL、ANDROIDNOISESUPPRESSION；精确名单见 JSON。
- 对象与 ABI：实现被调用的 OpenSL ES 对象句柄和 interface vtable 布局、`Realize`/`GetInterface`/生命周期接口及错误返回语义。以实际函数指针调用集合为准。不得以不透明句柄替代未经验证的对象布局；所有对象销毁后必须使其接口失效且避免回调悬挂。
- 队列与 PCM：对 Android Simple Buffer Queue 支持注册回调、enqueue、清队列/状态查询（仅实现游戏实际调用者）；正确处理缓冲区所有权、入队顺序、回调线程、销毁竞态。接受完整 PCM 描述，核实采样率、声道数、样本格式、字节序、帧/字节换算；不支持的格式明确报错或由已验证转换器处理。
- OHAudio 输出：建 renderer，配置 PCM 参数、`STREAM_USAGE_GAME`、延迟模式与回调，再生成/启动流。回调每次必须填满请求的字节长度；不得在回调中阻塞、分配内存、I/O，或调用 renderer 生命周期/控制接口。播放数据在回调返回后由服务排队，回调外不能再修改该 buffer。参考[回调 API 语义](https://developer.huawei.com/consumer/cn/doc/doccenter-references/api/capi-ohaudio-oh-audiorenderer-callbacks-struct)。
- 状态和生命周期：映射 OpenSL ES 的 realized/playing/paused/stopped/queue-empty/error 到 OHAudio 状态机；区分请求、服务确认、错误与中断，不把 API 返回成功视作声音已到达扬声器。处理焦点/中断、设备变化、前后台、暂停恢复、stop/flush/release 的幂等性和线程安全。

### P2：时钟、排队、延迟与判定

- 追踪每个 PCM 块的帧序号、提交/回调单调时钟时间、样本率、队列深度和 underrun 计数。目标采样率实际不符时明确选择/验证 ASRC；不靠重复丢样掩盖长期时钟漂移。
- 不混同 CRI 内部缓冲、OHAudio 服务缓冲、硬件缓冲。报告每层估算帧数与端到端测量，避免“配置了 FAST/AAudio 就等于低延迟”的推断。
- AAudio 与 OHAudio时钟不假定同步。必要时以时间戳/外部参考做时钟映射；触控输入、画面提交、游戏节拍时间轴与播放样本时间轴分开记录，再评估判定补偿。

## 验收用例与记录项

每项在相同设备、系统版本、APK、输出设备、音量、采样率/谱面/刷新率、温控和负载条件下重复，按设备及输出路径分组报告中位数、分位数、抖动、最大值、XRun/underrun、音频异常和失败率；不预设或承诺毫秒值/成功率。

1. 初始化：AAudio 开/关两组；记录配置、实际后端、builder/open 错误、回退与依赖装载。
2. 声音内容：连续音乐、密集短音效、同时多声部；检查 PCM 格式正确、无静音/错速/爆音，长时队列深度无持续增长。
3. 缓冲压力：逐步调整可用缓冲，观察 underrun/XRun 与音质；只在无声测试/校准阶段收敛参数，再跑实曲。
4. 生命周期：冷启动首音、暂停/恢复、前后台、锁屏/解锁、中断、耳机插拔/路由切换、stop/flush/release 和快速重启；确认无死锁、悬挂回调、重复释放。
5. 输入判定：外部同步触发/高速摄像或光电/音频采集，分测触摸到游戏响应、响应到屏幕显示、声音输出；不要仅用游戏内判定偏移反推音频延迟。
6. 基线：Android 原机默认配置、Android 原机 CRI AAudio、HarmonyOS HAP 上有效配置分开比较；报告配置与测量方法，保留原始时间戳/日志以复核。

## 已知风险与停止条件

- CRI AAudio 是 Beta 且默认关闭；HarmonyOS 的 AAudio 兼容性、运行配置和实际低延迟路径均须实测。
- 必需的 OpenSL 动态依赖可能在应用初始化之前触发装载失败。需以 loader 证据确认；不要假设 AAudio 可绕过 DT_NEEDED。
- 替代库可能满足解析却不满足 ABI/行为契约；vtable/IID错误、回调并发、buffer 所有权或生命周期错配会造成崩溃、静音、破音或判定漂移。
- OHAudio 回调缓冲属于服务排队数据，回调返回后不得修改；回调必须填满长度。低时延请求可能因设备/格式/资源不可用而退回普通模式，应记录实际状态。
- 若 Unity 自身 OpenSL ES 调用未被覆盖、无可验证回退，或原生依赖无法装载，应停止声称桥接可用，先补齐 ABI 证据或改走官方支持的引擎适配方案。

## 参考资料

- [CRI Atom 配置 API](https://game.criware.jp/manual/unity_plugin_en/latest/contents/classCriWare_1_1CriAtomConfig.html)
- [CRI Unity 插件初始化器说明](https://www.criware.cn/sdk_manual/chinese/contents/cri4u_component_initializer.html)
- [华为 HarmonyOS 游戏音视频适配建议](https://developer.huawei.com/consumer/cn/doc/doccenter-games/games-universal-video-0000002630264564)
- [OHAudio stream builder API](https://developer.huawei.com/consumer/en/doc/harmonyos-references-V13/native__audiostreambuilder_8h-V13)
- [OHAudio renderer callbacks](https://developer.huawei.com/consumer/cn/doc/doccenter-references/api/capi-ohaudio-oh-audiorenderer-callbacks-struct)
- [Android NDK AAudio latency and buffer tuning](https://developer.android.com/ndk/guides/audio/aaudio/aaudio)
- [Android NDK OpenSL ES notes (deprecated)](https://developer.android.com/ndk/guides/audio/opensl/opensl-prog-notes)
- [Android audio latency measurement caveats](https://developer.android.com/ndk/guides/audio/audio-latency)

## 新增本地 SDK 检视：系统 OpenSL ES 可作为候选后端

对本机 DevEco Studio API 26 sysroot 的只读检查发现：

- `.../usr/lib/aarch64-linux-ohos/libOpenSLES.so` 大小 7,376 字节，导出 50 个符号。导出中包含 `slCreateEngine`、`SL_IID_ENGINE`、`SL_IID_PLAY`、`SL_IID_RECORD`、`SL_IID_VOLUME` 和 `SL_IID_BUFFERQUEUE`。这使“优先尝试系统库 + 极小适配 shim”比自行实现完整 SLES 更合理。
- 同一 include/SLES 提供 `OpenSLES.h`、`OpenSLES_Platform.h`、`OpenSLES_OpenHarmony.h`；后者定义 `SL_IID_OH_BUFFERQUEUE` / `SLOHBufferQueueItf`。系统 SLES 的 50 个导出不包含 APK 导入的 Android 扩展 IID（如 `SL_IID_ANDROIDSIMPLEBUFFERQUEUE`、`SL_IID_ANDROIDCONFIGURATION`、Android 音效接口 IID）。完整清单见 `candidate-symbols.json`。
- **不能**由导出名匹配推断 Android NDK `libOpenSLES.so` 的二进制或行为兼容。系统库需要 shim 将 Android IID/接口调用适配到已有 OpenHarmony API；Android 扩展行为可能没有等价物。SLES 头文件提供声明也不表示 Android 扩展兼容。

### Buffer Queue 差异与小 shim 候选

标准 `SL_IID_BUFFERQUEUE` 由 OpenHarmony SLES 头声明，`SLBufferQueueItf` 提供 Enqueue/Clear/GetState/RegisterCallback。Android 特有的 `SL_IID_ANDROIDSIMPLEBUFFERQUEUE` 不在 SDK 导出表中。`OpenSLES_OpenHarmony.h` 中的 OH 队列使用 `SL_IID_OH_BUFFERQUEUE`，提供 Enqueue/Clear/GetState/GetBuffer/RegisterCallback；其回调原型含 `size` 参数（`SlOHBufferQueueCallback(caller, context, size)`）。Android Simple Buffer Queue 回调 ABI/参数语义不同，不能直接把回调函数指针透传。shim 必须提供独立适配 trampoline，维护回调上下文/当前缓冲信息，并在调用侧确认预期签名、队列计数/索引语义、缓冲所有权和回调线程。

建议最小原型仅做接口创建与查询、标准 PCM 队列入队、callback 转发和销毁，不处理压缩格式/录音/音效扩展。对 `SL_IID_ANDROIDCONFIGURATION` 及 AEC/AGC/NS IID：只有运行时追踪证明实际 GetInterface/配置使用后，才实现兼容或定义可接受的无效果降级；不可静默伪报“已启用”。若动态装载器要求这些数据符号先存在，可在 shim 导出正确数据符号地址，但必须让后续 `GetInterface` 返回清晰的 unsupported 结果，不能把任意 GUID 当成功接口。

### 无 APK/声音的最小验证步骤

1. 用一个独立 native 探针链接 sysroot `libOpenSLES.so`，只执行 `slCreateEngine`、对象 Realize、引擎 IID 查询、创建 PCM AudioPlayer、请求标准 BufferQueue 和 Play/Volume IID、对象销毁；不入队、不启动播放、不访问 APK。
2. 追加 shim 后验证 ELF 符号解析和 `GetInterface` 映射：先匹配标准 IID，再逐个测 Android 扩展 IID 的明确返回值；核对 AArch64 struct 布局、函数指针槽序与错误码。
3. 在不发声的隔离测试中只验证队列缓冲入队/回调调度和 clear/stop/destroy 竞态；若回调必须经真实音频驱动才会触发，则将其标记为设备端播放测试前置，不以静态编译通过替代行为验证。
4. 最后才做获授权 APK 装载/真实声音与低延迟测试。确认 loader 是否仍拒绝 DT_NEEDED、Unity 是否单独触发 OpenSL、CRI 的 AAudio 开关与 fallback；这些均不由 SDK exports 预检证明。

系统库可降低 shim 工作量，但既不证明 APK 可以直接加载/调用，也不证明 OpenSL 路径或其 OHAudio 后端达到低延迟。优先顺序仍为：验证 CRI AAudio → 验证系统 SLES 标准接口 → 为确实调用的 Android 扩展做小 shim → 才考虑完整替代实现。
