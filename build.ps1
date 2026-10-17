# Configures (once) and builds the runtime with the Android NDK.
#   .\build.ps1 [target] [-Abi arm64-v8a|x86_64]
# x86_64 builds are only for testing in the Android emulator.
param([string]$Target = "all", [string]$Abi = "arm64-v8a")
$ErrorActionPreference = "Continue"
$sdk = "$env:LOCALAPPDATA\Android\Sdk"
$ndk = "$sdk\ndk\27.3.13750724"
$cm = "$sdk\cmake\3.22.1\bin"
$src = $PSScriptRoot
$dir = if ($Abi -eq "x86_64") { "x86_64" } else { "arm64" }
$out = Join-Path $PSScriptRoot "build\$dir"
if (-not (Test-Path (Join-Path $out "build.ninja"))) {
    & "$cm\cmake.exe" -G Ninja -S $src -B $out "-DCMAKE_MAKE_PROGRAM=$cm\ninja.exe" `
        "-DCMAKE_TOOLCHAIN_FILE=$ndk\build\cmake\android.toolchain.cmake" `
        "-DANDROID_ABI=$Abi" -DANDROID_PLATFORM=android-29 -DANDROID_STL=c++_static `
        -DCMAKE_BUILD_TYPE=RelWithDebInfo
    if ($LASTEXITCODE) { exit $LASTEXITCODE }
}
& "$cm\ninja.exe" -C $out $Target
exit $LASTEXITCODE
