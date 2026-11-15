$ErrorActionPreference = 'Stop'
$base = 'C:\Program Files\Huawei\DevEco Studio\sdk\default\openharmony\native'
$clang = Join-Path $base 'llvm\bin\clang.exe'
$readelf = Join-Path $base 'llvm\bin\llvm-readelf.exe'
$syslib = Join-Path $base 'sysroot\usr\lib\aarch64-linux-ohos'
$output = Join-Path $PSScriptRoot 'libprobe_android_libc.so'
& $clang --target=aarch64-linux-android26 -fPIC -fno-builtin -nostdlib -fuse-ld=lld -shared '-Wl,-soname,libprobe_android_libc.so' '-Wl,--no-as-needed' "-L$syslib" (Join-Path $PSScriptRoot 'guest_libc.c') -lc -o $output
if ($LASTEXITCODE -ne 0) { throw 'Compilation failed' }
& $readelf -h -d --dyn-syms --version-info $output | Set-Content -Encoding utf8 (Join-Path $PSScriptRoot 'readelf.txt')
if ($LASTEXITCODE -ne 0) { throw 'Inspection failed' }
Get-FileHash -Algorithm SHA256 $output
