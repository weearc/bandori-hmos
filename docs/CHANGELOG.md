# 变更记录

本文件记录对外可见的变更。日期为本地时间（Asia/Shanghai）。

## 2026-11-19

### 构建：native-probe 端到端可用，并修掉一批「干净克隆编不过」的问题

以下问题都是在本机按 README 从零走一遍时才暴露的，逐一修掉：

- **新增 `tools/gen_payload_manifest.py`**：从用户的 APK 生成 `apk_payload_manifest.h`（版本、大小、采样身份、
  每条 payload 的 size/CRC32、DEX 摘要）。用它对本机 APK 生成的结果与仓库先前那份**逐字节一致**（3760 条）。
- **新增 `tools/apk_metadata.py`**：APK 二进制清单版本读取 + 采样 SHA-256 身份。
- `build_tls_candidate.py`：基线二进制改为可缺省（不再强制要求 `preserved-template/libsdktrace.so`）；
  输出目录改为递归创建；补齐 GC/触摸/观测相关源文件的复制与编译单元；链接补 `-lffrt.z`。
- `unity_player_probe.c`：引入 `host_callbacks_stub.inc`，为 `sdk_app_vm_is_foreground` / `sdk_app_vm_should_destroy`
  提供弱符号默认实现（宿主可用强符号覆盖）。
- `jni_gc_selftest.inc`：补 `<sys/stat.h>`、`<unistd.h>`（`fchmod` 在干净构建里没有隐式声明）。
- `touch_record.inc`：为 `ReplayNow` / `ReplayPoll` / replay 状态提供**无回放实现**的等价物，
  只有定义 `BG_ENABLE_TOUCH_REPLAY` 的诊断构建才链接真正的注入实现。
- `build_native.ps1`：产物同时复制到 `native-probe/` 与 `host/`，并调用 manifest 生成与素材自检。
- **签名材料不再入库**：`native-probe/build-profile.json5` 与 `host/build-profile.json5` 的 `signingConfigs` 清空；
  `.gitignore` 增加生成物与构建产物规则。
- **构建产物停止跟踪**：`sdk-init-next/vendor/`（456 个文件）取消跟踪，`*.hap`、`build/`、`.cxx/`、`.hvigor/` 全部忽略。
- `tools/verify_no_embedded_assets.py` 重写判定：清单里的路径字符串不再算违规，改为
  「64 字节高熵切片逐字节命中」或「PNG/UnityFS/ACB 容器魔数」才是违规；低熵（补零）切片被显式过滤。

验证结果（本机，DevEco 6.0.2.130）：

```text
libsdktrace.so            6.30 MB   素材自检 exit 0
entry-default-unsigned.hap  393 MB   41 项，module.json bundleName=top.weearc.bangdream.nativeprobe
HAP 内 libsdktrace.so              素材自检 exit 0
```



### 合规：把 APK 派生内容从仓库二进制里清掉

- **移除两个预编译运行时**：`host/entry/libs/arm64-v8a/libsdktrace.so` 与
  `sdk-runtime-spike/preserved-template/libsdktrace.so`。两份库的 `assets` 表各含 81 个来自 APK 的素材条目
  （合计 663,006 字节，含 `SplashImage.png`、`JigsawData.json`、`blive/images/*`、`bin/Data/boot.config`）。
  仓库不再分发预编译运行时，改由使用者本地构建。
- **移除两份 APK 派生资源表**：`sdk-init-next/apk_resource_table.inc` 与
  `sdk-runtime-spike/preserved-template/source/apk_resource_table.inc`（来自原 APK `resources.arsc`，
  3072 个资源 ID、192 个颜色）。改为构建期用 `resource-table-next/gen_apk_resource_table.py` 生成；
  未生成时 `platform_runtime.c` 通过 `__has_include` 回退到空表，构建不会失败。
- **素材生成器默认不嵌入**：`asset-reader-next/generate.py` 的默认行为改为「只写清单、不写字节数组」，
  原先控制嵌入的开关翻转为显式 `--embed`（仅诊断用）。
- **Unity 引导配置正文改为显式开关**：`build_tls_candidate.py` 里 `boot.config` 的嵌入改为需要
  `BG_EMBED_BOOT_CONFIG=1`，默认不嵌入；开启时脚本会打印「不要分发此构建」的警告。
- **新增自检工具**：`tools/verify_no_embedded_assets.py` 扫描 ELF 里的素材路径、生成表标记与清单摘要，
  命中即返回非 0；构建脚本与 CI 都用它做门禁。
- **CI 修复并加门禁**：`.github/workflows/ci.yml` 此前 YAML 已损坏（一个 step 的正则字面量把下一个 step
  的名字吞了进去，另有游离片段）。重写后新增三项检查：拒绝提交派生资源表/预编译运行时、对任何提交的 `*.so`
  执行素材扫描、校验生成器默认不嵌入。
- **文档同步**：`LEGAL.md` 删掉了「不含派生资源表」这一不准确表述，改为逐项说明「本地生成 + 自检」；
  `BUILD.md` 不再给出跨环境固定哈希，改为「记录你自己的构建哈希」。

