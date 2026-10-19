**English** | [Français](README.fr.md)

# Assassin's Creed Pirates on 64-bit-only Android phones

Assassin's Creed Pirates (Ubisoft, version 2.9.1) only exists as 32-bit ARM code: recent phones that can no longer
run 32-bit apps (Snapdragon 8 Gen 3, 8 Elite, Pixel 7 and newer…) refuse to install it. This project builds, from
your own copy of the game, an APK that installs on them: the game's native library (`libSparkApp.so`) is replaced by
an ARM64 runtime that loads the original ARM32 code and translates it on the fly with **Dynarmic**. The rest of the
APK (Java code, resources, OBB data) is the game's own.

The whole single-player game works, full screen and at 60 fps in 3D. Online features (Facebook, Google Play Games,
"More games", in-app purchases) no longer work: Ubisoft's servers are shut down. Extras: cheat codes (infinite
resources, and the Queen Anne's Revenge, which the game used to sell as an in-app purchase), see below.

**This repository contains none of the game's files** (code, images, sounds, data: all of it belongs to Ubisoft). You
need to own the game. For the same reason, do not publish the APK you build: it contains the game's files.

## What you need

- **The game, version 2.9.1** (version code 253), either:
  - an `.xapk` file, which holds the APK and the data;
  - or the game's APK and its data file `main.253.com.ubisoft.assassin.pirates.obb` (817 MB), for example taken
    from an old phone where the game is installed: `adb shell pm path com.ubisoft.assassin.pirates` gives the APK's
    path, to fetch with `adb pull <path> game.apk`; the OBB is in `Android/obb/com.ubisoft.assassin.pirates/`.

  `package.ps1` checks that this is the tested version: the game's library, `lib/armeabi-v7a/libSparkApp.so`, must
  have the SHA-256 `1dc7f264f7591193ddf8a234250f3919511f3dda16b5c383de9b66c17cc291a7`. Another version will most
  likely not work.
- **A 64-bit (ARM64) Android phone**, Android 10 or newer, with 1.2 GB free. Tested on a REDMAGIC NX799J
  (Snapdragon 8 Elite, Android 16). Phones that can still run 32-bit apps can use it too.
