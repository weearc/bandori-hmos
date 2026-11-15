param([string]$DevEcoRoot = 'C:\Program Files\Huawei\DevEco Studio', [switch]$UsePrebuiltNative)
$ErrorActionPreference = 'Stop'
$projectRoot = Join-Path $PSScriptRoot 'native-probe'
$nodePath = Join-Path $DevEcoRoot 'tools\node\node.exe'
$hvigorPath = Join-Path $DevEcoRoot 'tools\hvigor\hvigor\bin\hvigor.js'
$ioBridge = Join-Path $PSScriptRoot 'node_file_stdio.cjs'
foreach ($toolPath in @($nodePath, $hvigorPath, $ioBridge)) {
    if (-not (Test-Path -LiteralPath $toolPath)) { throw "Required build tool missing: $toolPath" }
}
$previousSdk = $env:DEVECO_SDK_HOME
$previousHvigor = $env:HVIGOR_USER_HOME
$previousJava = $env:JAVA_HOME
$previousPath = $env:Path
$moduleProfilePath = Join-Path $projectRoot 'entry\build-profile.json5'
$moduleProfileOriginal = $null
$temporaryNativeFiles = @()
if ($UsePrebuiltNative) {
    & python (Join-Path $PSScriptRoot 'compile_native_direct.py')
    if ($LASTEXITCODE -ne 0) { throw 'Direct compilation of actual CMake commands failed' }
}
Push-Location -LiteralPath $projectRoot
try {
    if ($UsePrebuiltNative) {
        $moduleProfileOriginal = [IO.File]::ReadAllText($moduleProfilePath)
        $moduleProfile = $moduleProfileOriginal | ConvertFrom-Json
        $moduleProfile.buildOption.PSObject.Properties.Remove('externalNativeOptions')
        foreach ($libraryName in @('libprobe.so', 'libprobe_payload.so')) {
            $destination = Join-Path $projectRoot ('entry\libs\arm64-v8a\' + $libraryName)
            if (Test-Path -LiteralPath $destination) { throw "Refusing to overwrite existing prebuilt library: $libraryName" }
            $source = Join-Path $projectRoot ('entry\build\default\intermediates\cmake\default\obj\arm64-v8a\' + $libraryName)
            Copy-Item -LiteralPath $source -Destination $destination
            $temporaryNativeFiles += $destination
        }
        [IO.File]::WriteAllText($moduleProfilePath, ($moduleProfile | ConvertTo-Json -Depth 20), [Text.UTF8Encoding]::new($false))
    }
    $env:DEVECO_SDK_HOME = Join-Path $DevEcoRoot 'sdk'
    $env:HVIGOR_USER_HOME = Join-Path $projectRoot '.hvigor-home'
    $jdkRoot = Join-Path $DevEcoRoot 'jbr'
    if (-not (Test-Path -LiteralPath (Join-Path $jdkRoot 'bin\java.exe'))) { throw 'DevEco bundled Java runtime not found' }
    $env:JAVA_HOME = $jdkRoot
    $env:Path = (Join-Path $jdkRoot 'bin') + ';' + $previousPath
    & $nodePath --require $ioBridge $hvigorPath --mode module -p module=entry -p product=default assembleHap --no-daemon --no-parallel
    if ($LASTEXITCODE -ne 0) { throw "HAP build failed with exit code $LASTEXITCODE" }
    $signedHap = Join-Path $projectRoot 'entry\build\default\outputs\default\entry-default-signed.hap'
    if (-not (Test-Path -LiteralPath $signedHap)) { throw 'Build did not produce a signed HAP' }
    Get-Item -LiteralPath $signedHap | Select-Object FullName, Length, LastWriteTime
} finally {
    if ($null -ne $moduleProfileOriginal) {
        [IO.File]::WriteAllText($moduleProfilePath, $moduleProfileOriginal, [Text.UTF8Encoding]::new($false))
    }
    foreach ($temporaryFile in $temporaryNativeFiles) { Remove-Item -LiteralPath $temporaryFile }
    Pop-Location
    $env:DEVECO_SDK_HOME = $previousSdk
    $env:HVIGOR_USER_HOME = $previousHvigor
    $env:JAVA_HOME = $previousJava
    $env:Path = $previousPath
}
