# Builds the runtime, repacks the APK, aligns and signs it.
#   .\package.ps1 [-SkipBuild] [-Abi arm64-v8a|x86_64]
# Output: out\AC_Pirates_arm64.apk (or out\AC_Pirates_emulator_x86_64.apk)
param([switch]$SkipBuild, [string]$Abi = "arm64-v8a")
$ErrorActionPreference = "Continue"
$root = $PSScriptRoot
$sdk = "$env:LOCALAPPDATA\Android\Sdk"
$bt = "$sdk\build-tools\36.1.0"
$strip = "$sdk\ndk\27.3.13750724\toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-strip.exe"
$work = Join-Path $root "work"
$out = Join-Path $root "out"
New-Item -ItemType Directory -Force $out | Out-Null

if (-not $SkipBuild) {
    & (Join-Path $root "build.ps1") SparkApp -Abi $Abi
    if ($LASTEXITCODE) { exit $LASTEXITCODE }
}

$dir = if ($Abi -eq "x86_64") { "x86_64" } else { "arm64" }
$stripped = Join-Path $work "libSparkApp.$dir.so"
& $strip --strip-unneeded -o $stripped (Join-Path $root "build\$dir\libSparkApp.so")
if ($LASTEXITCODE) { exit $LASTEXITCODE }

# Java bug fix (see tools/patch_dex.py).
$dexIn = Join-Path $work "dex"
$dexOut = Join-Path $work "dex_patched"
New-Item -ItemType Directory -Force $dexIn, $dexOut | Out-Null
& 7z e -y (Join-Path $work "original.apk") "classes*.dex" "-o$dexIn" | Out-Null
foreach ($dex in Get-ChildItem $dexIn -Filter "classes*.dex") {
    python -I (Join-Path $root "tools\patch_dex.py") $dex.FullName (Join-Path $dexOut $dex.Name)
    if ($LASTEXITCODE) { exit $LASTEXITCODE }
}

# In-game window for typing cheat codes (tools/java), added as one more dex file.
$javaOut = Join-Path $work "java"
$androidJar = "$sdk\platforms\android-34\android.jar"
Remove-Item -Recurse -Force $javaOut -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force (Join-Path $javaOut "classes") | Out-Null
$sources = (Get-ChildItem (Join-Path $root "tools\java") -Recurse -Filter *.java).FullName
javac --release 8 -encoding UTF-8 -Xlint:-options -cp $androidJar -d (Join-Path $javaOut "classes") $sources
if ($LASTEXITCODE) { exit $LASTEXITCODE }
$classes = (Get-ChildItem (Join-Path $javaOut "classes") -Recurse -Filter *.class).FullName
& "$bt\d8.bat" --lib $androidJar --min-api 21 --output $javaOut $classes
if ($LASTEXITCODE) { exit $LASTEXITCODE }
$extraDex = "classes" + ((Get-ChildItem $dexIn -Filter "classes*.dex").Count + 1) + ".dex"
Copy-Item (Join-Path $javaOut "classes.dex") (Join-Path $dexOut $extraDex) -Force

# Full screen on wide phones (see tools/patch_manifest.py).
& 7z e -y (Join-Path $work "original.apk") AndroidManifest.xml "-o$dexIn" | Out-Null
python -I (Join-Path $root "tools\patch_manifest.py") (Join-Path $dexIn "AndroidManifest.xml") (Join-Path $dexOut "AndroidManifest.xml") 3.0
if ($LASTEXITCODE) { exit $LASTEXITCODE }

$unsigned = Join-Path $work "unsigned.apk"
$aligned = Join-Path $work "aligned.apk"
python -I (Join-Path $root "tools\repack.py") (Join-Path $work "original.apk") $stripped (Join-Path $work "libSparkApp_arm32.so") $unsigned $Abi $dexOut
if ($LASTEXITCODE) { exit $LASTEXITCODE }
& "$bt\zipalign.exe" -p -f 4 $unsigned $aligned
if ($LASTEXITCODE) { exit $LASTEXITCODE }

# Local signing key, created on first use (only used to sign this repacked APK).
$ks = Join-Path $work "acp64.keystore"
$props = Join-Path $work "keystore.properties"
if (-not (Test-Path $ks)) {
    $pw = -join ((48..57) + (65..90) + (97..122) | Get-Random -Count 24 | ForEach-Object { [char]$_ })
    Set-Content -Encoding ascii $props "password=$pw"
    & keytool -genkeypair -keystore $ks -alias acp64 -keyalg RSA -keysize 2048 -validity 10000 `
        -storepass $pw -keypass $pw -dname "CN=acp64 local build" | Out-Null
    if ($LASTEXITCODE) { exit $LASTEXITCODE }
}
$pw = ((Get-Content $props) -replace "^password=", "").Trim()
$final = Join-Path $out $(if ($Abi -eq "x86_64") { "AC_Pirates_emulator_x86_64.apk" } else { "AC_Pirates_arm64.apk" })
& java -jar "$bt\lib\apksigner.jar" sign --ks $ks --ks-key-alias acp64 --ks-pass "pass:$pw" --key-pass "pass:$pw" --out $final $aligned
if ($LASTEXITCODE) { exit $LASTEXITCODE }
Get-Item $final | Select-Object FullName, Length