### 已知缺口：逐帧释放模块未接入公开构建

- `sdk-runtime-spike/overlay/leak_release_candidate.*` 是**未接线**的模块：公开树里没有
  `lr_register` / `lr_poll` 的调用点，构建脚本也不编译它。它记录的是这条路径的实现与接口，
  不是「克隆即可复现 0.2 KiB/s」的保证；
- 真机上那组数据（主页 35.199 → 0.2 KiB/s）来自接线后的诊断构建。要让公开构建拿到同样的回收效果，
  需要把 `lr_register`/`lr_returned`/`lr_poll` 接到 proxy 分派与帧节拍上，并把 `overlay/` 加入 include 路径；
- 触摸录制（`touch_record.inc`）与载荷缓存（`apk_cache_runtime.inc`、`payload_boot_reuse.inc`）已经随源码发布，
  但它们挂在 `native-probe/` 这个宿主入口上，`host/` 里没有对应实现。

### 构建：收敛到唯一宿主入口

- README 的目录表把 `native-probe/` 标为**正式宿主入口**（真机验证用的就是它），`host/` 标注为早期工程、
  保留供对照；两者的 `probe.cpp` 与 CMake 入口是独立副本，之前照 README 打包会漏掉新实现。

## 2026-11-18

### 新增：对象目录 / 世代 / 指标 / 准入契约

- `sdk-init-next/gc_directory.inc`、`gc_directory.h`：双向 ID 目录。内部引用为 64 位
  `[32 位世代票据 | 32 位槽号]`，正向槽位表与反向 `body` 地址互校，冲突即拒绝。
- `sdk-init-next/gc_reverse.inc`：反向映射（65,536 桶链式哈希），支撑引用回收与身份校验。
- `sdk-init-next/gc_generation.c`、`gc_generation.h`、`gc_generation_publish.inc`：世代发行与发布顺序，
  保证「槽复用后旧引用必然失效」。
- `sdk-init-next/gc_metrics.h`、`gc_metrics.inc`：内存指标——live charge、committed、metadata、
  分配与复用计数，供设备端读取。
- `sdk-init-next/gc_contract.inc`、`gc_operation.h`、`gc_scope_generation.inc`：准入契约与作用域借用
  （pin / borrow / monitor），把「裸指针跨分配点使用」收敛为可判定的失败而不是悬空访问。

### 新增：逐帧临时对象释放模块

- `sdk-runtime-spike/overlay/leak_release_candidate.h`、`.inc`：对 proxy 分派每帧产生的
  `Object[1]` 与装箱 `Long` 在返回后回收。**公开版本不做金丝雀隔离**（模块头部写明 `No quarantine`，
  直接归还分配器）；带 1/8 抽样隔离与 `0xDD` 填充的是本地诊断变体，未随仓库发布。
  文中 35.199 → 0.2 KiB/s 这组数据来自含诊断观测的构建，公开版本的行为与之同源但观测代码不同。
- 开关文件 `leak-release-control`（`1` 开 / `0` 关，重启生效）。
- 效果：同一构建下主页静置由 35.199 KiB/s 降到 0.2 KiB/s；关闭后回到 40.6 KiB/s。
  300 秒窗口释放 16,953 对 / 3,036,400 字节。

### 修复：诊断导出持锁写盘导致的帧停顿

- 采样与导出原先在持锁状态下做 CSV 格式化与写盘，实测持锁 p99 41.9 ms、帧等待该锁 p99 36.7 ms。
- 改为「持锁只做内存快照，格式化 / write / fsync 全部在锁外」，帧路径不再等待导出锁。
- 修复后持锁 p99 0.53 ms（约 80 倍改善），游玩中 >33 ms 帧占比 1.0–1.1%，
  开启释放时不再比关闭时更卡（修复前为 关 8 次 / 开 15 次，修复后为 关 13 次 / 开 10 次）。

### 修复：启动期重复复制 guest 载荷

- `native-probe/entry/src/main/cpp/apk_cache_runtime.inc`：清单命中且目标 `.sha256` 侧车与内容摘要
  全部一致时跳过复制；否则回退到原 `.part → 写入 → fsync → rename` 路径。
- 每次启动节省 18.1 MB 写入（三个 DEX 共 18,120,212 字节）。

### 新增：触摸录制与重放（诊断）

- `native-probe/entry/src/main/cpp/touch_record.inc`、`touch_record_core.h`、`touch_record_format.h`：
  环形缓冲 + 后台线程批量写入，行格式抽成独立头文件以保证与旧数据逐字节一致；
  刷盘由「每批次 fsync」改为按字节阈值 / 200 ms，降低录制对帧的影响。
- `touch_replay_host.inc`、`tools/touch-replay-os.c`：回放宿主侧支持。

### 新增：观测与诊断模块

- `native-elf-loader-next/gc_observ_core.c`、`gc_observ_core.h`、`gc_observ_export.inc`：复用事件观测。
- `native-elf-loader-next/touch_worker.inc`、`touch_drain_budget.inc`：触摸工作线程与排空预算。
- `cri-audio-adaptation/aaudio_diagnostics.inc`：音频路径诊断。

