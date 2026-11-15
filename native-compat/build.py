"""Compile the standalone Android log bridge; never install or run it."""
from pathlib import Path
import subprocess

folder = Path(__file__).resolve().parent
sdk = Path('C:/Program Files/Huawei/DevEco Studio/sdk/default/openharmony/native')
subprocess.run([str(sdk/'llvm/bin/clang.exe'), '--target=aarch64-linux-ohos',
    '--sysroot='+str(sdk/'sysroot'), '-shared', '-Wl,-z,max-page-size=16384', '-Wl,-z,common-page-size=16384', '-fPIC', '-O2', '-Wall', '-Wextra',
    '-Werror', '-Wl,--no-undefined', '-Wl,-soname,liblog.so', str(folder/'android_log.c'),
    '-lhilog_ndk.z', '-o', str(folder/'libgame_android_log.so')], check=True)
print('Compiled standalone log bridge; no HAP integration or device execution')
subprocess.run([str(sdk/'llvm/bin/clang.exe'), '--target=aarch64-linux-ohos',
    '--sysroot='+str(sdk/'sysroot'), '-shared', '-Wl,-z,max-page-size=16384', '-Wl,-z,common-page-size=16384', '-fPIC', '-O2', '-Wall', '-Wextra',
    '-Werror', '-Wl,--no-undefined', str(folder/'assets.c'),
    '-o', str(folder/'libgame_assets.so')], check=True)
print('Compiled extracted-file asset backend; JNI object binding not implemented')
