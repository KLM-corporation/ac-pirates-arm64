"""Profile report for the game: simpleperf samples of one thread, with the JIT-compiled guest code mapped back to
functions of the ARM32 game library.

usage: prof_report.py <perf.data> <jitmap.txt> <tid> [top N]

  perf.data   simpleperf record --app com.ubisoft.assassin.pirates -e cpu-clock ...
  jitmap.txt  written by acp64_profdump(path) (Lua probe) on the same thread, right after recording
Paths of the ARM32 game library (work/libSparkApp_arm32.so) and of the unstripped runtime
(build/arm64/libSparkApp.so) are fixed; the guest load bias is read from the jitmap's thunk table header.
"""
import bisect
import collections
import os
import re
import subprocess
import sys

from elftools.elf.elffile import ELFFile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SIMPLEPERF = os.path.join(os.environ["LOCALAPPDATA"], "Android", "Sdk", "ndk", "27.3.13750724", "simpleperf", "bin",
                          "windows", "x86_64", "simpleperf.exe")
CXXFILT = os.path.join(os.environ["LOCALAPPDATA"], "Android", "Sdk", "ndk", "27.3.13750724", "toolchains", "llvm",
                       "prebuilt", "windows-x86_64", "bin", "llvm-cxxfilt.exe")
GAME_LIB = os.path.join(ROOT, "work", "libSparkApp_arm32.so")
RUNTIME_LIB = os.path.join(ROOT, "build", "arm64", "libSparkApp.so")
GAME_BIAS = 0xFE26F000  # "libSparkApp.so: loaded at ..." in the runtime log


def func_table(path, symtab_names=(".symtab", ".dynsym"), clear_thumb=False):
    with open(path, "rb") as f:
        elf = ELFFile(f)
        syms = {}
        for name in symtab_names:
            sec = elf.get_section_by_name(name)
            if not sec:
                continue
            for s in sec.iter_symbols():
                if s["st_info"]["type"] == "STT_FUNC" and s["st_value"] and s.name:
                    a = s["st_value"] & ~1 if clear_thumb else s["st_value"]
                    syms.setdefault(a, s.name)
    addrs = sorted(syms)
    return addrs, [syms[a] for a in addrs]


def lookup(table, addr):
    addrs, names = table
    i = bisect.bisect_right(addrs, addr) - 1
    if i < 0:
        return None, 0
    return names[i], addr - addrs[i]


def demangle(names):
    if not names or not os.path.exists(CXXFILT):
        return {n: n for n in names}
    out = subprocess.run([CXXFILT], input="\n".join(names), capture_output=True, text=True).stdout.splitlines()
    return {n: (d if len(d) < 110 else d[:107] + "...") for n, d in zip(names, out)}


def main():
    perf, jitmap, tid = sys.argv[1], sys.argv[2], int(sys.argv[3])
    top = int(sys.argv[4]) if len(sys.argv) > 4 else 40

    # ---- JIT block map: entries of every JIT of the dumping thread
    ranges, entries = [], []
    thunks = {}
    for ln in open(jitmap, encoding="utf-8", errors="replace"):
        p = ln.split()
        if not p:
            continue
        if p[0] == "jit":
            ranges.append((int(p[2], 16), int(p[3], 16)))
        elif p[0] == "b":
            entries.append((int(p[1], 16), int(p[2], 16), p[3] == "1"))
        elif p[0] == "t":
            thunks[int(p[1], 16)] = p[2]
    entries.sort()
    entry_addrs = [e[0] for e in entries]

    game = func_table(GAME_LIB, clear_thumb=True)
    runtime = func_table(RUNTIME_LIB)

    # ---- samples
    out = subprocess.run([SIMPLEPERF, "report-sample", "-i", perf], capture_output=True, text=True, errors="replace").stdout
    samples = []
    cur = {}
    for ln in out.splitlines():
        ln = ln.strip()
        if ln == "sample:":
            if cur:
                samples.append(cur)
            cur = {}
        elif ": " in ln:
            k, v = ln.split(": ", 1)
            cur.setdefault(k, v)
    if cur:
        samples.append(cur)
    samples = [s for s in samples if s.get("thread_id") == str(tid)]
    total = len(samples)
    if not total:
        print("no samples for tid", tid)
        return

    guest_funcs = collections.Counter()
    host_syms = collections.Counter()
    cats = collections.Counter()
    for s in samples:
        f = s.get("file", "")
        va = int(s.get("vaddr_in_file", "0"), 16)
        if f in ("unknown", "[anon:dynarmic]") or f.startswith("[anon") or f == "//anon":
            in_jit = any(lo <= va < hi for lo, hi in ranges)
            i = bisect.bisect_right(entry_addrs, va) - 1
            if in_jit and i >= 0:
                host, gpc, thumb = entries[i]
                if gpc in thunks or (0x00100000 <= gpc < 0x00200000 and False):
                    guest_funcs["[thunk] " + thunks[gpc]] += 1
                    cats["jit: thunk stubs"] += 1
                    continue
                name, off = lookup(game, gpc - GAME_BIAS) if gpc >= GAME_BIAS else (None, 0)
                if name and gpc - GAME_BIAS < 0x1200000:
                    guest_funcs[name] += 1
                    cats["jit: game code"] += 1
                else:
                    guest_funcs["guest pc 0x%08x (outside the game library)" % (gpc & ~0xFFF)] += 1
                    cats["jit: other guest code"] += 1
            elif in_jit:
                cats["jit: dynarmic prelude/dispatcher"] += 1
                host_syms["[dynarmic prelude/dispatcher]"] += 1
            else:
                cats["anonymous memory (not a known JIT)"] += 1
                host_syms["[anon 0x%x]" % (va & ~0xFFFFF)] += 1
        else:
            base = os.path.basename(f.split("!")[-1])
            if base == "libSparkApp.so":
                name, off = lookup(runtime, va)
                host_syms["runtime: " + (name or "?")] += 1
                cats["host: runtime (thunks, dynarmic, HLE)"] += 1
            else:
                host_syms[base + ": " + s.get("symbol", "?")] += 1
                cats["host: " + base] += 1

    print(f"thread {tid}: {total} samples")
    print("\n== categories")
    for k, v in cats.most_common():
        print(f"  {100.0 * v / total:5.1f}%  {k}")
    names = [n for n, _ in guest_funcs.most_common(top)] + [n.split(": ", 1)[-1] for n, _ in host_syms.most_common(top)]
    dm = demangle(names)
    print(f"\n== guest functions (JIT code), top {top}")
    for n, v in guest_funcs.most_common(top):
        print(f"  {100.0 * v / total:5.1f}%  {dm.get(n, n)}")
    print(f"\n== host functions, top {top}")
    for n, v in host_syms.most_common(top):
        head, _, sym = n.partition(": ")
        print(f"  {100.0 * v / total:5.1f}%  {head}: {dm.get(sym, sym)}" if sym else f"  {100.0 * v / total:5.1f}%  {n}")


if __name__ == "__main__":
    main()
