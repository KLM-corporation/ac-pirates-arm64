"""Rebuilds the game APK for 64-bit-only devices.

Copies every entry of the original APK except the old native libraries and
signature, optionally replacing some entries (patched dex files) and adding the
replacement files the APK does not have yet (extra dex files), then adds:
  lib/<abi>/libSparkApp.so    the ARM32 runtime (loaded by NativeActivity)
  lib/<abi>/libSparkApp32.so  the original ARM32 game code (read by the runtime)

usage: repack.py <original.apk> <runtime.so> <game_arm32.so> <out_unsigned.apk> <abi> [replacement_dir]
"""
import os
import sys
import zipfile

SKIP_PREFIXES = ("lib/", "META-INF/")


def main():
    src, runtime, game, out, abi = sys.argv[1:6]
    replace_dir = sys.argv[6] if len(sys.argv) > 6 else None
    replacements = {}
    if replace_dir:
        for name in os.listdir(replace_dir):
            replacements[name] = os.path.join(replace_dir, name)
    with zipfile.ZipFile(src) as zin, zipfile.ZipFile(out, "w") as zout:
        original = {info.filename for info in zin.infolist()}
        for info in zin.infolist():
            if info.filename.startswith(SKIP_PREFIXES):
                continue
            if info.filename in replacements:
                with open(replacements[info.filename], "rb") as f:
                    data = f.read()
                print("replaced", info.filename)
            else:
                data = zin.read(info)
            new = zipfile.ZipInfo(info.filename, date_time=info.date_time)
            new.compress_type = info.compress_type
            new.external_attr = info.external_attr
            zout.writestr(new, data)
        for name in sorted(replacements.keys() - original):
            info = zipfile.ZipInfo(name, date_time=(2026, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            with open(replacements[name], "rb") as f:
                zout.writestr(info, f.read())
            print("added", name)
        for name, path in ((f"lib/{abi}/libSparkApp.so", runtime), (f"lib/{abi}/libSparkApp32.so", game)):
            info = zipfile.ZipInfo(name, date_time=(2026, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            with open(path, "rb") as f:
                zout.writestr(info, f.read())
    print("wrote", out)


if __name__ == "__main__":
    main()
