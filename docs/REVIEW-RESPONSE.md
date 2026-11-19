# 针对代码审查意见的处理记录

本文记录对公开审查意见的逐条处理，便于复核。审查意见基于公开源码静态抽查，非法律意见。

## P0 清理嵌入资源与反编译材料

| 项 | 处理 |
|---|---|
| `asset-reader-next/assets.inc`（2.43 MB，APK 派生资源表） | **已从仓库删除**；改为本地生成，`.gitignore` 排除 |
| `sdk-runtime-spike/preserved-template/source/assets.inc` | **已删除**；构建时由同一生成器产出 |
| `tls-java-next/analysis/SSLUtils.smali.txt`（DEX 反汇编） | **已删除**；只保留接口签名与行为说明 |
| `.orig-*` / `.bak-*` 临时文件（7 个） | **已删除** |
| Git 历史 | **已重建**为单一提交，历史中不残留上述文件 |

生成方式（用户本地，不入库）：

```powershell
python asset-reader-next/generate.py --apk D:\path\to\your.apk
```

`generate.py` 已重写：支持 `--apk` / `--no-embed`，不再依赖硬编码文件名；
`build_tls_candidate.py` 与两份 `build_integrated_login.py` 在缺失时会调用它，
并按 `BG_APK` 环境变量查找 APK，找不到就给出明确报错。

## P1 可复现构建与热路径 IO

| 问题 | 处理 |
|---|---|
| 干净克隆缺 `sdk-runtime-spike/preserved-template/libsdktrace.so` | **已改为本地构建**：该二进制已于 2026-11-19 从仓库移除（其 `assets` 表内嵌 81 个 APK 素材条目）。`preserved-template/` 只保留源码级模板 |
| 干净克隆缺 `host/entry/libs/arm64-v8a/*.so`（运行时及其垫片共 14 个） | **垫片仍在仓库**（13 个，本项目 AGPL 产物）；**运行时 `libsdktrace.so` 已于 2026-11-19 移除**，改由 `tools/build_native.ps1` 本地构建并用 `tools/verify_no_embedded_assets.py` 自检 |
| `asset_reader.c` 的 `asset_probe_log()` 每次 fopen/write/fclose | **已修复**：默认关闭（仅当 `files/sdkdex/engine-probe-observe` 含 `1` 才启用）；启用时流只开一次并保持，VM 结束时 flush+close；不再在热路径做同步 IO |

## P2 维护性与合规

| 项 | 处理 |
|---|---|
| 第三方清单缺 Project Lunaria | **已补**：`docs/LEGAL.md` 增加 Lunaria（MPL-2.0）并说明 MPL/AGPL 组合条件 |
| `LICENSE` 前有项目说明、被识别为 Other | **已还原**为标准 AGPL-3.0 全文，项目说明移入 `README.md` |
| 无自动测试 | **已加** `.github/workflows/ci.yml`：拒绝游戏资源/反编译材料/签名材料入库、Python 语法检查、LICENSE 与第三方声明检查 |
| 缺版本兼容矩阵 | **已加** `docs/COMPATIBILITY.md`：SDK、4KB 页约束、宿主注入的分辨率/密度、APK 版本绑定说明 |
| `apk_payload_manifest.h` 与 APK 绑定、无同步再生成 | **已加校验脚本** `tools/check_payload_manifest.py`（比对 APK size/CRC 与清单记录）；BUILD/COMPATIBILITY 说明换版本必须先校验/重生成 |

## 关于 4KB 页与分辨率（审查者关注点）

- 页大小：`libcrypto_1_1.so` / `libssl_1_1.so` / `libcustom_rules.so` / `libblkv.so` 的 `p_align=0x1000`、offset 与 vaddr 相差 0x1000，
  16KB 内核会报 `E-ELF-PAGE-CONGRUENCE`。这是 **guest 库**的限制，与分辨率无关。
- 分辨率：**不得写死**。宿主经 `@ohos.display.getDefaultDisplaySync()` 注入 `sdk_set_display_snapshot()`；
  自检曾把 DisplayMetrics 断言成字面量 2560x1600（`reflection_runtime.c`），**已修复**为有效性检查 + 缺失时 `SKIP`。

## 尚未完成（明确列出，不掩饰）

1. ~~`reflection_runtime.c` 中 DisplayMetrics 字面量断言~~ —— **已修复**：改为 `*_valid` 有效性检查，宿主快照缺失（宽高为 0）时记 `SKIP` 并打印真实 `sdk_display_source()`，不再 `goto done` 中断自检；仓库内 `host/entry/libs/arm64-v8a/libsdktrace.so` 已用修复后源码重建并替换（sha256=4debbe8f123c442936620e36bdd3b0edc9d42aa80139d577454c4119eae2c7eb），二进制内已无 `*_2560` 字符串；
2. `apk_payload_manifest.h` 目前只有校验脚本，没有自动再生成器；
3. 断触/跳帧、极端长帧、标题 30fps 仍未完成最终验收；
4. 大文件（`dvm.c` / `jni_bridge.c`）尚未拆分，属后续维护项。

