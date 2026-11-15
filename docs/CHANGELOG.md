# 变更记录

本文件记录对外可见的变更。日期为本地时间（Asia/Shanghai）。

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
