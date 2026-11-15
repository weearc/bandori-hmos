# bandori-hmos

在 HarmonyOS NEXT 上运行 Android/Unity 游戏（IL2CPP）的实验性兼容层。
本项目**不含游戏本体与资源**：你需要自备正版 APK 才能构建出可运行的应用。

作者：**weearc** · 许可证：**AGPL-3.0**（见 [LICENSE](LICENSE)）

> 本项目原创代码以 **AGPL-3.0** 发布；其中继承自 **Project Lunaria** 的 DEX 解释器文件仍为 **MPL-2.0**。
> 第三方组件与合规说明见 [docs/LEGAL.md](docs/LEGAL.md)，环境与版本矩阵见 [docs/COMPATIBILITY.md](docs/COMPATIBILITY.md)。

## 构建前必须知道的三件事

1. **你需要自备正版 APK**。仓库不含 APK/DEX/游戏 SO，也不含由 APK 派生的资源表：
   `asset-reader-next/assets.inc` 由你本地生成（已被 `.gitignore` 排除）：
   ```powershell
   python asset-reader-next/generate.py --apk D:\path\to\your.apk   # 或设置 BG_APK
   ```
2. **基线库随仓库提供**：`sdk-runtime-spike/preserved-template/libsdktrace.so` 是我们自己代码冻结出的基线二进制（AGPL），
   用于校验重新组装的运行时；干净克隆后直接可用，无需先构建它。
3. **分辨率与密度不写死**：由宿主 `@ohos.display` 注入；4KB 页只是部分 guest 库的几何约束，见 COMPATIBILITY。


---

## 它是什么

Android 游戏跑在 HarmonyOS 上，缺的不止是一个 VM。本工程按层补齐：

```text
ArkTS 宿主 (host/)
  EntryAbility / Index.ets : 窗口、EGL/NativeWindow、音频、触摸、输入法、宿主 RPC
  cpp/probe.cpp            : NAPI 侧宿主能力（EGL、音频、网络快照、权限）
  cpp/payload.cpp          : 以 C ABI 暴露给运行时的宿主接口
        |  dlopen + dlsym
libsdktrace.so（我们自己的运行时，源码在各 *-next 模块）
  dvm.c / dex.c            : DEX 解释器（类加载、解释执行、GC、异常）
  jni_bridge.c             : JNI 桥与对象模型
  elf_loader.c             : guest ELF 加载（段映射、重定位、GOT、init_array）
  unity_player_probe.c     : Unity 播放器与生命周期驱动（GIL、帧节拍）
  ssl_runtime.c / tls_client.c : Java TLS 与 mbedTLS 传输
  sqlite_runtime.c         : SQLite 适配
  network_runtime.c        : ConnectivityManager / 网络状态桥
  diagnostic_log.c         : 有界异步日志（worker 独占 IO）
        |
guest：classes*.dex + libil2cpp.so + libunity.so + 游戏自带 so（用户自备）
```

更细的说明见 [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)。

## 图标与素材政策

- 仓库里的应用图标是**自制占位图**（`BHMOS / compat layer` 渐变方块），不使用任何来自游戏 APK 的位图；
- 游戏图标、启动图、UI 贴图等都不随仓库发布；它们属于游戏本体，由使用者自己的 APK 提供；
- `tools/prepare_from_apk.py` 会在本地把占位图**替换**成你 APK 里的图标（不改仓库、不联网）；
  该替换只发生在你的工作区，提交前用 `git checkout -- host/**/app_icon.png` 还原。

## 目录

| 路径 | 说明 |
|---|---|
| `host/` | HarmonyOS 应用工程（ArkTS + NAPI），可直接用 DevEco 打开 |
| `*-next/` | 兼容层各子模块的 C 源码（按功能命名，共 57 个） |
| `sdk-runtime-spike/` | 运行时组装脚本 `build_tls_candidate.py` 与保留的基线模板 |
| `tools/` | 素材准备与构建脚本 |
| `docs/` | 架构、构建、排错、实测结论 |

## 快速开始

### 0. 环境

- DevEco Studio 6.0+（含 HarmonyOS SDK、NDK clang、hvigor、hdc、自带 JBR/JDK）
- Python 3.10+
- HarmonyOS NEXT arm64 设备（页面 4KB），开启开发者模式与 USB 调试
- 你自己的游戏 APK（IL2CPP / arm64）

> 本仓库在 DevEco Studio 6.0.2.130 / hvigor 6.26.9 上完整验证过。
> 若 SDK 路径不同，改 `tools/build_native.ps1` 顶部的 `$SDK`。

### 1. 从你的 APK 提取素材

```powershell
python tools/prepare_from_apk.py --apk D:\path\to\your.apk
```

