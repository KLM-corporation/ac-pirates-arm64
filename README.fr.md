[English](README.md) | **Français**

# Assassin's Creed Pirates sur téléphone ARM64 uniquement

Assassin's Creed Pirates (Ubisoft, version 2.9.1) n'existe qu'en ARM 32 bits : les téléphones récents qui ne
savent plus lancer d'applications 32 bits (Snapdragon 8 Gen 3, 8 Elite, Pixel 7 et plus récents…) refusent de
l'installer. Ce projet construit, à partir de ta propre copie du jeu, un APK qui s'y installe : la bibliothèque native
du jeu (`libSparkApp.so`) est remplacée par un runtime ARM64 qui charge le code ARM32 d'origine et le traduit à la
volée avec **Dynarmic**. Le reste de l'APK (Java, ressources, données OBB) est celui du jeu.

Le jeu solo fonctionne en entier, en plein écran et à 60 images/s en 3D. Les fonctions en ligne (Facebook,
Google Play Jeux, « Plus de jeux », achats intégrés) ne marchent plus : les serveurs d'Ubisoft sont fermés. En plus :
codes de triche (ressources infinies, le Queen Anne's Revenge que le jeu vendait en achat intégré), voir plus bas.

**Ce dépôt ne contient aucun fichier du jeu** (code, images, sons, données : tout appartient à Ubisoft). Il faut
avoir le jeu. Pour la même raison, ne publie pas l'APK construit : il contient les fichiers du jeu.

## Ce qu'il faut

- **Le jeu, version 2.9.1** (code de version 253), au choix :
  - un fichier `.xapk`, qui contient l'APK et les données ;
  - ou l'APK du jeu et son fichier de données `main.253.com.ubisoft.assassin.pirates.obb` (817 Mo), par exemple
    récupérés sur un ancien téléphone où le jeu est installé : `adb shell pm path com.ubisoft.assassin.pirates` donne
    le chemin de l'APK, à récupérer avec `adb pull <chemin> jeu.apk` ; l'OBB est dans
    `Android/obb/com.ubisoft.assassin.pirates/`.

  `package.ps1` vérifie que c'est bien la version testée : la bibliothèque du jeu, `lib/armeabi-v7a/libSparkApp.so`,
  doit avoir le SHA-256 `1dc7f264f7591193ddf8a234250f3919511f3dda16b5c383de9b66c17cc291a7`. Une autre version
  ne marchera probablement pas.
- **Un téléphone Android 64 bits (ARM64)**, Android 10 ou plus récent, avec 1,2 Go de libre. Testé sur un REDMAGIC
  NX799J (Snapdragon 8 Elite, Android 16). Les téléphones qui lancent encore le 32 bits peuvent aussi l'utiliser.
- **Un PC Windows 10 ou 11** avec 2 Go de libre et :
  - [Git](https://git-scm.com/download/win) ;
  - le **SDK Android** avec ces paquets : NDK 27.3.13750724, CMake 3.22.1, Build-Tools 36.1.0, Platform 34 et
    Platform-Tools. Avec [Android Studio](https://developer.android.com/studio) : *Tools > SDK Manager*, onglets
    *SDK Platforms* et *SDK Tools*, case *Show Package Details* pour choisir les versions. Avec les
    [outils en ligne de commande](https://developer.android.com/studio#command-line-tools-only) :
    `sdkmanager "ndk;27.3.13750724" "cmake;3.22.1" "build-tools;36.1.0" "platforms;android-34" "platform-tools"`.
    Le SDK est cherché dans `ANDROID_HOME`, `ANDROID_SDK_ROOT`, puis `%LOCALAPPDATA%\Android\Sdk` (l'emplacement
    d'Android Studio). Si une autre version d'un paquet est installée, elle est utilisée, avec un avertissement ;
  - un **JDK** 17 ou plus récent (celui d'Android Studio est trouvé tout seul) ;
  - **Python** 3.8 ou plus récent ([python.org](https://www.python.org/downloads/), cocher *Add python.exe to PATH*).

  Rien d'autre à télécharger : Dynarmic et les en-têtes Boost dont il a besoin sont dans `third_party/`.

## Construire l'APK

Dans PowerShell :

```powershell
git clone https://github.com/KLM-corporation/ac-pirates-arm64 C:\acp64
cd C:\acp64
powershell -ExecutionPolicy Bypass -File .\package.ps1 -Xapk "C:\chemin\vers\le\jeu.xapk"
```

- Avec l'APK et l'OBB séparés : `-Xapk "C:\...\jeu.apk" -Obb "C:\...\main.253.com.ubisoft.assassin.pirates.obb"`.
- `-ExecutionPolicy Bypass` lève, pour cette commande seulement, le blocage des scripts PowerShell de Windows. Pour
  ne plus avoir à l'écrire : `Set-ExecutionPolicy -Scope CurrentUser RemoteSigned`, puis `.\package.ps1 ...`.
- Clone le dépôt dans un dossier court (comme `C:\acp64`) : la compilation crée des chemins longs, et Windows
  refuse ceux de plus de 260 caractères.
- La première compilation prend de 2 à 10 minutes selon le PC. Résultat : `out\AC_Pirates_arm64.apk` (29 Mo). Les
  lignes `WARNING:` de Java pendant la signature sont normales.
- Les fichiers du jeu sont copiés dans `work\` (`original.apk`, `libSparkApp_arm32.so`, `obb\`). Les fois suivantes,
  `.\package.ps1` suffit, sans `-Xapk`.
- **La clé de signature** est créée au premier passage : `work\acp64.keystore`, avec son mot de passe dans
  `work\keystore.properties`. Garde ces deux fichiers, et une copie de sauvegarde. Android n'accepte une mise à jour
  que si elle est signée par la même clé : avec une autre clé, il faudrait désinstaller le jeu, ce qui efface la
  sauvegarde. Si tu construis le jeu depuis un autre dossier ou un autre PC, copie d'abord ces deux fichiers dans son
  `work\`.
- Les scripts affichent leurs messages en français si Windows est en français, en anglais sinon. Pour choisir :
  `$env:ACP64_LANG = "fr"` (ou `"en"`) dans PowerShell avant de les lancer.

## Installer sur le téléphone

Il faut installer l'APK **et** copier les données du jeu (l'OBB) au bon endroit.

**Avec ADB** (débogage USB ou sans fil activé dans les options pour les développeurs du téléphone) :

```powershell
.\installer_telephone.bat
```

Le script installe l'APK dans le profil principal du téléphone, puis copie l'OBB de `work\obb` dans
`Android/obb/com.ubisoft.assassin.pirates/` : environ 800 Mo, de 30 secondes en USB à quelques minutes en Wi-Fi.
Attends le message « Termine » avant d'ouvrir le jeu. Ouvert pendant la copie, le jeu affiche un écran de
téléchargement qui n'aboutit jamais ; il suffit alors de le relancer après « Termine ».

Au lancement, si son OBB n'est pas complet, le jeu efface tous les fichiers de son dossier OBB qui ne sont pas cet
OBB complet (`SparkActivity.downloadOBB`). Le script copie donc l'OBB dans un dossier à part
(`Android/obb/acp64-copie`), puis le déplace d'un coup dans celui du jeu : même ouvert trop tôt, le jeu n'a rien à
effacer. Le script ne recopie pas un OBB déjà complet sur le téléphone : relancé après une mise à jour, il ne prend
que quelques secondes.

`adb` est cherché dans le `PATH`, puis dans le dossier `platform-tools` du SDK Android. Si plusieurs appareils sont
connectés, donne le numéro de série affiché par `adb devices` : `.\installer_telephone.bat <numéro>`.

**À la main** :

1. Copie `out\AC_Pirates_arm64.apk` sur le téléphone et ouvre-le. Autorise l'installation d'applications inconnues
   si Android le demande. Si Play Protect avertit : *Plus d'infos > Installer quand même*.
2. Branche le téléphone au PC en mode *Transfert de fichiers*. Sur le PC, ouvre *Mémoire interne > Android > obb*,
   crée un dossier nommé exactement `com.ubisoft.assassin.pirates` et copie dedans
   `main.253.com.ubisoft.assassin.pirates.obb` (il est dans `work\obb\`). Les gestionnaires de fichiers du téléphone
   n'ont souvent plus le droit d'écrire dans `Android/obb` (Android 11 et plus) ; le PC, si.
3. **Une fois la copie terminée**, lance « Pirates ». Si le jeu est ouvert pendant la copie, il efface le fichier en
   cours de copie : il faut alors le fermer et recommencer l'étape 2. Android prévient que l'application a été conçue
   pour une ancienne version : *OK*. Accepte les autorisations demandées.

## Problèmes connus

- **Écran de téléchargement qui n'aboutit jamais au démarrage** : l'OBB manque, est incomplet (jeu ouvert pendant
  la copie), ou n'a pas le bon nom ou le bon dossier
  (`Android/obb/com.ubisoft.assassin.pirates/main.253.com.ubisoft.assassin.pirates.obb`, 817 249 909 octets).
  Ferme le jeu et relance `installer_telephone.bat` : il recopie l'OBB s'il n'a pas la bonne taille. Le jeu doit
  aussi être installé dans le profil principal du téléphone : l'Espace privé et le profil professionnel ont leur
  propre stockage, où il ne voit pas l'OBB copié par le PC.
- **« Application non installée »** ou `INSTALL_FAILED_UPDATE_INCOMPATIBLE` : une autre version du jeu est déjà
  installée (le jeu d'origine, ou un APK signé par une autre clé). Désinstalle-la d'abord (la sauvegarde est perdue).
- **Le bouton JOUER reste grisé** au premier écran : le jeu charge la partie en arrière-plan, jusqu'à une minute.
- **Le jeu plante** : `adb shell setprop debug.acp64.verbose 1`, relance le jeu, puis `adb logcat -s ACP64 DEBUG`
  montre ce que fait le runtime (voir « Diagnostic »).

## Organisation

- `package.ps1` : construit l'APK (runtime, Java, manifeste, signature) ; `build.ps1` : compile le runtime seul ;
  `tools/android_sdk.ps1` : recherche des outils du SDK ; `tools/import_game.py` : importe le XAPK ou l'APK dans `work\` ;
  `installer_telephone.bat` : installation par ADB.
- `src/mem.*` : espace d'adressage 32 bits du jeu (identité dans les 4 premiers Go du processus), tas dlmalloc.
- `src/elf_loader.*` : chargement et relocation de la bibliothèque ARM32.
- `src/cpu.*`, `src/thunk.h` : contextes Dynarmic par thread, appels hôte ↔ jeu, passerelles (`svc`).
- `src/hle/` : libc/pthread/stdio/réseau/temps… réimplémentés avec l'ABI bionic 32 bits.
- `src/jni/` : JNIEnv/JavaVM 32 bits ; `natives_gen.cpp` est généré par `tools/gen_natives.py` (qui demande `pyelftools`).
- `src/android/` : NativeActivity, libandroid, EGL/GLES 2, OpenSL ES, fenêtre de saisie des codes (`cheat_ui.*`).
- `src/cheat/` : codes de triche (voir plus bas) ; `src/game/perf_patches.cpp` : corrections de performances.
- `tools/repack.py` : reconstruit l'APK ; `tools/patch_dex.py` : corrige un bug Java d'origine
  (`AlarmService.SafeParseInt` récursif).
- `tools/patch_manifest.py` : plein écran jusqu'au format 3:1, gestion de tous les changements de configuration par le jeu (sinon Android recrée l'activité et le moteur se bloque : pliage, taille d'affichage, mode sombre…) et `targetSdkVersion` 24 (Android 15 refuse les applications ciblant moins).
- `tools/java/` : fenêtre de saisie des codes, compilée en `classes3.dex` par `package.ps1` ; `tools/lua/` : scripts Lua du code de triche et de mise au point ; `tools/java_test/` : outil de test (touchers multi-doigts depuis adb) ; les scripts `tools/*.sh` (Git Bash) servent aux mesures sur le téléphone.
- `third_party/` : Dynarmic, en-têtes Boost, dlmalloc (origines et licences dans `third_party/README.md`).
- La fenêtre des codes de triche (`tools/java/com/acp64/CheatDialog.java`) est en français si le téléphone est en
  français, en anglais sinon.

Pour un émulateur Android x86_64 (tests) : `.\package.ps1 -Abi x86_64` donne `out\AC_Pirates_emulator_x86_64.apk`.

## Diagnostic

`adb shell setprop debug.acp64.verbose 1` active les journaux détaillés (tag `ACP64`, à lire avec
`adb logcat -s ACP64`) ; `adb shell setprop debug.acp64.verbose 0` les coupe (ils ralentissent le jeu). Le
réglage est relu au lancement du jeu et disparaît au redémarrage du téléphone.

## Code de triche (ressources infinies)

La séquence secrète **Volume + , Volume + , Volume − , Volume −** (en moins de 4 s, en jeu) ouvre une fenêtre
« Code de triche » ; `klm` (majuscules ou minuscules) active ou coupe les ressources infinies : l'or, le bois,
les vivres et les livres du joueur sont remplis jusqu'aux limites du jeu (9 999 999 pour l'or, 9 999 pour le
reste) quelques fois par seconde. Le réglage est mémorisé entre deux lancements (fichier `infinite_resources`
dans le dossier privé de l'application). La fenêtre ne s'ouvre jamais sous un doigt posé sur l'écran.

Un geste tactile avait été essayé d'abord : il se déclenchait par erreur en jouant (volant + canons aux deux
pouces) et les appuis à trois doigts ou plus sont avalés par certains téléphones (nubia, REDMAGIC… : capture
d'écran à trois doigts, le jeu ne reçoit qu'une annulation). Les touches de volume n'interfèrent pas avec le
tactile. Chaque appui peut arriver deux fois au jeu à une image d'intervalle : `cheat_ui.cpp` ignore le second.

### Code `queen` : le Queen Anne's Revenge

`queen` (même fenêtre) débloque le Queen Anne's Revenge, le navire (identifiant `FRIGATE4`, toutes les
caractéristiques et le modèle 3D sont dans les données) que le jeu vendait en achat intégré (`ubi.mobilesdk.pirates.blackbeard`)
et qui ne peut plus s'acheter. `tools/lua/unlock_queen.lua` appelle `playerProfile:BuyShip("FRIGATE4")`, la fonction
que la boutique du jeu appelle une fois un navire payé ; il apparaît ensuite dans NAVIRES (bouton CHOISIR).

Le jeu remplace les données de navires du profil quand il charge la sauvegarde (et à d'autres moments) : le
runtime vérifie donc la possession environ une fois par seconde, et recommence l'octroi si besoin. Rien n'est accordé au profil par défaut du menu principal (`_SessionNumber` à 0 et `_IsInMainMenu` vrai), qui est remplacé au chargement. Le code est
mémorisé (fichier `queen_anne` dans le dossier privé de l'application) : le bateau est de nouveau là à chaque lancement,
sans retaper le code. Tapé avant que la partie soit chargée, le code est accepté (« sera débloqué dès que ta partie est
chargée ») et un second message « débloqué » arrive plus tard (`CheatDialog.notifyEvent`, appelé par le runtime).

Fonctionnement commun aux deux codes :

- La logique du jeu (économie comprise) est du LuaJIT et la bibliothèque exporte toute l'API C de Lua.
  `src/cheat/cheat.cpp` remplace la première instruction de `lua_pcall` par un `svc`
  (`cpu::install_thumb_entry_hook`) : à chaque appel, le runtime peut exécuter un morceau de Lua sur le fil
  qui fait tourner les scripts, entre deux appels du jeu, donc sans accès concurrent à l'état Lua.
- `tools/lua/cheat_infinite.lua` (intégré à la compilation par CMake) écrit dans `playerProfile`
  (`SoftCurrency`, `Wood`, `Supplies`, `Books`) en passant par les propriétés du jeu, qui bornent les valeurs
  et préviennent l'interface.
- `tools/java/com/acp64/CheatDialog.java` est la fenêtre de saisie ; elle appelle
  `Java_com_acp64_CheatDialog_nativeSubmit` (`src/android/cheat_ui.cpp`) qui vérifie le code.

## Fenêtre détruite pendant que le jeu l'interroge

Quand le jeu passe en arrière-plan (accueil, page « Plus de jeux », verrouillage…), Android détruit la fenêtre native,
mais le moteur continue un instant à lui demander sa taille avec l'ancien identifiant (et avant que la nouvelle existe).
`ANativeWindow_getWidth(nullptr)` plantait le jeu en segfault (adresse 0x90). Les fonctions `ANativeWindow_*` de
`android_glue.cpp` répondent maintenant avec les dernières valeurs connues quand l'identifiant n'existe plus
(`PtrMap::find_host`, sans journal), et `eglCreateWindowSurface` renvoie `EGL_NO_SURFACE` plutôt que de planter.

## Touchers annulés par le système

Quand Android retire un toucher à l'application (`ACTION_CANCEL` : capture d'écran à trois doigts, fenêtre qui
prend le focus…), le jeu ne relâche jamais ces doigts : son tactile reste bloqué (le volant ne répond plus, plus
aucun contour blanc) jusqu'au redémarrage. `android_glue.cpp` remplace donc l'annulation, pour le jeu, par le
relâchement de chaque doigt listé (`POINTER_UP` puis `UP`, `LiftEvent`), et réveille la boucle d'entrée du jeu
pour qu'elle les lise.

## Performances : 60 images/s en 3D

Mesures sur le REDMAGIC NX799J (Snapdragon 8 Elite), écran à 60 Hz pour le jeu, images réellement affichées
(`tools/fps_sample.py`, horodatages de SurfaceFlinger) :

| Version | Navigation en mer (3D) | Thread du jeu |
|---|---|---|
| Avant (jeu d'origine plafonné à 30) | 30 images/s ; forcé à 60 : ~52 images/s | 95 % (à 60 Hz) |
| Après | **59,9 images/s, aucune image en retard** (43 s de navigation, 60 s avec la pêche) | ~40 % |

Le jeu passe aussi les scènes 3D à 60 (`GP.PARAMS.FpsClampingTarget = 60`, et `TargetElapsedTime = 1/60` sur la
boucle de jeu, `fps60` dans `src/game/perf_patches.cpp`). Son horloge suit le temps réel (la globale Lua `Dt` est la
durée mesurée de l'image ; `AbsoluteTime` avance de 12,05 s pour 12,05 s réelles), donc 60 ne change pas la vitesse
du jeu. Le moteur prévoit d'ailleurs 60 par défaut (`GameLoop.TargetElapsedTime = 1/60`) ; les téléphones de
l'époque étaient réglés à 30.

Ce que le profilage a montré (thread du jeu, avant) et les corrections (`src/game/perf_patches.cpp`, chacune
désactivable avec `adb shell setprop debug.acp64.<nom> 0` pour comparer) :

- **Ramasse-miettes Lua (~50 % du thread du jeu)** : le moteur fait tourner le GC de LuaJIT sur un thread annexe
  pendant le rendu (`StartAsyncGarbageCollection` / `StopAsyncGarbageCollection`), mais `lua_asynchronous_gc_step`
  force un pas même quand le GC est au repos : un nouveau cycle complet démarre dès que le précédent finit, et les
  allocations du thread du jeu, qui trouvent toujours le GC en plein cycle, paient elles aussi des pas
  (`lj_gc_step`). `gcpace` : le thread annexe ne démarre un cycle que quand il est dû (le tas a grandi du rapport
  `pause`, comme dans LuaJIT) ; `gcdefer` : les pas du thread du jeu sont laissés au thread annexe tant que le tas
  reste sous 1,5 fois le seuil normal. Le tas oscille entre ~45 et ~90 Mo.
- **Compilateur LuaJIT (~1,5 ms par image)** : LuaJIT 2.0 doit placer son code machine à moins de 32 Mo de sa VM,
  au milieu de cette bibliothèque de 19 Mo : il n'y arrive jamais, enregistre une trace, échoue à la placer
  (32 essais de `mmap`/`munmap`) et recommence, ~800 fois par seconde. `luajit_off` coupe le compilateur au démarrage
  (`jit.off()`) : l'interpréteur seul va plus vite.
- **Attente active** : `IdleCurThread` (le « sommeil » du moteur, utilisé par `android_main` toutes les 16 ms) tourne
  en boucle sur `clock_gettime` + `sched_yield` (1,2 million d'appels par seconde, un cœur occupé à 100 %). `idle` :
  vrai sommeil.
- **Accès mémoire du JIT** : `fastmem` (Dynarmic) : la mémoire invitée étant à la même adresse dans le processus, une
  lecture ou une écriture invitée devient une seule instruction au lieu d'un passage par la table des pages.
- **Attachements Java** : `SparkSystem::JNIEnvWrapper` attachait et détachait `android_main` de la VM Java toutes les
  16 ms (un nouveau `java.lang.Thread` à chaque fois). Le détachement demandé par le jeu ne détache plus vraiment ; le
  thread est détaché quand il se termine (`jni_bridge.cpp`).

Outils de mesure (avec `debug.acp64.verbose=1`) : chaque seconde, `eglSwapBuffers` journalise images/s, temps de
travail, pire image, puis les fonctions importées les plus appelées (`calls/s`) et les attentes bloquantes avec leur
appelant (`waits:`), ainsi que les images de plus de 50 ms. Pour un profil : `<profileable android:shell="true"/>`
est dans le manifeste, donc `simpleperf record --app com.ubisoft.assassin.pirates -e cpu-clock` fonctionne ; la sonde
Lua `acp64_profdump(chemin)` écrit la carte des blocs compilés de chaque thread (`chemin.t<id>`), et
`tools/prof_report.py perf.data <carte> <tid>` attribue les échantillons aux fonctions du jeu ARM32.
`tools/goto_sea.sh` amène le jeu en mer depuis un démarrage, `tools/bench.sh` moyenne les statistiques d'images.

## Mise au point

Avec `debug.acp64.verbose` à 1 (et `adb shell setprop log.tag.ACP64 V` pour voir les lignes de débogage), un
fichier `probe.lua` copié dans `/sdcard/Android/data/com.ubisoft.assassin.pirates/files/` est exécuté dans
l'interpréteur Lua du jeu à chaque modification (touche-le avant de le recopier : adb ne garde que la seconde) ;
`acp64_log(texte)` écrit dans `probe.out` du même dossier (un `!` en début de texte l'envoie aussi dans logcat).
`tools/lua/probe_*.lua` en sont des exemples (liste des globales, recherche des noms liés à l'argent, contenu
des fonctions). `tools/java_test/Inj.java` injecte des touchers multi-doigts depuis adb (`app_process`) pour
tester sans toucher l'écran.

## Licence

Le code de ce dépôt (runtime, scripts, outils) est sous licence MIT : voir `LICENSE`. Les bibliothèques de
`third_party/` gardent leur propre licence (voir `third_party/README.md`).

Projet non officiel, sans lien avec Ubisoft. Le jeu (code, données, nom et marques Assassin's Creed) appartient à
Ubisoft et n'est pas couvert par cette licence.