- **A Windows 10 or 11 PC** with 2 GB free and:
  - [Git](https://git-scm.com/download/win);
  - the **Android SDK** with these packages: NDK 27.3.13750724, CMake 3.22.1, Build-Tools 36.1.0, Platform 34 and
    Platform-Tools. With [Android Studio](https://developer.android.com/studio): *Tools > SDK Manager*, *SDK
    Platforms* and *SDK Tools* tabs, tick *Show Package Details* to pick the versions. With the
    [command-line tools](https://developer.android.com/studio#command-line-tools-only):
    `sdkmanager "ndk;27.3.13750724" "cmake;3.22.1" "build-tools;36.1.0" "platforms;android-34" "platform-tools"`.
    The SDK is looked up in `ANDROID_HOME`, `ANDROID_SDK_ROOT`, then `%LOCALAPPDATA%\Android\Sdk` (where Android
    Studio puts it). If another version of a package is installed, it is used, with a warning;
  - a **JDK** 17 or newer (the one bundled with Android Studio is found automatically);
  - **Python** 3.8 or newer ([python.org](https://www.python.org/downloads/), tick *Add python.exe to PATH*).

  Nothing else to download: Dynarmic and the Boost headers it needs are in `third_party/`.

## Build the APK

In PowerShell:

```powershell
git clone https://github.com/KLM-corporation/ac-pirates-arm64 C:\acp64
cd C:\acp64
powershell -ExecutionPolicy Bypass -File .\package.ps1 -Xapk "C:\path\to\the\game.xapk"
```

- With a separate APK and OBB: `-Xapk "C:\...\game.apk" -Obb "C:\...\main.253.com.ubisoft.assassin.pirates.obb"`.
- `-ExecutionPolicy Bypass` lifts Windows' block on PowerShell scripts, for this command only. To stop typing it:
  `Set-ExecutionPolicy -Scope CurrentUser RemoteSigned`, then `.\package.ps1 ...`.
- Clone the repository into a short folder (like `C:\acp64`): the build creates long paths, and Windows rejects
  paths longer than 260 characters.
- The first build takes 2 to 10 minutes depending on the PC. Result: `out\AC_Pirates_arm64.apk` (29 MB). Java
  `WARNING:` lines during signing are normal.
- The game's files are copied into `work\` (`original.apk`, `libSparkApp_arm32.so`, `obb\`). Next time,
  `.\package.ps1` is enough, without `-Xapk`.
- **The signing key** is created on the first run: `work\acp64.keystore`, with its password in
  `work\keystore.properties`. Keep both files, and a backup. Android only accepts an update signed with the same key:
  with another key, you would have to uninstall the game, which erases the save. If you build the game from another
  folder or another PC, copy these two files into its `work\` first.
- The scripts print their messages in French on a French Windows, in English otherwise. To choose:
  `$env:ACP64_LANG = "en"` (or `"fr"`) in PowerShell before running them.

## Install on the phone

You need to install the APK **and** copy the game data (the OBB) to the right place.

**With ADB** (USB or wireless debugging enabled in the phone's developer options):

```powershell
.\installer_telephone.bat
```

The script installs the APK in the phone's main profile, then copies the OBB from `work\obb` to
`Android/obb/com.ubisoft.assassin.pirates/`: about 800 MB, from 30 seconds over USB to a few minutes over Wi-Fi.
Wait for the "Done" message before opening the game. If it is opened during the copy, the game shows a download
screen that never finishes; just start it again after "Done".

When it starts without a complete OBB, the game deletes every file in its OBB folder other than that complete OBB
(`SparkActivity.downloadOBB`). So the script copies the OBB into a separate folder (`Android/obb/acp64-copie`), then
moves it into the game's folder in one go: even if it is opened too early, the game has nothing to delete. The script
does not copy again an OBB that is already complete on the phone: run again after an update, it only takes a few
seconds.

`adb` is looked up in the `PATH`, then in the Android SDK's `platform-tools` folder. If several devices are
connected, give the serial number shown by `adb devices`: `.\installer_telephone.bat <serial>`.

**By hand**:

1. Copy `out\AC_Pirates_arm64.apk` to the phone and open it. Allow installing unknown apps if Android asks. If Play
   Protect warns you: *More details > Install anyway*.
2. Connect the phone to the PC in *File transfer* mode. On the PC, open *Internal storage > Android > obb*, create a
   folder named exactly `com.ubisoft.assassin.pirates` and copy `main.253.com.ubisoft.assassin.pirates.obb` into it
   (it is in `work\obb\`). File managers on the phone are often no longer allowed to write to `Android/obb` (Android
   11 and newer); the PC is.
3. **Once the copy is finished**, start "Pirates". If the game is opened during the copy, it deletes the file being
   copied: close it and do step 2 again. Android warns that the app was built for an older version: *OK*. Accept the
   permissions it asks for.

## Known issues

- **Download screen that never finishes at startup**: the OBB is missing, incomplete (game opened during the copy),
  or has the wrong name or folder (`Android/obb/com.ubisoft.assassin.pirates/main.253.com.ubisoft.assassin.pirates.obb`,
  817,249,909 bytes). Close the game and run `installer_telephone.bat` again: it copies the OBB again if its size is
  wrong. The game must also be installed in the phone's main profile: the Private space and the work profile have
  their own storage, where it does not see the OBB copied by the PC.
- **"App not installed"** or `INSTALL_FAILED_UPDATE_INCOMPATIBLE`: another version of the game is already installed
  (the original game, or an APK signed with another key). Uninstall it first (the save is lost).
- **The Play button stays greyed out** on the title screen: the game is loading your save in the background, for up
  to a minute.
- **The game crashes**: `adb shell setprop debug.acp64.verbose 1`, start the game again, then
  `adb logcat -s ACP64 DEBUG` shows what the runtime does (see "Diagnostics").

## Layout

- `package.ps1`: builds the APK (runtime, Java, manifest, signature); `build.ps1`: builds the runtime only;
  `tools/android_sdk.ps1`: finds the SDK tools; `tools/import_game.py`: imports the XAPK or APK into `work\`;
  `installer_telephone.bat`: installation through ADB.
- `src/mem.*`: the game's 32-bit address space (identity-mapped in the first 4 GB of the process), dlmalloc heap.
- `src/elf_loader.*`: loading and relocating the ARM32 library.
- `src/cpu.*`, `src/thunk.h`: per-thread Dynarmic contexts, host ↔ game calls, gateways (`svc`).
- `src/hle/`: libc/pthread/stdio/network/time… reimplemented with the 32-bit bionic ABI.
- `src/jni/`: 32-bit JNIEnv/JavaVM; `natives_gen.cpp` is generated by `tools/gen_natives.py` (which needs
  `pyelftools`).
- `src/android/`: NativeActivity, libandroid, EGL/GLES 2, OpenSL ES, cheat code input window (`cheat_ui.*`).
- `src/cheat/`: cheat codes (see below); `src/game/perf_patches.cpp`: performance fixes.
- `tools/repack.py`: rebuilds the APK; `tools/patch_dex.py`: fixes a bug in the original Java code (recursive
  `AlarmService.SafeParseInt`).
- `tools/patch_manifest.py`: full screen up to a 3:1 aspect ratio, every configuration change handled by the game
  (otherwise Android recreates the activity and the engine hangs: folding, display size, dark mode…) and
  `targetSdkVersion` 24 (Android 15 refuses apps targeting less).
- `tools/java/`: cheat code input window, compiled into `classes3.dex` by `package.ps1`; `tools/lua/`: Lua scripts of
  the cheat codes and for debugging; `tools/java_test/`: test tool (multi-finger touches from adb); the
  `tools/*.sh` scripts (Git Bash) are for measurements on the phone.
- `third_party/`: Dynarmic, Boost headers, dlmalloc (origins and licenses in `third_party/README.md`).
- The cheat code window (`tools/java/com/acp64/CheatDialog.java`) is in French on a phone set to French, in English
  otherwise.

For an x86_64 Android emulator (testing): `.\package.ps1 -Abi x86_64` gives `out\AC_Pirates_emulator_x86_64.apk`.

## Diagnostics

`adb shell setprop debug.acp64.verbose 1` turns on detailed logs (tag `ACP64`, read them with
`adb logcat -s ACP64`); `adb shell setprop debug.acp64.verbose 0` turns them off (they slow the game down). The
setting is read when the game starts and is cleared when the phone restarts.

## Cheat code (infinite resources)

The secret sequence **Volume +, Volume +, Volume −, Volume −** (within 4 s, in game) opens a "Cheat code" window;
`klm` (upper or lower case) turns infinite resources on or off: the player's gold, wood, supplies and books are
refilled up to the game's limits (9,999,999 for gold, 9,999 for the rest) a few times per second. The setting is
remembered across launches (file `infinite_resources` in the app's private folder). The window never opens under a
finger touching the screen.

A touch gesture was tried first: it triggered by mistake while playing (wheel + cannons with both thumbs), and
presses with three fingers or more are swallowed by some phones (nubia, REDMAGIC…: three-finger screenshot, the game
only receives a cancel). The volume keys do not interfere with touch. Each press can reach the game twice, one frame
apart: `cheat_ui.cpp` ignores the second one.

### `queen` code: the Queen Anne's Revenge

`queen` (same window) unlocks the Queen Anne's Revenge, the ship (identifier `FRIGATE4`; all its characteristics and
its 3D model are in the data) that the game sold as an in-app purchase (`ubi.mobilesdk.pirates.blackbeard`) and that
can no longer be bought. `tools/lua/unlock_queen.lua` calls `playerProfile:BuyShip("FRIGATE4")`, the function the
game's shop calls once a ship is paid for; it then shows up in your ships, where you can select it.

The game replaces the profile's ship data when it loads the save (and at other times): so the runtime checks
ownership about once per second, and grants the ship again if needed. Nothing is granted to the main menu's default
profile (`_SessionNumber` at 0 and `_IsInMainMenu` true), which is replaced on load. The code is remembered (file
`queen_anne` in the app's private folder): the ship is back at every launch, without typing the code again. Typed
before your game is loaded, the code is accepted ("unlocks once your game is loaded") and a second "unlocked"
message comes later (`CheatDialog.notifyEvent`, called by the runtime).

How both codes work:

- The game logic (economy included) is LuaJIT, and the library exports the whole Lua C API. `src/cheat/cheat.cpp`
  replaces the first instruction of `lua_pcall` with an `svc` (`cpu::install_thumb_entry_hook`): at every call, the
  runtime can run a Lua chunk on the thread that runs the scripts, between two calls from the game, so without
  concurrent access to the Lua state.
- `tools/lua/cheat_infinite.lua` (embedded at build time by CMake) writes to `playerProfile` (`SoftCurrency`,
  `Wood`, `Supplies`, `Books`) through the game's properties, which clamp the values and notify the interface.
- `tools/java/com/acp64/CheatDialog.java` is the input window; it calls `Java_com_acp64_CheatDialog_nativeSubmit`
  (`src/android/cheat_ui.cpp`), which checks the code.

## Window destroyed while the game queries it

When the game goes to the background (home screen, "More games" page, lock…), Android destroys the native window,
but the engine keeps asking for its size for a moment with the old handle (and before the new one exists).
`ANativeWindow_getWidth(nullptr)` crashed the game with a segfault (address 0x90). The `ANativeWindow_*` functions
of `android_glue.cpp` now answer with the last known values when the handle no longer exists (`PtrMap::find_host`,
without logging), and `eglCreateWindowSurface` returns `EGL_NO_SURFACE` instead of crashing.

## Touches cancelled by the system

When Android takes a touch away from the app (`ACTION_CANCEL`: three-finger screenshot, a window taking the focus…),
the game never releases those fingers: its touch input stays stuck (the wheel no longer responds, no more white
outline) until it restarts. `android_glue.cpp` therefore replaces the cancel, for the game, with the release of each
listed finger (`POINTER_UP` then `UP`, `LiftEvent`), and wakes up the game's input loop so that it reads them.

## Performance: up to 144 fps

The game aims for the display's highest refresh rate, read from Android when its window is created (144 Hz on the
REDMAGIC NX799J, 120 Hz on many phones), and asks Android to run the display at that rate for its window
(`ANativeWindow_setFrameRate`, Android 11 and newer): some phones give games 60 Hz unless they ask.
`adb shell setprop debug.acp64.fps 60` (30 to 240) fixes another frame rate, for instance to save battery, and
`adb shell setprop debug.acp64.fps ''` goes back to automatic (read when the game starts).

Frames actually presented on the REDMAGIC NX799J (Snapdragon 8 Elite), `tools/fps_sample.py` (SurfaceFlinger
timestamps). With the display at 60 Hz:

| Version | Sailing at sea (3D) | Game thread |
|---|---|---|
| Before (original game capped at 30) | 30 fps; forced to 60: ~52 fps | 95% (at 60 Hz) |
| After | **59.9 fps, no late frame** (43 s of sailing, 60 s with fishing) | ~40% |

With the display at 144 Hz:

| Scene | Frames per second |
|---|---|
| Menus, in-engine cutscenes | **144**, no late frame |
| Tutorial bay, sailing and turning | 140 to 144 |
| Open sea with many islands (Scorpion Reef) | ~124: the game thread is saturated (~8 ms per frame for a 6.9 ms budget) |

On this phone, Android keeps the display at 144 Hz while the game runs, even when the game asks for 120 or 60: where
the game cannot keep up, some frames stay on screen for two refreshes instead of one.

The game also runs its 3D scenes at that rate (`GP.PARAMS.FpsClampingTarget`, and `TargetElapsedTime` on the game
loop, `fps60` in `src/game/perf_patches.cpp`). Its clock follows real time (the Lua global `Dt` is the measured frame
time; `AbsoluteTime` advances by 12.05 s in 12.05 real seconds at 60 fps, by 13.061 s in 13.055 s at 144 fps), so the
frame rate does not change the game's speed. The engine actually defaults to 60 (`GameLoop.TargetElapsedTime = 1/60`);
the phones of the time were set to 30.

What profiling showed (game thread) and the fixes (`src/game/perf_patches.cpp`, `src/cpu.cpp`; each one can be turned
off with `adb shell setprop debug.acp64.<name> 0` to compare):

- **Lua garbage collector (~50% of the game thread)**: the engine runs LuaJIT's GC on a helper thread during
  rendering (`StartAsyncGarbageCollection` / `StopAsyncGarbageCollection`), but `lua_asynchronous_gc_step` forces a
  step even when the GC is idle: a new full cycle starts as soon as the previous one ends, and the game thread's
  allocations, which always find the GC mid-cycle, pay for steps too (`lj_gc_step`). `gcpace`: the helper thread only
  starts a cycle when one is due (the heap has grown by the `pause` ratio, as in LuaJIT); `gcdefer`: the game
  thread's steps are left to the helper thread as long as the heap stays under 1.5 times the normal threshold. The
  heap oscillates between ~45 and ~90 MB.
- **LuaJIT compiler (~1.5 ms per frame)**: LuaJIT 2.0 must place its machine code within 32 MB of its VM, in the
  middle of this 19 MB library: it never manages to, so it records a trace, fails to place it (32 `mmap`/`munmap`
  attempts) and starts again, ~800 times per second. `luajit_off` turns the compiler off at startup (`jit.off()`):
  the interpreter alone is faster.
- **Busy waiting**: `IdleCurThread` (the engine's "sleep", used by `android_main` every 16 ms) spins on
  `clock_gettime` + `sched_yield` (1.2 million calls per second, one core busy at 100%). `idle`: a real sleep.
- **JIT memory accesses**: `fastmem` (Dynarmic): since guest memory is at the same address in the process, a guest
  load or store becomes a single instruction instead of a page-table walk.
- **Java attachments**: `SparkSystem::JNIEnvWrapper` attached and detached `android_main` from the Java VM every
  16 ms (a new `java.lang.Thread` each time). The detach requested by the game no longer really detaches; the thread
  is detached when it exits (`jni_bridge.cpp`).
- **Octree frustum culling (~14% of the game thread at 144 fps)**: `geOctreeNode<geMeshEntity>::GetFrustumIntersection`,
  called for every node and object of the scene at every frame, spent most of its time in the four tiny calls it
  makes and their returns, each one a block lookup for Dynarmic. `frustum`: the same computation runs natively, in
  the same order and without fused multiply-adds, so the results are identical; ~2% left.
- **Indirect branches (~9%)**: Dynarmic's arm64 backend went back to its dispatcher (a C++ hash map) for every
  indirect branch, and the Lua interpreter ends every bytecode with one. `fastdispatch`: a fast dispatch table looked
  up by the generated code itself (added to our copy of Dynarmic, see `third_party/README.md`); open sea: 117 → 124 fps.

Where the time goes now (open sea, game thread): the Lua interpreter ~40%, character animation ~10%, the runtime
~12%, the GPU driver 5%.

Measurement tools (with `debug.acp64.verbose=1`): every second, `eglSwapBuffers` logs fps, work time, worst frame,
then the most called imported functions (`calls/s`) and the blocking waits with their caller (`waits:`), as well as
frames longer than 50 ms. For a profile: `<profileable android:shell="true"/>` is in the manifest, so
`simpleperf record --app com.ubisoft.assassin.pirates -e cpu-clock` works; the Lua probe `acp64_profdump(path)`
writes the map of each thread's compiled blocks (`path.t<id>`), and `tools/prof_report.py perf.data <map> <tid>`
attributes the samples to the ARM32 game functions. `tools/goto_sea.sh` takes the game to sea from a fresh start,
`tools/bench.sh` averages the frame statistics.

## Image quality: full resolution and 4x MSAA

The game picks a quality profile by phone model when it starts (`DeviceParameters.lua`). On a phone it does not know,
it takes its Android "HD" profile (`ParamsConfig_Android_HD`): 3D scenes are drawn at 62.5% of the screen resolution
(`RenderScaleFactor`, 1680×760 for a 2688×1216 screen), then stretched to the screen, without anti-aliasing
(`MsaaSampleCount` = 1). Edges are jagged and blurry, the rigging above all. The game's own top profile
(`ParamsConfig_HD`) uses the full resolution and 4x MSAA, and the port now does the same (`quality` chunk in
`src/game/perf_patches.cpp`): it sets both values in the game's parameters, which each level reads when it loads to
create the render target its 3D scene is drawn into (`PostFX.lua`).

The engine needed a fix as well: it keeps the sample count asked for a render target only on an OpenGL ES 3 context
(`geOesRenderer::IsOES3`, false because the game asks EGL for ES 2) and silently uses 1 otherwise, although its ES 2
path is complete: it renders with `GL_EXT_multisampled_render_to_texture`, which phone GPUs resolve in their tile
memory. The two checks (in `geOesRenderer::CreateTexture` and `CreateDepthStencilBuffer`) are removed. When a level
loads, the log shows `MSAA render target: ... 4 samples`.

Sailing in Scorpion Reef on the REDMAGIC NX799J at 144 Hz:

| | 3D scene | MSAA | Frames per second | GPU busy (`gpubusy`) |
|---|---|---|---|---|
| Game's profile | 1680×760, stretched | none | 113 | ~29% |
| Port | 2688×1216 | 4x | 119 | ~45% |

No frame lost: the game thread remains the limit, the GPU stays below half its capacity (the two runs were not at
the same time of day, hence the gap). On a weaker phone, `adb shell setprop debug.acp64.msaa 2` (1, 2 or 4 samples)
and `adb shell setprop debug.acp64.renderscale 75` (25 to 100, in percent of the screen resolution) lower these
values; `0` keeps the game's value and `''` goes back to the port's (read when the game starts).

## Debugging

With `debug.acp64.verbose` at 1 (and `adb shell setprop log.tag.ACP64 V` to see the debug lines), a `probe.lua` file
copied to `/sdcard/Android/data/com.ubisoft.assassin.pirates/files/` is run in the game's Lua interpreter every time
it changes (touch it before copying it again: adb only keeps whole seconds); `acp64_log(text)` writes to `probe.out`
in the same folder (a `!` at the start of the text also sends it to logcat). `tools/lua/probe_*.lua` are examples
(list of globals, search for money-related names, function contents). `tools/java_test/Inj.java` injects
multi-finger touches from adb (`app_process`), to test without touching the screen.

## License

The code in this repository (runtime, scripts, tools) is under the MIT license: see `LICENSE`. The libraries in
`third_party/` keep their own licenses (see `third_party/README.md`).

Unofficial project, not affiliated with Ubisoft. The game (code, data, Assassin's Creed name and trademarks) belongs
to Ubisoft and is not covered by this license.
