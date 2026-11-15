# Unity 窗口属性：x.a(Activity) 定点证据

REA/JADX 定点记录 `ev_d4e800ae733bee96b9aaa7b2bd3aba2fe80cb47f50946cdb4803252d499a116e`，原 APK SHA-256 `2ff2baf1b8fe78692b67507f88d3849422b672a0a929b374a878e1087d1519ec`，目标 `com.unity3d.player.x.a(Landroid/app/Activity;)V`，类清单 `ev_dadbbc287d86af265a35265ebe71d5e785a0ed3ef7c2d13520b1b0122ebc5c2c`。静态证据不证明设备分支实际执行。

Activity 或其 Window 为 null，或 PIE_SUPPORT 不成立则返回；否则依据 VANILLA_ICE_CREAM_SUPPORT、RED_VELVET_CAKE_SUPPORT 及 ApplicationInfo metadata `unity.render-outside-safearea` 计算模式，最后执行 `activity.getWindow().getAttributes().layoutInDisplayCutoutMode = i2`。JADX 对分支变量发出类型推断和重复块警告：可确定值包含0、3和布尔转整数路径，但具体所有异常路径/分支值不应仅凭伪代码过度断言。当前APK已保存 manifest metadata 该项为 true（`login-milestone-next/application-metadata-original.json`）；实际运行分支仍需平台版本状态证据。

此方法直接写 `layoutInDisplayCutoutMode:I`，没有读取/修改 `flags`，也没有调用 `Window.setAttributes()`；DEX call-edge 数据同样只记录 getAttributes。不要因没有 setAttributes 就假定它已被系统窗口服务应用。Android 官方定义该字段表示窗口如何进入屏幕挖孔区域，默认0（DEFAULT）；1为SHORT_EDGES，3为ALWAYS。[Android LayoutParams](https://developer.android.com/reference/android/view/WindowManager.LayoutParams)；[挖孔布局说明](https://developer.android.com/develop/ui/views/layout/display-cutout)。

最小实现：为每个VM/Window保持稳定同一 `WindowManager.LayoutParams` guest对象，定义真正可由DEX `iput/iget` 使用的 `layoutInDisplayCutoutMode:I` 实例字段；按真实Android默认初始化0，允许任意原始int读写，隔离不同Window。保证对象生命周期和已有字段布局/继承一致，不把一份通用空Object当作LayoutParams。字段guest写入与鸿蒙真实挖孔/安全区策略分开报告；当前没有已验证的OH应用或读取回执。后续涉及 `setAttributes` 或读取其它字段时明确不支持，除非另有真实实现。不得把可写Java字段表述为设备画面已布局到挖孔。
