$sdk = 'C:\Program Files\Huawei\DevEco Studio\sdk\default\openharmony\native'
& "$sdk\llvm\bin\clang++.exe" --target=aarch64-linux-ohos --sysroot="$sdk\sysroot" -std=c++17 -Wall -Wextra -Werror '-Wl,--no-undefined' -shared '-Wl,-z,max-page-size=16384' '-Wl,-z,common-page-size=16384' -fPIC "$PSScriptRoot\aaudio_bridge.cpp" -lohaudio -o "$PSScriptRoot\libaaudio.so"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }


