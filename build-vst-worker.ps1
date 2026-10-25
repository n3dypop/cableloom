param(
    [Parameter(Mandatory)][string]$Vst3SdkRoot,
    [string]$Output = (Join-Path $PSScriptRoot 'build\vst-worker'),
    [ValidateSet('Debug','Release')][string]$Configuration = 'Release',
    [string]$ExpectedSdkCommit = '3cdf9ca5d1f5b1b21e0a86832aa4abe55607bd96'
)
$ErrorActionPreference = 'Stop'
$license = Join-Path $Vst3SdkRoot 'LICENSE.txt'
if (-not (Test-Path -LiteralPath $license)) {
    throw 'Vst3SdkRoot must point to a complete Steinberg VST3 SDK checkout.'
}
$actual = & git -C $Vst3SdkRoot rev-parse HEAD
if ($LASTEXITCODE -ne 0 -or $actual -ne $ExpectedSdkCommit) {
    throw "VST3 SDK commit mismatch. Expected $ExpectedSdkCommit, found $actual."
}
$vswhere = Join-Path ([Environment]::GetFolderPath('ProgramFilesX86')) 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -version '[18.0,19.0)' -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'Visual Studio 2026 with the x64 C++ tools is required.' }
$cmake = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
if (-not (Test-Path -LiteralPath $cmake)) { throw "CMake is missing: $cmake" }
& $cmake -S $PSScriptRoot -B $Output -A x64 -DCABLELOOM_BUILD_VST_WORKER=ON "-DCABLELOOM_VST3_SDK_ROOT=$Vst3SdkRoot"
if ($LASTEXITCODE -ne 0) { throw 'VST3 worker configuration failed.' }
& $cmake --build $Output --config $Configuration --target cableloom-vst-worker -- /m:1 /p:CL_MPCount=1
if ($LASTEXITCODE -ne 0) { throw 'VST3 worker build failed.' }
Write-Output "VST3 worker built at $Output\$Configuration\cableloom-vst-worker.exe"
