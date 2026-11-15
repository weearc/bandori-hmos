$ErrorActionPreference = 'Stop'
$root = 'C:\Program Files\Huawei\DevEco Studio\sdk\default\openharmony\native'
$clang = Join-Path $root 'llvm\bin\clang.exe'
$readelf = Join-Path $root 'llvm\bin\llvm-readelf.exe'
$sysroot = Join-Path $root 'sysroot'
$output = Join-Path $PSScriptRoot 'libunity_spike_loader.so'
& $clang --target=aarch64-linux-ohos "--sysroot=$sysroot" -fPIC -shared -fvisibility=hidden (Join-Path $PSScriptRoot 'loader.c') -o $output
if ($LASTEXITCODE -ne 0) { throw 'loader compilation failed' }
& $readelf -h -d --dyn-syms $output | Set-Content -Encoding utf8 (Join-Path $PSScriptRoot 'loader-readelf.txt')
if ($LASTEXITCODE -ne 0) { throw 'loader inspection failed' }
Get-FileHash -Algorithm SHA256 $output
