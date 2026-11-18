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

## 已验证能力

| 能力 | 状态 |
|---|---|
| 启动 → 标题 → 登录 → 首页 | 可用 |
| 本地单人游玩 | 可用 |
| 联机：协力、匹配、进入房间、房间内交互 | 可用（真机确认） |
| 网络可达性桥 | `tryGetNetworkConnectivity=2`、`registered=1`、`reason=ok` |
| 帧分布（游玩中） | >33 ms 占 1.0–1.1%，max 42.2 ms |
| 持锁时长（诊断导出） | p50 0.019 ms / p99 0.53 ms / max 7.09 ms |
| 主页静置内存增速 | 开：0.2 KiB/s；关：40.6 KiB/s（同一构建，仅切开关） |
| 逐帧临时对象释放 | 300 秒内释放 16,953 对、3,036,400 字节 |

## 剩余工作

- 单次 50.5 ms 长帧的归因（与慢持锁、大分配、region 提交均不重合）；
- 持锁 p99.9 = 2.27 ms 的偶发尖峰（1,080 秒内 100 次超 2 ms，集中在后台帧队列消费与自治循环快照）；
- 进歌加载期增长（约 195 MB/h）的进一步压缩——这部分分配不经逐帧释放路径；
- `apk_payload_manifest.h` 的自动生成脚本（当前只有校验脚本）。

