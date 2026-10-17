# Builds the ARM64 APK from your own copy of the game: builds the runtime, repacks the game's APK around it,
# aligns and signs it.
#   .\package.ps1 -Xapk <game .xapk or .apk> [-Obb <game .obb>]   first time: imports the game into work\
#   .\package.ps1 [-SkipBuild] [-Abi arm64-v8a|x86_64]             later builds
# Output: out\AC_Pirates_arm64.apk (or out\AC_Pirates_emulator_x86_64.apk)
param([string]$Xapk = "", [string]$Obb = "", [switch]$SkipBuild, [string]$Abi = "arm64-v8a")
$ErrorActionPreference = "Continue"
$root = $PSScriptRoot
. (Join-Path $root "tools\android_sdk.ps1")
$ndk = Get-AcpNdkTools
$pk = Get-AcpPackagingTools
$py = $pk.Python
$bt = $pk.BuildTools
$androidJar = $pk.AndroidJar
$work = Join-Path $root "work"
$out = Join-Path $root "out"
New-Item -ItemType Directory -Force $work, $out | Out-Null

# The game's own files, imported once into work\ (see tools/import_game.py).
if ($Xapk) {
    & $py -I (Join-Path $root "tools\import_game.py") $Xapk $work $Obb
    if ($LASTEXITCODE) { exit $LASTEXITCODE }
}
$originalApk = Join-Path $work "original.apk"
$gameLib = Join-Path $work "libSparkApp_arm32.so"
if (-not (Test-Path $originalApk) -or -not (Test-Path $gameLib)) {
    Stop-Acp "les fichiers du jeu manquent dans work\. Lance d'abord : .\package.ps1 -Xapk <chemin du fichier .xapk du jeu>"
}

if (-not $SkipBuild) {
    & (Join-Path $root "build.ps1") SparkApp -Abi $Abi
    if ($LASTEXITCODE) { exit $LASTEXITCODE }
}

$dir = if ($Abi -eq "x86_64") { "x86_64" } else { "arm64" }
$stripped = Join-Path $work "libSparkApp.$dir.so"
& $ndk.Strip --strip-unneeded -o $stripped (Join-Path $root "build\$dir\libSparkApp.so")
if ($LASTEXITCODE) { exit $LASTEXITCODE }

# The game's Java code and manifest, patched below.
$dexIn = Join-Path $work "dex"
$dexOut = Join-Path $work "dex_patched"
Remove-Item -Recurse -Force $dexIn, $dexOut -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $dexIn, $dexOut | Out-Null
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [IO.Compression.ZipFile]::OpenRead($originalApk)
try {
    foreach ($e in $zip.Entries) {
        if ($e.FullName -match '^classes\d*\.dex$' -or $e.FullName -eq "AndroidManifest.xml") {
            [IO.Compression.ZipFileExtensions]::ExtractToFile($e, (Join-Path $dexIn $e.FullName), $true)
        }
    }
} finally {
    $zip.Dispose()
}

# Java bug fix (see tools/patch_dex.py).
foreach ($dex in Get-ChildItem $dexIn -Filter "classes*.dex") {
    & $py -I (Join-Path $root "tools\patch_dex.py") $dex.FullName (Join-Path $dexOut $dex.Name)
    if ($LASTEXITCODE) { exit $LASTEXITCODE }
}

# In-game window for typing cheat codes (tools/java), added as one more dex file.
$javaOut = Join-Path $work "java"
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
& $py -I (Join-Path $root "tools\patch_manifest.py") (Join-Path $dexIn "AndroidManifest.xml") (Join-Path $dexOut "AndroidManifest.xml") 3.0
if ($LASTEXITCODE) { exit $LASTEXITCODE }

$unsigned = Join-Path $work "unsigned.apk"
$aligned = Join-Path $work "aligned.apk"
& $py -I (Join-Path $root "tools\repack.py") $originalApk $stripped $gameLib $unsigned $Abi $dexOut
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
    Write-Host "Clé de signature créée : work\acp64.keystore (mot de passe dans work\keystore.properties)."
    Write-Host ("Garde ces deux fichiers : pour mettre le jeu à jour sur le téléphone, le nouvel APK doit être signé " +
        "par la même clé (sinon il faut désinstaller le jeu, ce qui efface la sauvegarde).") -ForegroundColor Yellow
}
$pw = ((Get-Content $props) -replace "^password=", "").Trim()
$final = Join-Path $out $(if ($Abi -eq "x86_64") { "AC_Pirates_emulator_x86_64.apk" } else { "AC_Pirates_arm64.apk" })
& java -jar "$bt\lib\apksigner.jar" sign --ks $ks --ks-key-alias acp64 --ks-pass "pass:$pw" --key-pass "pass:$pw" --out $final $aligned
if ($LASTEXITCODE) { exit $LASTEXITCODE }
$mb = "{0:N1}" -f ((Get-Item $final).Length / 1e6)
Write-Host ""
Write-Host "APK prêt : $final ($mb Mo)" -ForegroundColor Green
if ($Abi -ne "x86_64") {
    Write-Host "Installation : .\installer_telephone.bat (téléphone connecté en ADB), ou README.md, partie « Installer »."
}
