# Assassin's Creed Pirates sur téléphone ARM64 uniquement

Le jeu (2.9.1) n'existe qu'en ARM 32 bits. Ce projet remplace sa bibliothèque native
`libSparkApp.so` par un runtime ARM64 qui charge le code ARM32 d'origine et l'exécute avec
**Dynarmic** (traduction ARM32 → ARM64/x86-64 à la volée). Le reste de l'APK (Java, ressources, OBB)
est conservé.

## Organisation

- `src/mem.*` : espace d'adressage 32 bits du jeu (identité dans les 4 premiers Go du processus), tas dlmalloc.
- `src/elf_loader.*` : chargement et relocation de la bibliothèque ARM32.
- `src/cpu.*`, `src/thunk.h` : contextes Dynarmic par thread, appels hôte ↔ jeu, passerelles (`svc`).
- `src/hle/` : libc/pthread/stdio/réseau/temps… réimplémentés avec l'ABI bionic 32 bits.
- `src/jni/` : JNIEnv/JavaVM 32 bits ; `natives_gen.cpp` est généré par `tools/gen_natives.py`.
- `src/android/` : NativeActivity, libandroid, EGL/GLES 2, OpenSL ES, fenêtre de saisie des codes (`cheat_ui.*`).
- `src/cheat/` : code de triche « ressources infinies » (voir plus bas).
- `tools/patch_dex.py` : corrige un bug Java d'origine (`AlarmService.SafeParseInt` récursif).
- `tools/patch_manifest.py` : plein écran jusqu'au format 3:1, gestion de tous les changements de configuration par le jeu (sinon Android recrée l'activité et le moteur se bloque : pliage, taille d'affichage, mode sombre…) et `targetSdkVersion` 24 (Android 15 refuse les applications ciblant moins).
- `tools/java/` : fenêtre de saisie des codes, compilée en `classes3.dex` par `package.ps1` ; `tools/lua/` : scripts Lua du code de triche et de mise au point ; `tools/java_test/` : outil de test (touchers multi-doigts depuis adb).

## Construire

```
.\package.ps1                 # APK ARM64 -> out\AC_Pirates_arm64.apk
.\package.ps1 -Abi x86_64     # APK de test pour l'émulateur Android x86_64
```

Prérequis : Android SDK avec le NDK 27.3.13750724 et CMake 3.22.1 (`build.ps1`), JDK (javac, keytool), Python 3,
et les en-têtes Boost (1.57 ou plus, utilisés par Dynarmic) dans `third_partyoostoost\`.

Les fichiers du jeu ne sont pas dans ce dépôt (contenu Ubisoft) : il faut son propre XAPK 2.9.1, d'où viennent
`work\original.apk` (l'APK principal), `work\libSparkApp_arm32.so` (`lib/armeabi-v7a/libSparkApp.so` de cet APK)
et l'OBB. La clé de signature (`workcp64.keystore`) est créée au premier `package.ps1`.

## Installer

L'APK s'installe en l'ouvrant sur le téléphone (ou `adb install -r`), puis il faut copier l'OBB dans
`Android/obb/com.ubisoft.assassin.pirates/`. `out\installer_telephone.bat` fait les deux par ADB.

## Diagnostic

`adb shell setprop debug.acp64.verbose 1` active les journaux détaillés (tag `ACP64`).

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
