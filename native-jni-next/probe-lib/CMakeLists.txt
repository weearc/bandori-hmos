cmake_minimum_required(VERSION 3.10)
project(jni_probe C)

# 自有 JNI 探针库。只依赖 libc，不链接 libsdktrace，也不接触任何原厂库。
#
# 在 DevEco/OHOS 工具链下构建时，CMake 的 OHOS toolchain 会自动提供
# aarch64-linux-ohos 的 sysroot；本文件不写死本机路径。
#
# 等价的手工静态检查/编译命令（不自动执行）：
#   "C:/Program Files/Huawei/DevEco Studio/sdk/default/openharmony/native/llvm/bin/clang.exe" \
#     --target=aarch64-linux-ohos \
#     --sysroot="C:/Program Files/Huawei/DevEco Studio/sdk/default/openharmony/native/sysroot" \
#     -std=gnu11 -fPIC -O1 -Wall -Wextra \
#     -I native-jni-next \
#     -shared -Wl,--no-undefined -ldl -lm -lpthread \
#     -o libjni_probe.so native-jni-next/probe-lib/jni_probe.c

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_library(jni_probe SHARED jni_probe.c)
target_include_directories(jni_probe PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_compile_options(jni_probe PRIVATE
    -std=gnu11 -fPIC -O1 -Wall -Wextra)
# 探针库不需要导出除 Java_/JNI_OnLoad 之外的符号；符号可见性由
# jni_abi.h 的 JNIEXPORT 控制。
set_target_properties(jni_probe PROPERTIES
    PREFIX "lib"
    OUTPUT_NAME "jni_probe")
