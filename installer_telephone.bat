@echo off
rem Installs the APK built by package.ps1 on the phone connected through ADB (USB or wireless debugging), then
rem copies the game data (the OBB file of work\obb) into Android/obb on the phone.
rem   installer_telephone.bat [ADB serial number, when several devices are connected]
rem Messages are in French on a French Windows, in English otherwise; ACP64_LANG=fr or en forces one.
setlocal
set "PKG=com.ubisoft.assassin.pirates"
set "APK=%~dp0out\AC_Pirates_arm64.apk"
if not "%~1"=="" set "ANDROID_SERIAL=%~1"

set "FR="
if /i "%ACP64_LANG%"=="fr" set "FR=1"
rem powershell.exe and not powershell: a powershell.cmd or .bat found first on the PATH would end this script.
if not defined ACP64_LANG powershell.exe -NoProfile -Command "exit [int]((Get-UICulture).TwoLetterISOLanguageName -ne 'fr')" >nul 2>&1 && set "FR=1"

rem adb: on the PATH, else in the Android SDK (same places as package.ps1).
set "ADB="
for %%a in (adb.exe) do set "ADB=%%~$PATH:a"
if not defined ADB if defined ANDROID_HOME if exist "%ANDROID_HOME%\platform-tools\adb.exe" set "ADB=%ANDROID_HOME%\platform-tools\adb.exe"
if not defined ADB if defined ANDROID_SDK_ROOT if exist "%ANDROID_SDK_ROOT%\platform-tools\adb.exe" set "ADB=%ANDROID_SDK_ROOT%\platform-tools\adb.exe"
if not defined ADB if exist "%LOCALAPPDATA%\Android\Sdk\platform-tools\adb.exe" set "ADB=%LOCALAPPDATA%\Android\Sdk\platform-tools\adb.exe"
if not defined ADB goto no_adb

if not exist "%APK%" goto no_apk
set "OBB="
for %%f in ("%~dp0work\obb\main.*.%PKG%.obb") do set "OBB=%%~ff" & set "OBBNAME=%%~nxf" & set "OBBSIZE=%%~zf"
if not defined OBB goto no_obb
set "DIR=/sdcard/Android/obb/%PKG%"
set "DEST=/sdcard/Android/obb/%PKG%/%OBBNAME%"
rem Copy folder, next to the game's own: see below.
set "TMPDIR=/sdcard/Android/obb/acp64-copie"
set "TMP=/sdcard/Android/obb/acp64-copie/%OBBNAME%"

"%ADB%" get-state >nul 2>&1 || goto no_phone

if defined FR (echo Installation de l'APK. Si le telephone demande une confirmation, accepte-la.) else (echo Installing the APK. If the phone asks for a confirmation, accept it.)
rem In the main profile only (user 0), where the OBB is copied: without --user, adb also installs the game in the
rem Private space or the work profile, which have their own storage and would not see the OBB there.
"%ADB%" install -r --user 0 "%APK%"
if errorlevel 1 goto install_failed

rem When it starts without a complete OBB, the game deletes every file in its OBB folder other than that complete
rem OBB (SparkActivity.downloadOBB), then shows a download screen that never finishes.
"%ADB%" shell am force-stop %PKG%

call :obb_on_phone
if errorlevel 1 goto copy_obb
if defined FR (echo Les donnees du jeu sont deja sur le telephone.) else (echo The game data is already on the phone.)
goto done

:copy_obb
echo.
if defined FR (echo Copie des donnees du jeu, %OBBNAME%, environ 800 Mo. N'ouvre pas le jeu avant la fin.) else (echo Copying the game data, %OBBNAME%, about 800 MB. Do not open the game before the end.)
rem The OBB is copied into a folder the game never looks at, then moved into its own in one go (a plain rename on
rem the same storage): even if it is opened during the copy, the game has nothing to delete.
"%ADB%" shell mkdir -p "%DIR%" "%TMPDIR%"
"%ADB%" shell rm -f "%TMP%"
"%ADB%" push "%OBB%" "%TMP%"
if errorlevel 1 goto copy_failed
"%ADB%" shell mv -f "%TMP%" "%DEST%"
if errorlevel 1 goto copy_failed
"%ADB%" shell rmdir "%TMPDIR%" >nul 2>&1
call :obb_on_phone
if errorlevel 1 goto copy_failed
rem If the game was opened during the copy anyway, it is stuck on its download screen.
"%ADB%" shell am force-stop %PKG%

:done
if defined FR (echo Termine. Lance "Pirates" sur le telephone.) else (echo Done. Start "Pirates" on the phone.)
exit /b 0

rem Exit code 0 when the OBB is on the phone with the right size. The comparison runs on the phone; the command is
rem quoted so that cmd leaves the redirections to the phone's shell.
:obb_on_phone
"%ADB%" shell "[ $(stat -c %%s %DEST% 2>/dev/null || echo 0) = %OBBSIZE% ]" 2>nul
exit /b %errorlevel%

:no_adb
if defined FR (echo adb introuvable : installe les Platform-Tools du SDK Android.) else (echo adb not found: install the Android SDK Platform-Tools.)
exit /b 1

:no_apk
if defined FR (echo out\AC_Pirates_arm64.apk manque : lance d'abord package.ps1.) else (echo out\AC_Pirates_arm64.apk is missing: run package.ps1 first.)
exit /b 1

:no_obb
if defined FR (echo Pas de fichier OBB dans work\obb : lance package.ps1 -Xapk avec le XAPK du jeu, ou copie l'OBB a la main.) else (echo No OBB file in work\obb: run package.ps1 -Xapk with the game's XAPK, or copy the OBB by hand.)
exit /b 1

:no_phone
if defined FR (echo Aucun telephone pret, ou plusieurs : voir "adb devices" et donner le numero de serie.) else (echo No phone ready, or several: see "adb devices" and give the serial number.)
exit /b 1

:install_failed
echo.
if defined FR (
    echo L'installation a echoue. Si le message parle de signature, INSTALL_FAILED_UPDATE_INCOMPATIBLE,
    echo une autre version du jeu est deja installee : le jeu d'origine, ou un APK signe par une autre cle.
    echo Desinstalle-la d'abord, ce qui efface la sauvegarde :   adb uninstall %PKG%
) else (
    echo The installation failed. If the message mentions a signature, INSTALL_FAILED_UPDATE_INCOMPATIBLE,
    echo another version of the game is already installed: the original game, or an APK signed with another key.
    echo Uninstall it first, which erases the save:   adb uninstall %PKG%
)
exit /b 1

:copy_failed
echo.
if defined FR (echo La copie des donnees du jeu a echoue. Relance le script : l'installation est rapide, puis la copie reprend.) else (echo Copying the game data failed. Run the script again: the installation is quick, then the copy starts over.)
"%ADB%" shell rm -rf "%TMPDIR%" >nul 2>&1
exit /b 1
