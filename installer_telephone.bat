@echo off
rem Installe l'APK construit par package.ps1 sur le telephone connecte en ADB (USB ou debogage sans fil),
rem puis copie les donnees du jeu (fichier OBB de work\obb) dans Android/obb sur le telephone.
rem   installer_telephone.bat [numero de serie ADB, si plusieurs appareils sont connectes]
setlocal
set "PKG=com.ubisoft.assassin.pirates"
set "APK=%~dp0out\AC_Pirates_arm64.apk"
if not "%~1"=="" set "ANDROID_SERIAL=%~1"

rem adb : dans le PATH, sinon dans le SDK Android (memes emplacements que package.ps1).
set "ADB="
for %%a in (adb.exe) do set "ADB=%%~$PATH:a"
if not defined ADB if defined ANDROID_HOME if exist "%ANDROID_HOME%\platform-tools\adb.exe" set "ADB=%ANDROID_HOME%\platform-tools\adb.exe"
if not defined ADB if defined ANDROID_SDK_ROOT if exist "%ANDROID_SDK_ROOT%\platform-tools\adb.exe" set "ADB=%ANDROID_SDK_ROOT%\platform-tools\adb.exe"
if not defined ADB if exist "%LOCALAPPDATA%\Android\Sdk\platform-tools\adb.exe" set "ADB=%LOCALAPPDATA%\Android\Sdk\platform-tools\adb.exe"
if not defined ADB (echo adb introuvable : installe les Platform-Tools du SDK Android. & exit /b 1)

if not exist "%APK%" (echo out\AC_Pirates_arm64.apk manque : lance d'abord package.ps1. & exit /b 1)
set "OBB="
for %%f in ("%~dp0work\obb\main.*.%PKG%.obb") do set "OBB=%%~ff" & set "OBBNAME=%%~nxf" & set "OBBSIZE=%%~zf"
if not defined OBB (echo Pas de fichier OBB dans work\obb : lance package.ps1 -Xapk avec le XAPK du jeu, ou copie l'OBB a la main. & exit /b 1)
set "DIR=/sdcard/Android/obb/%PKG%"
set "DEST=/sdcard/Android/obb/%PKG%/%OBBNAME%"
rem Dossier de copie, a cote de celui du jeu : voir plus bas.
set "TMPDIR=/sdcard/Android/obb/acp64-copie"
set "TMP=/sdcard/Android/obb/acp64-copie/%OBBNAME%"

"%ADB%" get-state >nul 2>&1 || (echo Aucun telephone pret, ou plusieurs : voir "adb devices" et donner le numero de serie. & exit /b 1)

echo Installation de l'APK. Si le telephone demande une confirmation, accepte-la.
rem Dans le profil principal seulement (utilisateur 0), celui ou l'OBB est copie : sans --user, adb installe aussi
rem le jeu dans l'Espace prive ou le profil professionnel, qui ont leur propre stockage et n'y verraient pas l'OBB.
"%ADB%" install -r --user 0 "%APK%"
if errorlevel 1 goto install_failed

rem Au lancement, si son OBB n'est pas complet, le jeu efface tous les fichiers de son dossier OBB qui ne sont pas
rem cet OBB complet (SparkActivity.downloadOBB), puis affiche un ecran de telechargement qui n'aboutit jamais.
"%ADB%" shell am force-stop %PKG%

call :obb_on_phone
if not errorlevel 1 (
    echo Les donnees du jeu sont deja sur le telephone.
    goto done
)

echo.
echo Copie des donnees du jeu, %OBBNAME%, environ 800 Mo. N'ouvre pas le jeu avant la fin.
rem L'OBB est copie dans un dossier que le jeu ne regarde pas, puis deplace d'un coup dans le sien (simple
rem renommage, sur le meme stockage) : meme ouvert pendant la copie, le jeu n'a rien a effacer.
"%ADB%" shell mkdir -p "%DIR%" "%TMPDIR%"
"%ADB%" shell rm -f "%TMP%"
"%ADB%" push "%OBB%" "%TMP%"
if errorlevel 1 goto copy_failed
"%ADB%" shell mv -f "%TMP%" "%DEST%"
if errorlevel 1 goto copy_failed
"%ADB%" shell rmdir "%TMPDIR%" >nul 2>&1
call :obb_on_phone
if errorlevel 1 goto copy_failed
rem Si le jeu a quand meme ete ouvert pendant la copie, il est reste sur l'ecran de telechargement.
"%ADB%" shell am force-stop %PKG%

:done
echo Termine. Lance "Pirates" sur le telephone.
exit /b 0

rem Code de retour 0 si l'OBB est sur le telephone avec la bonne taille. La comparaison se fait sur le telephone ;
rem la commande est entre guillemets pour que cmd laisse les redirections au shell du telephone.
:obb_on_phone
"%ADB%" shell "[ $(stat -c %%s %DEST% 2>/dev/null || echo 0) = %OBBSIZE% ]" 2>nul
exit /b %errorlevel%

:install_failed
echo.
echo L'installation a echoue. Si le message parle de signature, INSTALL_FAILED_UPDATE_INCOMPATIBLE,
echo une autre version du jeu est deja installee : le jeu d'origine, ou un APK signe par une autre cle.
echo Desinstalle-la d'abord, ce qui efface la sauvegarde :   adb uninstall %PKG%
exit /b 1

:copy_failed
echo.
echo La copie des donnees du jeu a echoue. Relance le script : l'installation est rapide, puis la copie reprend.
"%ADB%" shell rm -rf "%TMPDIR%" >nul 2>&1
exit /b 1