## 2026-11-15

### 修复：宿主分辨率不再被写死（可移植性）

- `sdk-init-next/reflection_runtime.c`：自检里把 `DisplayMetrics` 断言成 `widthPixels==2560` / `heightPixels==1600`，
  而 `JG_CHECK` 不满足即 `goto done`，任何非 2560x1600 的设备都会中断自检、应用起不来。
  改为有效性检查（`>0`）；宿主快照尚未注入（宽高为 0）时记 `SKIP` 并附原因，不中断；
  日志改用真实的 `sdk_display_source()`，不再硬编码 `source=host_snapshot`。
- `host/entry/libs/arm64-v8a/libsdktrace.so` 已用修复后源码重建（`sha256=4debbe8f…`），
  二进制内已不含 `*_2560` 字符串。

### 修复：干净克隆无法构建

- 补齐 `sdk-runtime-spike/preserved-template/libsdktrace.so`（基线与校验用二进制）；
- 补齐 `host/entry/libs/arm64-v8a/` 下的 14 个运行时与垫片（均本项目产物，非游戏内容）；
- `tools/build_integrated_login.py` / `sdk-runtime-spike/build_integrated_login.py` /
  `build_tls_candidate.py` 在缺少 `assets.inc` 时会用 `BG_APK` 或仓库根目录的 `*.apk` 本地生成；
- 验证：`git clone` → `hvigorw assembleHap` → `BUILD SUCCESSFUL`，产出 12.78 MB `entry-default-unsigned.hap`。

### 修复：热路径同步文件 IO

- `asset-reader-next/asset_reader.c:asset_probe_log()` 原本每次调用 `fopen`/`vfprintf`/`fclose`；
  改为默认关闭（仅当 `files/sdkdex/engine-probe-observe` 含 `1` 时启用），启用时流只打开一次，
  `asset_reader_vm_end()` 统一 `fflush`+`fclose`。

### 合规整理

- 从仓库移除 `asset-reader-next/assets.inc`（2.43 MB，APK 派生资源表）与 `preserved-template/source/assets.inc`；
  `generate.py` 重写为 `--apk` / `--no-embed`，生成物被 `.gitignore` 排除；
- 移除 `tls-java-next/analysis/SSLUtils.smali.txt`（DEX 反汇编）与 7 个 `.orig-*` 临时文件；
- 构建不再依赖 smali：改为 `docs/TLS-JAVA-INTERFACE.md` 记录签名与可观察行为；
- `LICENSE` 还原为标准 AGPL-3.0 全文，项目说明移入 `README.md`；
- `docs/LEGAL.md` 补入 Project Lunaria（`MPL-2.0`）并说明 MPL/AGPL 的组合条件；
- Git 历史重建为干净提交序列，历史中不残留上述文件。

### 新增

- `.github/workflows/ci.yml`：拒绝游戏资源 / 反编译材料 / 签名材料入库，Python 语法检查，LICENSE 与第三方声明检查；
- `docs/COMPATIBILITY.md`：SDK 版本、4KB 页约束、宿主注入的分辨率/密度、APK 版本绑定；
- `docs/REVIEW-RESPONSE.md`：对外部代码审查意见的逐条处理；
- `docs/TLS-JAVA-INTERFACE.md`：TLS 相关 Java 接口签名与行为；
- `tools/check_payload_manifest.py`：校验 `apk_payload_manifest.h` 是否与你的 APK 匹配。

### 运行时能力

- `sdk-init-next/settings_compat.inc`：新增 `android/provider/Settings` / `$Secure` / `$System`；
  `getInt/getLong/getFloat/getString/getUriFor` 带默认值语义，`putInt/putString` 写入 VM 内存覆盖表（不落盘）；
  `android/location/LocationManager` 最小桩。修复了 `UnityPlayer.nativeRender()` 每帧抛 `NoClassDefFoundError` 的问题
  （修复前首页最长帧 1164.8 ms，修复后 p99 23.61 ms、>33 ms 仅 4/3593 帧）。
- `application-lifecycle-next/activity_runtime.c`：放宽 `Activity` 构造守卫，`NetworkConnectivity.<init>` 得以成功；
  联机可达性 `tryGetNetworkConnectivity=2`、`registered=1`、`reason=ok`，可进入房间界面。
- `native-elf-loader-next/input_pipeline_observer.inc`、`enqueue_drain_observer.inc`、`multitouch_runtime.inc`：
  触摸链路分层计数（有界、默认关闭、生产者不做文件 IO）；实测 20,899 次触摸 0 丢失。
- `sdk-init-next/play_stall.inc`、`perf_attr.*`、`choreographer_runtime.inc`：长帧分解（GIL/GC/EGL/诊断/IO/未归因）。

## 未完成

- 断触/跳帧手感验收（需要真实演奏窗口）；
- 标题界面约 30fps；
- 极端长帧（>1 s）仍有未归因残余；
- `apk_payload_manifest.h` 目前只有校验脚本，没有自动再生成器；
- `dvm.c` / `jni_bridge.c` 体积过大，待拆分。
