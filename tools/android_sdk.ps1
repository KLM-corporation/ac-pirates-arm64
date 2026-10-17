# Finds the tools that build.ps1 and package.ps1 need (dot-sourced by both), with a clear message when one is
# missing. Tested versions: NDK 27.3.13750724, CMake 3.22.1, build-tools 36.1.0, platform android-34. When the
# tested version is not installed, the newest installed one is used, with a warning.

$AcpSdkPackages = '"ndk;27.3.13750724" "cmake;3.22.1" "build-tools;36.1.0" "platforms;android-34" "platform-tools"'

function Stop-Acp([string]$Message) {
    Write-Host "ERREUR : $Message" -ForegroundColor Red
    exit 1
}

function ConvertTo-AcpVersion([string]$Name) {
    $v = ($Name -replace '^android-', '') -replace '[^0-9.]', ''
    if ($v -notmatch '\.') { $v += ".0" }
    try { return [version]$v } catch { return [version]"0.0" }
}

function Find-AcpSdk {
    foreach ($d in @($env:ANDROID_HOME, $env:ANDROID_SDK_ROOT, (Join-Path $env:LOCALAPPDATA "Android\Sdk"))) {
        if ($d -and (Test-Path $d)) { return (Resolve-Path $d).Path }
    }
    Stop-Acp ("SDK Android introuvable (ni ANDROID_HOME, ni %LOCALAPPDATA%\Android\Sdk). Installe Android Studio, " +
        "ou les outils en ligne de commande, puis : sdkmanager $AcpSdkPackages")
}

# <sdk>\<Kind>\<Wanted> if installed, else the newest <sdk>\<Kind>\* (warning), else stop.
function Select-AcpPackage([string]$Sdk, [string]$Kind, [string]$Wanted, [string]$Package) {
    $dir = Join-Path $Sdk $Kind
    $exact = Join-Path $dir $Wanted
    if (Test-Path $exact) { return $exact }
    $all = @(Get-ChildItem $dir -Directory -ErrorAction SilentlyContinue |
        Sort-Object { ConvertTo-AcpVersion $_.Name } -Descending)
    if ($all.Count -gt 0) {
        Write-Warning "$Kind $Wanted absent du SDK : utilisation de $($all[0].Name) (version non testée)."
        return $all[0].FullName
    }
    Stop-Acp "$Kind introuvable dans $Sdk. Installe-le : sdkmanager `"$Package`""
}

# NDK and CMake (build.ps1).
function Get-AcpNdkTools {
    $sdk = Find-AcpSdk
    $ndk = Select-AcpPackage $sdk "ndk" "27.3.13750724" "ndk;27.3.13750724"
    $cmake = Select-AcpPackage $sdk "cmake" "3.22.1" "cmake;3.22.1"
    return @{
        Sdk = $sdk
        Ndk = $ndk
        CmakeBin = Join-Path $cmake "bin"
        Strip = Join-Path $ndk "toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-strip.exe"
    }
}

# build-tools, android.jar, JDK and Python (package.ps1).
function Get-AcpPackagingTools {
    $sdk = Find-AcpSdk
    $bt = Select-AcpPackage $sdk "build-tools" "36.1.0" "build-tools;36.1.0"
    $platform = Select-AcpPackage $sdk "platforms" "android-34" "platforms;android-34"

    # JDK: javac on the PATH, else JAVA_HOME, else the one bundled with Android Studio.
    if (-not (Get-Command javac -ErrorAction SilentlyContinue)) {
        $jdkBins = @()
        if ($env:JAVA_HOME) { $jdkBins += Join-Path $env:JAVA_HOME "bin" }
        $jdkBins += Join-Path $env:ProgramFiles "Android\Android Studio\jbr\bin"
        $found = $jdkBins | Where-Object { Test-Path (Join-Path $_ "javac.exe") } | Select-Object -First 1
        if (-not $found) { Stop-Acp "JDK introuvable (javac). Installe un JDK 17 ou plus récent, ou Android Studio." }
        $env:PATH = "$found;$env:PATH"
    }

    # Python 3: "python" can be the Microsoft Store stub, so ask the interpreter for its own path.
    $python = $null
    foreach ($cmd in @(@("python"), @("py", "-3"))) {
        if (-not (Get-Command $cmd[0] -ErrorAction SilentlyContinue)) { continue }
        $args1 = @($cmd | Select-Object -Skip 1) + @("-c", "import sys; print(sys.executable if sys.version_info >= (3, 8) else '')")
        # Read the whole output: stopping the pipeline early (Select-Object -First) kills the process, and
        # $LASTEXITCODE is then -1.
        $lines = @(& $cmd[0] $args1 2>$null)
        if ($LASTEXITCODE -ne 0 -or $lines.Count -eq 0) { continue }
        $exe = "$($lines[0])".Trim()
        if ($exe -and (Test-Path $exe)) { $python = $exe; break }
    }
    if (-not $python) { Stop-Acp "Python 3.8 ou plus récent introuvable. Installe-le depuis python.org (case `"Add to PATH`")." }

    return @{
        Sdk = $sdk
        BuildTools = $bt
        AndroidJar = Join-Path $platform "android.jar"
        Python = $python
    }
}
