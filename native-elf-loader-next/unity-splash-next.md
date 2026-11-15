# Unity 原 splash 的资源与 View 路径

原 APK SHA-256 `2ff2baf1b8fe78692b67507f88d3849422b672a0a929b374a878e1087d1519ec`。REA/JADX 只读定点证据：类 `com.unity3d.player.a.f` 清单 `ev_d7b3d1128c0ce5f4a39d53cd021762f4ad9dc6be1770e556d1cb1d3f647e80ae`，构造器 `ev_71fdcdfc7d7b402ffd65200c7b216670d007d1f3f4f177f20233c47aee570d2e`，`onLayout` `ev_6ad175b232cf3734a0b992dfab5345d93be64fe5be9a8061cb728e588b47617c`，`onDetachedFromWindow` `ev_5d7aadbc79e37d004b2b664542030ef8129b95e3c7b633a0cc3ebb0bea9eaa68`。均为静态观察，不能证明当前设备已绘制。

构造器 `(Landroid/content/Context;I)V` 在 `View.<init>(context)` 后保存模式值，默认背景黑色；随后调用 `getResources().getIdentifier("unity_static_splash", "drawable", getContext().getPackageName())`，存在时调用 `forceLayout()`；再查 `staticSplashScreenBackgroundColor` 的 color ID，非零则调用 `getColor(I)` 并保存；最后 `setBackgroundColor(I)`。必须使用该View绑定的原Context，而不是把View对象本身传给Context资源实现。

已生成的真实 APK 表 `sdk-init-next/apk_resource_table.inc` 指向 drawable `0x7f070176`、color `0x7f0500db`、ARGB `0xff231f20`；`resource-table-next/apk-resource-report.json` 将drawable映射为 `res/iF.png`。从原APK只读核对该PNG 785字节、IHDR 1136×640、SHA-256 `63098eb9b3e0381eaf1482e72023bff58d192c6a3e13b270509fd392df980906`。资源条目实际存在，不应返回0来跳过 splash；设备配置/资源选择仍须走真实表及资源后端。

类清单没有自定义 `onDraw`：原类覆盖的是 `onLayout(ZIIII)V` 和 `onDetachedFromWindow()`。`onLayout` 在图片ID非零时通过 `BitmapFactory.Options.inScaled=false` 和 `BitmapFactory.decodeResource` 加载源图，以实际View宽高及模式计算尺寸、`Bitmap.createScaledBitmap`，设置资源densityDpi，组合 `ColorDrawable`、居中的 `BitmapDrawable` 与 `LayerDrawable` 后调用 `View.setBackground`；相同尺寸可复用，旧缩放图需要按原逻辑回收。JADX对重复块有警告，精确缩放及舍入应以DEX/实际结果校验。detach 先调用super，再回收并清除原图及缩放图。绘制来自Android View背景机制，不应将构造器返回或一个保存的背景颜色报告为已绘制。

现有 `platform_runtime.c` 有APK-backed `Resources.getIdentifier` 和经表校验的 `getColor`，需确认候选构建启用相应资源表；`view_state_runtime.c` 本轮补足 View绑定Context及getResources委托，但尚未接入真实布局/绘制。最早的潜在下一步是实际 `forceLayout` 和 `setBackgroundColor`；可维护真实guest请求状态，但布局、窗口挂载、背景内容、尺寸变化及Surface/帧提交必须由真实宿主后端处理，不能空成功冒充显示。BitmapFactory、Bitmap、Drawable和View绘制链需要按实际异常与资源证据逐步实现；不要关闭 manifest 的 splash 开关或用伪数据跳过资源和绘制。
