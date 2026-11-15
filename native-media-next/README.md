# 原生媒体后端：72dp真实音频解码与72dm解复用基线

2026-11-09。此目录使用真实鸿蒙媒体设施，不是mock媒体服务。72dq原Unity映射/重定位成功，构造器和引擎初始化未执行；本页72dp音频结果在72dq再次通过。__FD_SET_chk已补齐，不能再称当前加载阻塞。

## 72dp真实音频解码

[72dp manifest](../batch-validation/integrated-login-tls-next-rebuild72dp-device-manifest.json)与[报告](../sdk-runtime-spike/device-login-tls-integrated-login-tls-next-rebuild72dp-agent-rerun.txt)：`NATIVE_MEDIA_CODEC_PROBE PASS ... pcm_exact=1`。两包各800字节G711 μ-law（Android audio/g711-mlaw显式映射OH audio/g711mu），真实OH_AudioCodec解码成8000Hz/mono/16bit共3200字节PCM，前包±32124、后包±15996，全部1600样本逐一比较顺序和值。三次输出含EOS且全部release；PTS起0/EOS200000us。输出format复制后在codec销毁后读取仍正常。未播放。

`media_codec.c/.h`提供14个NDK入口，OH异步callback→同步dequeue模型：16个codec槽、每方向128事件/持有buffer，0xC1高位tag+48位代际令牌；独立ticket全局单调且耗尽拒绝。统一序号保持buffer/format事件顺序，monotonic timeout、无全局锁跨OH调用、delete先cancel/wake再等在途pin，旧令牌不可命中新实例。输出attr校验offset/size/capacity，flags显式转换。

**限制：audio-only；flush/video/Surface/crypto/encoder unsupported；stop后实例终态，必须delete新建。** 尚未验收restart callback隔离、阻塞等待取消与并发压力；AAC/MP3及原游戏编码未测试。当前压缩输入直接喂codec，未证明Extractor压缩轨→Codec完整链路。格式clone用OH_AVFormat_Copy保留codec_config，PCM Android 2/3/4/21/22显式转换OH枚举；未知编码拒绝，并有实际设备往返/独立副本测试。

72dn仅编译；72do产生输出及EOS但探针依赖初始format-change而check19失败；72dp主动查真实输出描述，format_changes=0合法，未伪造事件。SDK另有API20同步Query/Get接口，可评估后续简化，本轮不推翻已有callback实现。官方输出描述所有权文字有矛盾，显式要求手动Destroy；已保存[官方参考](../batch-validation/unity-bootstrap-20261109/oh-native-audiocodec-reference.md)，实现复制独立snapshot后销毁源。

构建器新增4个codec/probe文件，全部媒体对象排除guest arena，链接native_media_acodec；`NATIVE_MEDIA_CODEC_PROBE=1`启用codec探针，`NATIVE_MEDIA_PROBE=1`启用旧解复用探针，都要求UNITY_LIBRARY_PREFLIGHT=1。capture完成不等于probe PASS。下面72dm范围描述作为历史基线保留，其“未解码”仅指当时：

## 实现

- `media_extractor.c/.h`：NDK Extractor的13个入口已接线，**仅FD本地常规文件、单选轨**有正向功能；URL/custom明确返回UNSUPPORTED，多选另一轨也拒绝，未调用网络URI后端。FD校验范围并dup，source/demux/cache随delete清理。
- 缓存当前sample，重复read/getTime/getTrackIndex不前进，advance才读取下一sample；EOS只认OH flag，硬错误保存独立诊断，不伪造空数据成功。读取接口负值本身不能区分全部错误，须查看last_error/内部状态。
- seek显式映射Android previous0/next1到OH previous1/next0，closest2一致；微秒除1000转毫秒，存在亚毫秒截断，非纳秒或微秒精确seek承诺。
- 每个extractor缓存最多4MiB，超过时明确错误，不无限增长或悄悄丢样本。64个活动extractor与64个活动format，两类代际token有不同高位tag、耗尽拒绝，先查表再访问底层。
- `media_format.c/.h`：包装自有OH_AVFormat，wrap无论成功失败都接管所有权；6个NDK getter/setter/delete入口。已知key映射，并适配OH double帧率到Android float。字符串借用至修改/删除。
- **color-format/stride不直通**：底层同名key不能证明像素枚举/行跨度单位等价。尚无视频数值转换；原生字段不会冒充Android字段返回。显式setInt32写入的Android值在独立字段保留，不改变原生pixel/stride。

## 设备证据

[72dm manifest](../batch-validation/integrated-login-tls-next-rebuild72dm-device-manifest.json)与[完整报告](../sdk-runtime-spike/device-login-tls-integrated-login-tls-next-rebuild72dm-agent-rerun.txt)。SDK SHA256 `63f4604b59db0dbd501bbf93ee2668741bd3fcb18690d297e57f6eeddcbcf5df`；HAP SHA256 `2c95f15d1ca921372ff79719e952c41b30b5d0f4c2dc008b88471857013877cb`。候选/staged/HAP内SDK与report/hilog/aa hash核对通过。

`NATIVE_MEDIA_BACKEND_RESULT PASS`：真实FD→OH源→解复用，2声道48000Hz，首次4096字节样本前缀按已知PCM核验、重复read保持样本；500000us seek返回500000us；完整读取47个sample共192000字节，EOS及之后seek恢复通过。元数据类型/跨类型句柄隔离通过。此探针**直接调用适配后端**，不是原Unity已执行媒体路径，也没有播放或解码声音。

设备输入[WAV](../batch-validation/media-fixture-device-72dm.wav)与[参数/hash](../batch-validation/media-fixture-device-72dm.json)已取回，全部192044字节与确定性生成器一致，SHA256 `3978388832cb7fdb022af118f9f144eec058a7fb11860fb287391303b61274da`。样本内容仅验证了起始前缀、重复与seek恢复，未声称对每个输出sample全量逐字节比对。

72dk基础读取/seek首版通过；72dl扩展探针check39发现native同名key被错误当Android值，runner正确停止，不能算该轮通过。72dm修复字段隔离后完整回归通过。失败候选、日志与manifest保留。

## 构建、限制和下一步

builder编译本目录6个文件且不进入guest arena；链接native_media_core、codecbase、avsource、avdemuxer。保留现有三个Unity诊断flag，额外 `NATIVE_MEDIA_PROBE=1`启用此probe；当前probe位于UNITY_LIBRARY_PREFLIGHT条件内，两者都要启用。capture成功/最终停止标记不等于所有测试PASS，必须检查媒体结果行。

非零FD段、压缩音视频、多轨、URI、自定义callback源、DRM、视频pixel/stride实际转换和真实codec均未验证/未完成。原format getString/getInt64已接线但本轮未专项覆盖全部字段。单文件样例成功不等于完整MediaExtractor支持。

下一主线为OH异步音频解码器与Android同步dequeue/queue语义。必须建真实输入/输出buffer所有权、callback队列、超时/flush/stop/delete唤醒取消，并验证压缩音频输出PCM，禁止固定下标或假帧清除符号。源码/调研从[研究索引](../research-index.md)进入。
