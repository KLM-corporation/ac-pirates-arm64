"""Imports the game's own files into work/ for package.ps1 (the repository has none of Ubisoft's files).

Accepts the game's .xapk (a zip holding the APK and the OBB), or its .apk, optionally followed by its OBB.
Writes:
  work/original.apk            the game's APK (Java code, resources, assets)
  work/libSparkApp_arm32.so    its ARM32 library (lib/armeabi-v7a/libSparkApp.so), run by the runtime
  work/obb/main.<n>.com.ubisoft.assassin.pirates.obb   the game data, for installer_telephone.bat

Messages are in French on a French system, in English otherwise (ACP64_LANG=fr or en forces one).

usage: import_game.py <game.xapk | game.apk> <work dir> [game.obb]
"""
import hashlib
import json
import os
import shutil
import sys
import zipfile

PKG = "com.ubisoft.assassin.pirates"
LIB = "lib/armeabi-v7a/libSparkApp.so"
# The runtime is written for version 2.9.1 (version code 253): hooks, patches and the JNI bridge rely on this
# exact library.
TESTED_LIB_SHA256 = "1dc7f264f7591193ddf8a234250f3919511f3dda16b5c383de9b66c17cc291a7"
TESTED_OBB = f"main.253.{PKG}.obb"


def is_french():
    lang = os.environ.get("ACP64_LANG", "")
    if lang:
        return lang.lower().startswith("fr")
    if sys.platform == "win32":
        import ctypes
        return ctypes.windll.kernel32.GetUserDefaultUILanguage() & 0x3FF == 0x0C  # LANG_FRENCH
    return os.environ.get("LANG", "").lower().startswith("fr")


FRENCH = is_french()


def tr(fr, en):
    return fr if FRENCH else en


def match_console_encoding():
    """When the output of package.ps1 is piped or saved to a file, Python writes in the ANSI code page while
    PowerShell reads the console's OEM code page, and accents come out garbled: write in the console's code page.
    (Straight to the console, Python already writes Unicode.)"""
    if sys.platform == "win32" and not sys.stdout.isatty():
        import ctypes
        cp = ctypes.windll.kernel32.GetConsoleOutputCP()
        if cp:
            sys.stdout.reconfigure(encoding=f"cp{cp}", errors="replace")


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def copy_stream(src, dst_path):
    tmp = dst_path + ".part"
    with open(tmp, "wb") as dst:
        shutil.copyfileobj(src, dst, 1 << 20)
    os.replace(tmp, dst_path)


def same_size(path, size):
    return os.path.exists(path) and os.path.getsize(path) == size


def fail(msg):
    print(tr("ERREUR : ", "ERROR: ") + msg)
    sys.exit(1)


