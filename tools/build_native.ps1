# Build the native runtime (libsdktrace.so) and copy it into the host projects.
# native-probe/ is the release entry; host/ is kept for legacy comparison.
# Usage: powershell -ExecutionPolicy Bypass -File tools\build_native.ps1
$ErrorActionPreference = 'Stop'

# Adapt this single line to your environment:
$SDK = 'C:/Program Files/Huawei/DevEco Studio/sdk/default/openharmony/native'

$repo = Split-Path -Parent $PSScriptRoot
$clang = Join-Path $SDK 'llvm/bin/clang.exe'
if (-not (Test-Path $clang)) { throw ('clang not found: ' + $clang + ' (edit $SDK at the top of this script)') }

Push-Location $repo
try {
    Write-Host '=== 1/4 PCRE2 (UTF-16) ===' -ForegroundColor Cyan
    python sdk-init-next/build_pcre2.py
    if ($LASTEXITCODE -ne 0) { throw 'PCRE2 build failed' }

    Write-Host '=== 2/4 mbedTLS 3.6.7 ===' -ForegroundColor Cyan
    python tls-java-next/native/build_native.py
    if ($LASTEXITCODE -ne 0) { throw 'mbedTLS build failed' }

    Write-Host '=== 3/4 SQLite adapter ===' -ForegroundColor Cyan
    New-Item -ItemType Directory -Force -Path sqlite-backend-next/build | Out-Null
    $sysroot = '--sysroot=' + $SDK + '/sysroot'
    $inc = @('-Isqlite-backend-next/vendor','-Isqlite-backend-next/src','-Isdk-runtime-spike/generated','-Isdk-init-next')
    & $clang '--target=aarch64-linux-ohos' $sysroot '-std=gnu11' '-O2' '-fPIC' @inc '-c' 'sqlite-backend-next/vendor/sqlite3.c' '-o' 'sqlite-backend-next/build/sqlite3.o'
    if ($LASTEXITCODE -ne 0) { throw 'sqlite3.o build failed' }
    & $clang '--target=aarch64-linux-ohos' $sysroot '-std=gnu11' '-O2' '-fPIC' @inc '-c' 'sqlite-backend-next/src/game_sqlite.c' '-o' 'sqlite-backend-next/build/game_sqlite.o'
    if ($LASTEXITCODE -ne 0) { throw 'game_sqlite.o build failed' }
    & $clang '--target=aarch64-linux-ohos' $sysroot '-shared' 'sqlite-backend-next/build/sqlite3.o' 'sqlite-backend-next/build/game_sqlite.o' '-lpthread' '-ldl' '-lm' '-o' 'sqlite-backend-next/build/libgame_sqlite.so'
    if ($LASTEXITCODE -ne 0) { throw 'SQLite adapter build failed' }

    Write-Host '=== 4/4 Assemble libsdktrace.so ===' -ForegroundColor Cyan
    $env:TLS_CANDIDATE_DIR = 'local-build'
    python sdk-runtime-spike/build_tls_candidate.py
    if ($LASTEXITCODE -ne 0) { throw 'libsdktrace.so assembly failed' }

    $so = 'sdk-runtime-spike/build/local-build/libsdktrace.so'
    if (-not (Test-Path $so)) { throw ('artifact missing: ' + $so) }

    Write-Host '=== payload manifest (generated locally from your APK) ===' -ForegroundColor Cyan
    python tools/gen_payload_manifest.py
    if ($LASTEXITCODE -ne 0) { throw 'apk_payload_manifest.h generation failed (set BG_APK or pass --apk)' }

    foreach ($hostDir in @('native-probe','host')) {
        $libs = Join-Path $hostDir 'entry/libs/arm64-v8a'
        New-Item -ItemType Directory -Force -Path $libs | Out-Null
        Copy-Item $so $libs -Force
        $target = Join-Path $libs 'libsdktrace.so'
        $hash = (Get-FileHash $target -Algorithm SHA256).Hash.ToLower()
        $size = (Get-Item $target).Length
        Write-Host ('DONE: ' + $target + '  ' + $size + ' bytes  sha256=' + $hash) -ForegroundColor Green
    }

    Write-Host '=== compliance self check ===' -ForegroundColor Cyan
    python tools/verify_no_embedded_assets.py native-probe/entry/libs/arm64-v8a/libsdktrace.so
    if ($LASTEXITCODE -ne 0) { throw 'runtime still carries embedded game assets; rebuild with embedding disabled' }
}
finally { Pop-Location }
