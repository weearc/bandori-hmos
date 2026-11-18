# 排错手册

## 1. 日志与诊断文件在哪

应用私有目录：`/data/app/el2/100/base/<bundleName>/haps/entry/files/sdkdex/`

| 文件 | 含义 |
|---|---|
| `diagnostic-log-status.txt` | 诊断队列；`diagnostic_records_lost` 增长说明写得太猛 |
| `connectivity-status.txt` | 网络可达性桥：`tryGetNetworkConnectivity` 返回值、`registered`、`reason` |
| `socket-counters.csv` | socket 级计数；开关文件 `socket-counter-observe` 置 1 开启 |
| `net-room-network.txt` | HTTP 元数据（已脱敏，0644） |
| `play-stall-frames.csv` | 帧与停顿采样（需开启） |
| `vm-lifetime.txt` | VM 生命周期事件 |

## 2. 按现象定位

### 启动即退出，日志里没有 mono_ns

- 先看 `gc1-selftest`：GC1 自检失败会 fail-closed，直接不启动 Unity。
- 空日志文件意味着诊断 worker 没起来，检查 `perf_diag_start` 是否被调用。
- 旧的坑：`fclose` 对自定义流返回 `EBADF`，导致总结行永远写不出去（已修）。

### 卡顿（每 5 秒一次的长帧）

- 历史根因：宿主生命周期驱动在持 GIL 时调用 `log_threads`，每线程 `fflush+fsync`；
  诊断队列满时还有 10ms x 500 次等待，单次可达 2.5 秒。
- 判定方法：`play-stall-frames.csv` 里看大于 33ms 的帧是否等间隔出现，且与 GIL 持有时间同量级。


### 协力/匹配点「确定」后立刻弹「无法连接网络」

- 这不是网络失败：游戏在 `OnClickRankButton` 里先读 `Application.internetReachability`，
  为 0 就直接弹窗，根本不会发起连接。
- 用 `socket-counters.csv` 验证：那一刻应当没有任何到匹配服的 connect。
- 运行时需要实现 `ConnectivityManager.registerDefaultNetworkCallback` 与回调投递，
  需要实现 `ConnectivityManager.registerDefaultNetworkCallback` 与回调投递。

### E-ELF-PAGE-CONGRUENCE

- guest 库的 PT_LOAD 只满足 4KB 同余（`p_align=0x1000`，offset 与 vaddr 差 0x1000），
  在 16KB 页内核上无法映射。
- 这不是配置问题，只能换 4KB 设备或换库。

### 内存缓慢增长

设备端有两个文件可直接读（0644）：

| 文件 | 内容 |
|---|---|
| `leak-release-control` | 释放开关：`1` 开、`0` 关，重启生效；缺失视为开 |
| `leak-release-status.txt` | 每约 60 秒更新：`rate_60s_KiB_s`、`freed_pairs`、`freed_bytes`、`denied`、`fault`、`frames_gt33`、`live_charge` |

判断顺序：

1. 先看 `denied`：应为 0。非 0 说明 arena 准入在拒绝，属于限额问题而不是泄漏。
2. 再看 `rate_60s_KiB_s`：主页静置时开为 0.2、关为 40.6（同一构建）。两者都远高于此说明页面仍在做批量分配。
3. 进歌/切场景时的峰值来自谱面、纹理、音频加载，不经逐帧释放路径；回到主页后应回落到 0.2 量级。
4. `freed_pairs` 应随游玩持续增长（实测约 59 对/秒），不增长说明开关被关或构建没带释放模块。

帧相关的直接读法：`frames_gt33 / frames_n` 是**进程累计**，只在同一对状态快照相减时才是同区间比例。

## 3. 常用命令

```powershell
hdc list targets -v
hdc shell pidof <bundleName>
hdc shell hidumper -s 3302 -a "-i"          # 电量/电流（注意引号）
hdc file recv <device-path> .                 # 取诊断文件
```

## 4. 不要做的事

- 不要在热路径加文件 IO 或 fsync（会让帧直接掉下去）。
- 不要打开移动 GC（moving）或常开 STW。
- 不要修改游戏 DEX/APK/SO，也不要合成登录/同意。
