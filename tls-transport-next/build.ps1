$ErrorActionPreference = 'Stop'
$sdk = 'C:/Program Files/Huawei/DevEco Studio/sdk/default/openharmony/native'
$cmake = "$sdk/build-tools/cmake/bin/cmake.exe"
$ninja = "$sdk/build-tools/cmake/bin/ninja.exe"
$root = $PSScriptRoot
& $cmake -S "$root/vendor/mbedtls-3.6.7" -B "$root/build" -G Ninja "-DCMAKE_MAKE_PROGRAM=$ninja" "-DCMAKE_TOOLCHAIN_FILE=$sdk/build/cmake/ohos.toolchain.cmake" -DOHOS_ARCH=arm64-v8a -DENABLE_TESTING=OFF -DENABLE_PROGRAMS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DUSE_SHARED_MBEDTLS_LIBRARY=OFF -DUSE_STATIC_MBEDTLS_LIBRARY=ON
if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed' }
& $cmake --build "$root/build" --parallel
if ($LASTEXITCODE -ne 0) { throw 'Build failed' }
