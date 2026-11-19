# 法律与合规

## 本项目许可证

**GNU Affero General Public License v3.0（AGPL-3.0）**
版权：Copyright (C) 2026 weearc（仅限本项目原创代码）。全文见仓库根目录 `LICENSE`（标准 AGPL 文本，未做任何改动）。

AGPL 要点（不构成法律意见）：

- 可自由使用、修改、分发；分发时须提供完整对应源码并保留许可证；
- 第 13 条：若把修改版作为网络服务提供给他人，须向使用者提供对应源码；
- 不提供担保。

## 上游代码与第三方组件

本项目的 **DEX 解释器部分继承自 Project Lunaria**，并保留了原作者版权信息。
Project Lunaria 以 **Mozilla Public License 2.0（MPL-2.0）** 发布。

| 组件 | 版本 / 来源 | 许可证 | 位置 |
|---|---|---|---|
| Project Lunaria（DEX 解释器上游） | 上游 MPL-2.0 代码 | MPL-2.0 | `sdk-init-next/dvm.c`、`sdk-init-next/dex.c` 等解释器文件，文件头保留版权声明 |
| PCRE2 | 10.45 | BSD-3-Clause | `sdk-init-next/vendor/pcre2-10.45/` |
| Mbed TLS | 3.6.7 | Apache-2.0 | `tls-transport-next/vendor/mbedtls-3.6.7/` |
| SQLite | 3.x amalgamation | Public Domain | `sqlite-backend-next/vendor/` |

### MPL-2.0 与 AGPL-3.0 的组合说明

MPL-2.0 与 AGPL-3.0 可以组合分发，但有前提，**不能把上游 MPL 代码简单声明成单方 AGPL 作品**：

- MPL-2.0 是**文件级** copyleft：被修改的 MPL 源文件必须继续以 MPL-2.0 提供；
- 允许把 MPL 文件与大工程（可含 AGPL 部分）一起分发，形成“更大作品”，前提是 MPL 文件仍保留 MPL 条款与版权声明；
- 因此本仓库的准确表述是：**本项目原创部分 AGPL-3.0；继承自 Lunaria 的解释器文件仍为 MPL-2.0**；
- 若你要再分发，请保留 `dvm.c` / `dex.c` 等文件头的 MPL 版权声明，并在文档中同时列出 MPL-2.0。
参考：Mozilla 关于 MPL 与其它许可证组合的说明（MPL 2.0 FAQ）。

## 不包含的内容

- 不含游戏本体：没有 APK、没有 `classes*.dex`、没有 `assets/`、没有 `libil2cpp.so` / `libunity.so` 等游戏自带库；
- 不含由游戏 APK 派生的素材：`asset-reader-next/assets.inc` 与 `source-manifest.json` 只存在于你的工作区，
  由 `asset-reader-next/generate.py` 从你自己的 APK 生成（被 `.gitignore` 排除）。生成器**默认不嵌入任何字节**，
  只写清单；`--embed` 是诊断用开关，会把小素材正文写进二进制，**不要用于对外发布的构建**；
- 不含由游戏 APK 派生的资源表：`apk_resource_table.inc` 已从仓库移除（此前 `sdk-init-next/` 与
  `preserved-template/source/` 各有一份，来自原 APK 的 `resources.arsc`，含 3072 个资源 ID 与 192 个颜色）。
  现在由 `resource-table-next/gen_apk_resource_table.py` 在构建时按你本地的 APK 生成；
  未生成时 `platform_runtime.c` 通过 `__has_include` 回退到空表，构建不会失败；
- 不含预编译的运行时二进制：`libsdktrace.so` 已从仓库移除（`host/entry/libs/arm64-v8a/` 与
  `sdk-runtime-spike/preserved-template/` 两份旧库都曾内嵌 81 个 APK 素材条目，合计 663,006 字节）。
  现在必须由 `tools/build_native.ps1` / `build_tls_candidate.py` 在你本地构建，并用
  `tools/verify_no_embedded_assets.py` 自检：一旦检测到素材字节，脚本返回非 0；
- 不含 Unity 引导配置正文：`assets/bin/Data/boot.config` 只在显式设置 `BG_EMBED_BOOT_CONFIG=1` 的
  诊断构建里才会被嵌入，默认不嵌入；
- 不含反编译分析材料：`*.smali.txt` 等 DEX 反汇编文件已移除，只保留接口签名与行为分析；
- 不含账号、Cookie、Token 或真实用户数据；
- 不含签名私钥（`*.p12` / `*.p7b` / `*.cer` 均在 `.gitignore` 中）。

## 图标与位图

- 仓库内 `host/**/app_icon.png` 是自制占位图（纯几何绘制），不是从游戏 APK 提取的图标；
- `tools/prepare_from_apk.py` 会在本地把占位图替换成你 APK 里的图标（仅写入工作区，不入库）；
- 提交前用 `git checkout -- host/**/app_icon.png` 还原。

## 使用者的责任

1. 你需要**自己拥有该游戏**，并从自己的安装包提取 guest 库与 DEX；
2. 本项目是**运行时兼容层**，不提供、不修改、不再分发游戏本体；
3. 请遵守游戏用户协议与当地法律；不要用于账号共享、外挂、加速或商业再分发；
4. 本项目**未实现**反作弊绕过、支付绕过或在线对局伪造；
5. **自己编译自己使用**、**公开源码**、**分发内含原 APK 的 HAP** 是三种不同的行为，合规风险不同；本仓库只提供前两者所需材料，不含 APK。

## 免责

本项目为个人研究性兼容层，按现状提供，不含任何形式的担保。