会放入：`rawfile/sdkdex/base.apk`、`rawfile/sdkdex/classes*.dex`、`entry/libs/arm64-v8a/` 的 8 个游戏 so，
并把**应用图标替换成你 APK 里的图标**（解析 AndroidManifest 的 `android:icon` → resources.arsc → 最高密度 png）。

**这些文件被 .gitignore 排除；图标文件会被本地改写，提交前用
`git checkout -- host/**/app_icon.png` 还原占位图。** 加 `--no-icon` 可跳过图标替换。

### 2. 编译原生运行时 → libsdktrace.so

```powershell
powershell -ExecutionPolicy Bypass -File tools\build_native.ps1
```

四个阶段（脚本依次执行）：PCRE2 → mbedTLS 3.6.7 → SQLite 适配层 → 组装 `libsdktrace.so`，
最后自动复制到 `host/entry/libs/arm64-v8a/`。

### 3. 构建 HAP

用 DevEco Studio 打开 `host/`，或用命令行（需要 java 在 PATH）：

```powershell
$de = 'C:\Program Files\Huawei\DevEco Studio'
$env:DEVECO_SDK_HOME = "$de\sdk"
$env:JAVA_HOME      = "$de\jbr"
$env:PATH           = "$de\jbr\bin;$de\tools\node;$de\tools\hvigor\bin;" + $env:PATH

cd host
hvigorw assembleHap --mode module -p product=default
## 产物：host/entry/build/default/outputs/default/entry-default-unsigned.hap
```

> 已验证：本仓库从干净状态可构建出 **unsigned HAP（约 12.6 MB）**。
> 若只想确认编译链是否通，做到这一步即可，不需要签名。

签名（要装到设备时才需要）：

1. DevEco → File → Project Structure → Signing Configs → 勾选 Automatically generate signature；
2. 或用你自己的 `.p12/.cer/.p7b`，通过 DevEco 的 Sign 面板 / `hap-sign-tool` 完成。

仓库里的 `build-profile.json5` 故意把 `signingConfigs` 留空，避免把任何人的证书带进来。

### 4. 安装运行

```powershell
hdc install -r host\entry\build\default\outputs\default\entry-default-signed.hap
hdc shell aa start -a EntryAbility -b <bundleName>
```

首次启动会把 `base.apk` 解包到私有目录（约 30–60 秒），之后约 10 秒。

## 设备端诊断

私有目录 `files/sdkdex/` 下（部分为 0644，可直接 `hdc file recv`）：

| 文件 | 用途 |
|---|---|
| `diagnostic-log-status.txt` | 诊断队列状态、丢失计数 |
| `connectivity-status.txt` | 网络可达性桥的返回值与注册状态 |
| `socket-counters.csv` | socket 级计数（开关 `socket-counter-observe`） |
| `net-room-network.txt` | HTTP 元数据（已脱敏） |
| `play-stall-frames.csv` | 帧/停顿采样（可选） |
| `vm-lifetime.txt` | VM 生命周期事件 |

排错见 [docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md)。

## 当前状态（诚实版）

| 能力 | 状态 |
|---|---|
| 启动 → 标题 → 登录 → 首页 | ✅ 可用 |
| 本地单人游玩 | ✅ 可用 |
| 打歌卡顿（每 5 秒 ~2.5 秒停顿） | ✅ 已定位并修复，>33ms 帧 37 → 0（已定位并修复，详细数据不随仓库发布） |
| 内存占用 | 🟡 仍有 ~36.8 KiB/s 泄漏，已定界未根治（详细数据不随仓库发布） |
| 协力 / 匹配 | 🟡 根因已定位（本地网络可达性判定）并补齐回调桥，真机确认待完成（详细分析不随仓库发布） |
| 协力实际对局 | ❌ 未实现，也不在本项目目标内 |
| 反作弊 / 支付 | ❌ 不实现 |

## 许可证

**GNU Affero General Public License v3.0**，见 [LICENSE](LICENSE)。
Copyright (C) 2026 **weearc**。

注意 AGPL 的第 13 条：如果你把本项目的修改版作为网络服务提供，需要向使用者提供对应源码。

第三方组件保留其自身许可证（PCRE2 BSD-3-Clause、mbedTLS Apache-2.0、SQLite Public Domain），
详见 [docs/LEGAL.md](docs/LEGAL.md)。

---

## English summary

An experimental compatibility layer that runs Android/Unity (IL2CPP) games on HarmonyOS NEXT: 
a custom DEX interpreter, JNI bridge, guest ELF loader, mbedTLS-based TLS, SQLite adapter, and an 
ArkTS host providing EGL/NativeWindow, audio, touch and input.

No game assets, APKs, DEX files or game libraries are included. Bring your own APK and run 
`tools/prepare_from_apk.py` to extract the guest libraries. Build the native runtime with 
`tools/build_native.ps1`, then build the HAP with hvigor (unsigned build verified at ~12.6 MB).

Licensed under the GNU Affero General Public License v3.0. Copyright (C) 2026 weearc.
