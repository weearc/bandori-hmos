$ErrorActionPreference='Stop'
$taskRoot=$PSScriptRoot
$nativeSdk='C:/Program Files/Huawei/DevEco Studio/sdk/default/openharmony/native'
$compiler=Join-Path $nativeSdk 'llvm/bin/clang.exe'
$common=@('--target=aarch64-linux-ohos',"--sysroot=$nativeSdk/sysroot",'-std=gnu11','-fPIC','-O1',"-I$taskRoot/generated")
python (Join-Path $taskRoot 'prepare.py')
if($LASTEXITCODE){throw 'prepare failed'}
foreach($source in @('generated/dvm/dvm.c','generated/dvm/dex.c','strict_runtime.c','java_basics.c','java_basics_test.c','android_basics.c','android_basics_test.c','guest_profile.c','collections.c','collections_test.c')) {
 $output=Join-Path $taskRoot ('build/'+[IO.Path]::GetFileNameWithoutExtension($source)+'.o')
 & $compiler @common -c (Join-Path $taskRoot $source) -o $output
 if($LASTEXITCODE){throw "compile failed: $source"}
}
$objects=@((Join-Path $taskRoot 'build/dvm.o'),(Join-Path $taskRoot 'build/dex.o'),(Join-Path $taskRoot 'build/strict_runtime.o'),(Join-Path $taskRoot 'build/java_basics.o'),(Join-Path $taskRoot 'build/java_basics_test.o'),(Join-Path $taskRoot 'build/android_basics.o'),(Join-Path $taskRoot 'build/android_basics_test.o'),(Join-Path $taskRoot 'build/guest_profile.o'),(Join-Path $taskRoot 'build/collections.o'),(Join-Path $taskRoot 'build/collections_test.o'))
& $compiler @common -shared @objects '-Wl,--no-undefined' -lm -o (Join-Path $taskRoot 'build/libsdktrace.so')
if($LASTEXITCODE){throw 'shared link failed'}
& $compiler @common (Join-Path $taskRoot 'runner.c') @objects '-Wl,--no-undefined' -lm -o (Join-Path $taskRoot 'build/sdktrace-runner')
if($LASTEXITCODE){throw 'executable link failed'}