def main():
    match_console_encoding()
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    src, work = sys.argv[1], sys.argv[2]
    obb_arg = sys.argv[3] if len(sys.argv) > 3 and sys.argv[3] else None
    if not os.path.isfile(src):
        fail(tr(f"fichier introuvable : {src}", f"file not found: {src}"))
    if not zipfile.is_zipfile(src):
        fail(tr(f"{src} n'est pas un .xapk ni un .apk (ce n'est pas une archive zip)",
                f"{src} is neither an .xapk nor an .apk (it is not a zip archive)"))
    os.makedirs(os.path.join(work, "obb"), exist_ok=True)
    apk_out = os.path.join(work, "original.apk")
    obbs = []
    imported = tr("déjà importé", "already imported")

    with zipfile.ZipFile(src) as z:
        names = z.namelist()
        if "AndroidManifest.xml" in names:
            # A plain APK.
            print(f"APK: {os.path.basename(src)}")
            shutil.copyfile(src, apk_out)
        else:
            # An XAPK: the APK(s), the OBB and a manifest.json.
            if "manifest.json" in names:
                try:
                    m = json.loads(z.read("manifest.json"))
                    print(f"XAPK: {m.get('package_name')} version {m.get('version_name')} ({m.get('version_code')})")
                    if m.get("package_name") not in (None, PKG):
                        fail(tr(f"ce XAPK n'est pas Assassin's Creed Pirates ({m.get('package_name')})",
                                f"this XAPK is not Assassin's Creed Pirates ({m.get('package_name')})"))
                except ValueError:
                    pass
            apks = [n for n in names if n.lower().endswith(".apk")]
            if not apks:
                fail(tr("aucun APK dans ce fichier", "no APK in this file"))
            # The base APK (the one with the Java code); split APKs other than the native-library one are not
            # supported (this game ships a single APK).
            base = None
            for n in apks:
                with z.open(n) as f, zipfile.ZipFile(f) as inner:
                    if "classes.dex" in inner.namelist():
                        base = n
                        break
            if base is None:
                fail(tr("aucun APK principal (avec classes.dex) dans ce fichier",
                        "no main APK (with classes.dex) in this file"))
            others = [n for n in apks if n != base]
            if others:
                print(tr("  APK ignorés (non pris en charge) : ", "  APKs ignored (not supported): ") + ", ".join(others))
            info = z.getinfo(base)
            if same_size(apk_out, info.file_size):
                print(f"  {base}: {imported}")
            else:
                print(f"  {base} -> work/original.apk")
                with z.open(base) as f:
                    copy_stream(f, apk_out)
            for n in names:
                if n.lower().endswith(".obb"):
                    dst = os.path.join(work, "obb", os.path.basename(n))
                    size = z.getinfo(n).file_size
                    if same_size(dst, size):
                        print(f"  {os.path.basename(n)}: {imported}")
                    else:
                        print(f"  {os.path.basename(n)} ({size // 1000000} {tr('Mo', 'MB')}) -> work/obb/")
                        with z.open(n) as f:
                            copy_stream(f, dst)
                    obbs.append(os.path.basename(n))

    if obb_arg:
        if not os.path.isfile(obb_arg):
            fail(tr(f"OBB introuvable : {obb_arg}", f"OBB not found: {obb_arg}"))
        dst = os.path.join(work, "obb", os.path.basename(obb_arg))
        if not same_size(dst, os.path.getsize(obb_arg)):
            print(f"  {os.path.basename(obb_arg)} -> work/obb/")
            shutil.copyfile(obb_arg, dst)
        obbs.append(os.path.basename(obb_arg))

    lib_out = os.path.join(work, "libSparkApp_arm32.so")
    with zipfile.ZipFile(apk_out) as apk:
        if LIB not in apk.namelist():
            fail(tr(f"l'APK ne contient pas {LIB} : ce n'est pas le jeu attendu",
                    f"the APK has no {LIB}: this is not the expected game"))
        with apk.open(LIB) as f:
            copy_stream(f, lib_out)
    print(f"  {LIB} -> work/libSparkApp_arm32.so")

    if sha256(lib_out) == TESTED_LIB_SHA256:
        print(tr("Version 2.9.1 reconnue (bibliothèque identique à celle testée).",
                 "Version 2.9.1 recognized (library identical to the tested one)."))
    else:
        print(tr("ATTENTION : cette bibliothèque n'est pas celle de la version 2.9.1 testée ; le jeu risque de ne pas "
                 "démarrer.",
                 "WARNING: this library is not the one of the tested version 2.9.1; the game may not start."))
    if not obbs:
        print(tr(f"ATTENTION : pas de fichier OBB (données du jeu, {TESTED_OBB}). Donne-le en plus de l'APK, ou "
                 f"copie-le toi-même sur le téléphone dans Android/obb/{PKG}/.",
                 f"WARNING: no OBB file (game data, {TESTED_OBB}). Give it along with the APK, or copy it to the "
                 f"phone yourself, into Android/obb/{PKG}/."))
    elif TESTED_OBB not in obbs:
        print(tr(f"ATTENTION : OBB inattendu ({', '.join(obbs)}) ; la version 2.9.1 utilise {TESTED_OBB}.",
                 f"WARNING: unexpected OBB ({', '.join(obbs)}); version 2.9.1 uses {TESTED_OBB}."))
    return 0


if __name__ == "__main__":
    sys.exit(main())
