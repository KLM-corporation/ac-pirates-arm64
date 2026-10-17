@echo off
rem Installe l'APK construit par package.ps1 sur le telephone connecte en ADB (USB ou debogage sans fil),
rem puis copie les donnees du jeu (fichier OBB de work\obb) dans Android/obb sur le telephone.
rem   installer_telephone.bat [numero de serie ADB, si plusieurs appareils sont connectes]
setlocal
set PKG=com.ubisoft.assassin.pirates
set APK=%~dp0out\AC_Pirates_arm64.apk
if not "%~1"=="" set ANDROID_SERIAL=%~1

where adb >nul 2>&1 || (echo adb introuvable : installe les "platform-tools" du SDK Android et ajoute leur dossier au PATH. & exit /b 1)
if not exist "%APK%" (echo %APK% manque : lance d'abord package.ps1. & exit /b 1)
set OBB=
for %%f in ("%~dp0work\obb\main.*.%PKG%.obb") do set "OBB=%%~ff"
if not defined OBB (echo Pas de fichier OBB dans work\obb : lance package.ps1 -Xapk avec le XAPK du jeu, ou copie l'OBB a la main. & exit /b 1)

adb get-state >nul 2>&1 || (echo Aucun telephone pret, ou plusieurs : voir "adb devices" et donner le numero de serie. & exit /b 1)

echo Installation de l'APK...
adb install -r "%APK%"
if errorlevel 1 goto install_failed

echo Copie des donnees du jeu (environ 800 Mo)...
adb shell mkdir -p /sdcard/Android/obb/%PKG%
adb push "%OBB%" /sdcard/Android/obb/%PKG%/
if errorlevel 1 exit /b 1

echo Termine. Lance "Pirates" sur le telephone.
exit /b 0

:install_failed
echo.
echo L'installation a echoue. Si le message parle de signature (INSTALL_FAILED_UPDATE_INCOMPATIBLE),
echo une autre version du jeu est deja installee (le jeu d'origine, ou un APK signe par une autre cle) :
echo desinstalle-la d'abord, ce qui efface la sauvegarde :   adb uninstall %PKG%
exit /b 1
