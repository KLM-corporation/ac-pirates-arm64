"""Copies the Boost headers that the build uses into third_party/boost (kept in the repository: 4 MB instead of
the 190 MB of a full Boost).

Dynarmic only needs a few header-only Boost libraries (icl, variant, mpl...). The list comes from the compiler's
dependency records of existing builds (`ninja -t deps`), so build once with a full Boost as third_party/boost,
then run this script after updating Dynarmic or the NDK.

usage: boost_subset.py <full boost dir (contains boost/)> <ninja.exe> <build dir>...
"""
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEST = os.path.join(ROOT, "third_party", "boost")


def main():
    full, ninja, builds = sys.argv[1], sys.argv[2], sys.argv[3:]
    used = {"boost/version.hpp"}  # read by CMake's FindBoost
    for d in builds:
        out = subprocess.run([ninja, "-C", d, "-t", "deps"], capture_output=True, text=True, errors="replace", check=True)
        for ln in out.stdout.splitlines():
            p = ln.strip().replace("\\", "/")
            i = p.rfind("/boost/")  # <boost root>/boost/<library>/<header>
            if i >= 0 and "third_party/" in p and p.endswith((".hpp", ".h", ".ipp")):
                used.add(p[i + 1:])
    total = 0
    for rel in sorted(used):
        src = os.path.join(full, rel)
        dst = os.path.join(DEST, rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copyfile(src, dst)
        total += os.path.getsize(dst)
    lic = os.path.join(full, "LICENSE_1_0.txt")
    if os.path.exists(lic):
        shutil.copyfile(lic, os.path.join(DEST, "LICENSE_1_0.txt"))
    print(f"{len(used)} headers, {total // 1024} KiB -> {DEST}")


if __name__ == "__main__":
    main()
