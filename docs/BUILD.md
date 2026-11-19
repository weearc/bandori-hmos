# 构建手册（从零到可安装的 HAP）

本文件的每条命令都在仓库的干净状态下实际执行过（DevEco Studio 6.0.2.130 / hvigor 6.26.9）。
所有命令以仓库根目录为当前目录（Windows / PowerShell）。

## 0. 前置检查

```powershell
# HarmonyOS SDK 自带的 clang
Test-Path 'C:/Program Files/Huawei/DevEco Studio/sdk/default/openharmony/native/llvm/bin/clang.exe'
# Python 3.10+
python --version
# 设备（安装阶段才需要）
hdc list targets
```

SDK 不在默认路径时，改 `tools/build_native.ps1` 顶部的 `$SDK`，以及
`sdk-init-next/build_pcre2.py`、`tls-java-next/native/build_native.py`、
`sdk-runtime-spike/build_tls_candidate.py` 顶部同样的常量。

## 0. 先选对宿主工程

仓库里有**两个宿主工程**，构建步骤不同：

| 目录 | 定位 | 说明 |
|---|---|---|
| `native-probe/` | **正式入口** | 真机验证、触摸录制、载荷缓存、全部新实现都在这里 |
| `host/` | 早期工程 | 保留供对照；`probe.cpp` 与 CMake 是独立副本，不含新实现 |

下面的步骤以 `native-probe/` 为准；把命令里的 `native-probe` 换成 `host` 只能得到旧版宿主。

## 1. 取游戏素材（需要你自己的 APK）

```powershell
python tools/prepare_from_apk.py --apk D:\path\to\your.apk
```

| 目标路径 | 来源 |
|---|---|
| `host/entry/src/main/resources/rawfile/sdkdex/base.apk` | 你的 APK 原样复制 |
| `host/entry/src/main/resources/rawfile/sdkdex/classes*.dex` | APK 内同名条目 |
| `host/entry/libs/arm64-v8a/` 下 8 个游戏 so | APK 的 `lib/arm64-v8a/` |

> 这一步同时会把两个 `app_icon.png` 换成你 APK 里的图标（`--no-icon` 可跳过）。
> 这一步不做也能构建 unsigned HAP（只是装到设备后无法真正运行游戏）。

## 2. 编译依赖与运行时

```powershell
powershell -ExecutionPolicy Bypass -File tools\build_native.ps1
```

它依次做四件事：

| 阶段 | 命令 | 产物 |
|---|---|---|
| PCRE2（UTF-16） | `python sdk-init-next/build_pcre2.py` | `sdk-init-next/vendor/build-pcre2-16/libpcre2-16.a` |
| mbedTLS 3.6.7 | `python tls-java-next/native/build_native.py` | `tls-java-next/native/build/libmbed*.a` |
| SQLite 适配层 | 见脚本（编 `sqlite3.o` / `game_sqlite.o`，再链 `.so`） | `sqlite-backend-next/build/libgame_sqlite.so` |
| 组装运行时 | `python sdk-runtime-spike/build_tls_candidate.py` | `sdk-runtime-spike/build/local-build/libsdktrace.so` |

最后把 `libsdktrace.so` 复制到 `host/entry/libs/arm64-v8a/` 并打印 sha256。

仓库**不再提供预编译的 `libsdktrace.so`**：两次不同环境的构建产物并不保证一致，且旧的预编译库曾内嵌游戏素材
（已移除）。请以你自己构建出的哈希为准，并把它记在你的发布说明里：

```text
# 构建后自行记录，示例格式
libsdktrace.so  <大小>  sha256=<你的构建哈希>
```

构建完请跑一次合规自检（有内嵌素材会返回非 0）：

```powershell
python tools/verify_no_embedded_assets.py host\entry\libs\arm64-v8a\libsdktrace.so
```

## 3. 构建 unsigned HAP（已验证）

命令行方式需要 java 在 PATH（DevEco 自带 JBR）：

```powershell
$de = 'C:\Program Files\Huawei\DevEco Studio'
$env:DEVECO_SDK_HOME = "$de\sdk"
$env:JAVA_HOME      = "$de\jbr"
$env:PATH           = "$de\jbr\bin;$de\tools\node;$de\tools\hvigor\bin;" + $env:PATH

cd host
hvigorw assembleHap --mode module -p product=default
```

```text
> hvigor Finished :entry:default@PackageHap...
> hvigor BUILD SUCCESSFUL
entry-default-unsigned.hap   12.6 MB
WARN: No signingConfig found for product default   <- 预期，未配置签名
```

也可以直接用 DevEco Studio 打开 `host/` 后点 Build → Build Hap(s)。

常见失败：

| 报错 | 原因 | 处理 |
|---|---|---|
| `spawn java ENOENT` | PATH 里没有 java | 按上面把 `%DEVECO%\jbr\bin` 加进 PATH |
| `hvigorw` 不存在 | 只用 DevEco 图形界面时 | 用 SDK 自带的 `tools\hvigor\bin\hvigorw.bat` |

## 4. 签名（要装设备才需要）

