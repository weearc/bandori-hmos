# 兼容性与版本矩阵

## 已验证环境

| 项 | 值 | 说明 |
|---|---|---|
| DevEco Studio | 6.0.2.130 | 含 HarmonyOS SDK / NDK clang / hvigor 6.26.9 |
| HarmonyOS SDK | `sdk/default/openharmony/native` | 路径可在构建脚本顶部修改 |
| Python | 3.12 | 构建脚本要求 3.10+ |
| 设备（开发者 A） | MatePad Mini 悦读版，HarmonyOS 7.0.0.111 | 页大小 4096，2560x1600 |
| 设备（外部测试者） | HarmonyOS 7，页大小 4096 | 分辨率非 2560x1600（用于验证可移植性） |

## 设备侧依赖与边界

| 变量 | 是否必须 | 说明 |
|---|---|---|
| 页大小 | 4KB | 现有 guest 库 `libcrypto_1_1.so` / `libssl_1_1.so` / `libcustom_rules.so` / `libblkv.so` 只满足 4KB 同余（`p_align=0x1000`，offset 与 vaddr 差 0x1000）；16KB 内核会在加载时报 `E-ELF-PAGE-CONGRUENCE` |
| 分辨率 | **不要求** | 由宿主通过 `@ohos.display.getDefaultDisplaySync()` 注入 `sdk_set_display_snapshot`；不得写死 |
| 密度 / DPI | **不要求** | 同上，来自宿主 |
| 方向 | **不要求** | 来自宿主；旋转时重新注入 |

> 结论：**4KB 是 guest 库的限制，不是本项目的设计要求**；分辨率与密度必须来自宿主，不能写死。

## 游戏 APK 版本

| 项 | 值 |
|---|---|
| 验证版本 | BanG Dream! 10.1.3（`com.bilibili.star.bili`） |
| 架构 | arm64-v8a，IL2CPP |
| 依赖 | `libil2cpp.so`、`libunity.so`、`libmain.so`、`libcri_ware_unity.so` 等由 APK 提供 |

### 换版本时的注意事项

1. `host/entry/src/main/cpp/apk_payload_manifest.h` **与具体 APK 版本绑定**（记录了 `APK_SOURCE_SIZE` / `APK_SOURCE_CRC` / 每条资产的 size+crc）。
2. 换 APK 后必须重新生成该头文件，否则提取阶段会校验失败。
3. 校验当前头文件是否与你的 APK 匹配：
   ```powershell
   python tools/check_payload_manifest.py --apk D:\path\to\your.apk
   ```
4. `asset-reader-next/assets.inc` 同样与 APK 绑定，且**不入库**，由 `generate.py` 本地生成：
   ```powershell
   python asset-reader-next/generate.py --apk D:\path\to\your.apk
   ```

## 已验证能力（截至本文件更新时）

| 能力 | 状态 |
|---|---|
| 启动 → 标题 → 登录 → 首页 | 可用 |
| 本地单人游玩 | 可用 |
| 协力可达性（`getNetworkConnectivity`） | 可用：`tryGetNetworkConnectivity=2`、`registered=1`、`reason=ok` |
| 创建房间 → 进入房间界面 | 可用（进入后按测试规程 3 秒内退出，避免真实匹配） |
| 帧率（首页） | p50 15.60 ms、p99 23.61 ms、>33 ms 仅 4/3593 |
| 内存回收 | 未启用（`leak-proxy-reclaim=0`），native JNIBridge 体证据仍 UNKNOWN |

## 已知未完成

- 断触/跳帧的最终验收（需要真实演奏窗口，且需在修复后重测）；
- 标题界面 30fps（独立现象，未定位）；
- 极端长帧（>1s）仍有未归因残余；
- `apk_payload_manifest.h` 的自动重新生成脚本（当前只有校验脚本）。
