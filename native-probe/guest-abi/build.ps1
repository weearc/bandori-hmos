$ErrorActionPreference = 'Stop'
$clang = 'C:\Program Files\Huawei\DevEco Studio\sdk\default\openharmony\native\llvm\bin\clang.exe'
$readelf = 'C:\Program Files\Huawei\DevEco Studio\sdk\default\openharmony\native\llvm\bin\llvm-readelf.exe'
$root = Split-Path -Parent $PSScriptRoot
$output = Join-Path $root 'entry\libs\arm64-v8a\libprobe_android_guest.so'
New-Item -ItemType Directory -Force (Split-Path -Parent $output) | Out-Null
& $clang --target=aarch64-linux-android26 -fPIC -nostdlib -fuse-ld=lld -shared '-Wl,-soname,libprobe_android_guest.so' '-Wl,--build-id=sha1' (Join-Path $PSScriptRoot 'guest.c') -o $output
if ($LASTEXITCODE -ne 0) { throw 'Guest compilation failed' }
& $readelf -h -d $output
if ($LASTEXITCODE -ne 0) { throw 'ELF inspection failed' }
Get-FileHash -Algorithm SHA256 $output
