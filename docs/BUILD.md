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

验证结果（两次独立构建一致）：

```text
libsdktrace.so  6.8 MB  sha256=16825da15eec0c49a5d2b6b74e380d853614b99bcf716710202a0308794ef578
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

### 关于 `preserved-template/libsdktrace.so`

该文件是**我们自己代码冻结出的基线二进制**（AGPL-3.0），随仓库提供，用于校验重新组装出的运行时；
它不是游戏内容，也不包含游戏代码。若你想从头重做基线，可用 `sdk-runtime-spike/build.ps1` 编译后替换，
但校验摘要会随之改变。

### 换 APK 版本

`host/entry/src/main/cpp/apk_payload_manifest.h` 与具体 APK 绑定（size/CRC/每条资产）。换版本前先校验：

```powershell
python tools/check_payload_manifest.py --apk D:\path\to\your.apk
```

返回 MISMATCH 就必须为新 APK 重新生成该头文件，否则提取阶段会校验失败。

## 仓库随附的运行时二进制

`host/entry/libs/arm64-v8a/` 内的 14 个 `.so` 是**本项目自己代码的构建产物**（AGPL-3.0），随仓库提供，
以保证「干净克隆 → 直接可构建出可运行的 HAP」：

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