1. DevEco → File → Project Structure → Signing Configs → 勾选 Automatically generate signature；
2. 或用自己的 `.p12/.cer/.p7b`（DevEco Sign 面板 / `hap-sign-tool`）。

本仓库 `build-profile.json5` 的 `signingConfigs` 为空，避免带入任何人的证书。

## 5. 安装与首次启动

```powershell
hdc install -r host\entry\build\default\outputs\default\entry-default-signed.hap
hdc shell aa start -a EntryAbility -b <bundleName>
hdc shell hilog | Select-String -Pattern 'E-|FATAL|mono_ns'
```

首次启动会解包 `base.apk`（约 30–60 秒），之后约 10 秒。

## 6. 验证清单

```powershell
$D = '/data/app/el2/100/base/<bundleName>/haps/entry/files/sdkdex'
hdc shell "ls -l $D | head -20"
hdc file recv "$D/diagnostic-log-status.txt" .
hdc file recv "$D/connectivity-status.txt" .
```

- 能到标题页 → `mono_ns` 应该开始递增
- 进首页 → 若开启采样，检查 `play-stall-frames.csv` 是否出现大于 33ms 的帧
- 任何 `E-` 开头的日志都先处理（`E-GC1-SELFTEST-FAIL`、`E-ELF-PAGE-CONGRUENCE` 等）

## 7. 图标

`host/entry/src/main/resources/base/media/app_icon.png` 与 `host/AppScope/resources/base/media/app_icon.png`
是仓库自带的占位图（自制几何图形）。`tools/prepare_from_apk.py` 会自动把它们替换成你 APK 里的图标；
提交前用 `git checkout -- host/**/app_icon.png` 还原，**不要把游戏图标提交回仓库**。
也可以手动替换成任意 512×512 PNG。

## 0. 前置：本地生成资源表（不入库）

`asset-reader-next/assets.inc` 由你的 APK 派生，仓库不包含它：

```powershell
python asset-reader-next/generate.py --apk D:\path\to\your.apk
# 或
$env:BG_APK='D:\path\to\your.apk'
```

若文件缺失，`sdk-runtime-spike/build_tls_candidate.py` 会尝试用 `BG_APK` 或仓库根目录第一个 `*.apk` 自动生成并给出明确报错。

### 关于 `apk_resource_table.inc`

该文件同样由 APK 派生（原 `resources.arsc`，3072 个资源 ID、192 个颜色），**不再入库**：

```powershell
python resource-table-next/gen_apk_resource_table.py --apk D:\path\to\your.apk --out sdk-init-next/apk_resource_table.inc
```

没生成也能编译：`platform_runtime.c` 用 `__has_include` 回退到空表，只是 `getIdentifier()` 之类的查询会落空。

### 关于 `sdk-runtime-spike/preserved-template/`

该目录保留**源码级**基线（组装脚本需要的模板文件），但**不再随仓库提供** `libsdktrace.so`：

- 旧基线库内嵌了 81 个来自 APK 的素材条目（合计 663,006 字节），已从仓库移除；
- 需要基线时请用 `sdk-runtime-spike/build_tls_candidate.py` 自己构建，产物落在
  `sdk-runtime-spike/build/local-build/libsdktrace.so`；
- 与旧基线的差异（符号、行为）请以源码为准，不再用二进制哈希做跨环境比较。

### 换 APK 版本

`host/entry/src/main/cpp/apk_payload_manifest.h` 与具体 APK 绑定（size/CRC/每条资产）。换版本前先校验：

```powershell
python tools/check_payload_manifest.py --apk D:\path\to\your.apk
```

返回 MISMATCH 就必须为新 APK 重新生成该头文件，否则提取阶段会校验失败。

## 仓库随附的运行时二进制

`host/entry/libs/arm64-v8a/` 内保留**本项目自己代码的垫片与依赖**（AGPL-3.0，共 13 个），随仓库提供，
让「干净克隆 → 构建」不必先手工准备这些垫片；**运行时本体 `libsdktrace.so` 已从仓库移除**，必须自己构建：

```text
libsdktrace.so              运行时（DEX 解释器/JNI/ELF/网络/TLS/SQLite 等）
libgame_sqlite.so           SQLite 适配
libgame_aaudio_bridge.so    音频桥
libjnibatch.so / libunitybatch.so     批处理辅助
libandroid_compat.so / liblog.so      兼容垫片
libprobe_android_guest.so / libprobe_android_libc.so   guest 侧垫片
libnested_caller.so / libnested_leaf.so / libnested_skipped.so  dlopen 嵌套测试件
libunity_spike_loader.so / libspike_original_main.so   早期 spike 件
```

它们**不含游戏代码**，也不含任何游戏资源；游戏侧 `.so`（`libil2cpp.so` / `libunity.so` / `libmain.so` /
`libcri_ware_unity.so` / `libcrypto_1_1.so` / `libssl_1_1.so` / `libblkv.so` / `libcustom_rules.so`）
在 `.gitignore` 中显式排除，必须由使用者从自己的 APK 提供。

需要自行重建运行时：见 `sdk-runtime-spike/build.ps1`、`tools/build_tls_candidate.py`、
`sqlite-backend-next/build_*.py`，把产物覆盖到 `host/entry/libs/arm64-v8a/` 即可。
