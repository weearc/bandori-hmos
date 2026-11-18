# overlay（构建期注入的运行时模块）


这些文件不在 `*-next` 主源码树里被直接 `#include`，而是在组装 `libsdktrace.so` 时
由构建脚本复制进临时源码树，再与主运行时一起编译。

- `leak_release_candidate.h` / `.inc`：逐帧临时对象（proxy 的 `Object[1]` 与装箱 `Long`）的释放模块。
  接口：`lr_start` / `lr_register` / `lr_returned` / `lr_poll` / `lr_frame` / `lr_denied` 等。
  由 `sdk-init-next` 的 proxy 分派与分配路径登记、轮询与释放；开关文件 `leak-release-control`（1 开 / 0 关，重启生效）。

构建脚本请在自己的组装流程里把本目录加入 include 路径。
