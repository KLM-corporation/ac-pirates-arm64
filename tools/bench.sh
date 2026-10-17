#!/usr/bin/env bash
# Averages the runtime's per-second frame statistics ("swap:" lines, needs debug.acp64.verbose=1) over N seconds.
#   tools/bench.sh <serial> <seconds> <label>
export MSYS_NO_PATHCONV=1
A="adb -s $1"
WORK=$(mkdir -p "$(dirname "$0")/../work" && cd "$(dirname "$0")/../work" && (pwd -W 2>/dev/null || pwd))
SECS=${2:-10}
LABEL=${3:-bench}
$A logcat -c
sleep "$SECS"
$A logcat -d -s ACP64:I | grep "swap:" | sed 's/.*swap: //' > "$WORK/bench_tmp.txt"
python -I - "$LABEL" "$WORK/bench_tmp.txt" <<'PY'
import re, sys
label, path = sys.argv[1], sys.argv[2]
rows = []
for ln in open(path, encoding='utf-8', errors='replace'):
    m = re.search(r'= ([\d.]+) fps; per frame: ([\d.]+) ms of work, ([\d.]+) ms waiting.*worst ([\d.]+) ms, (\d+) frames over', ln)
    if m:
        rows.append(tuple(float(x) for x in m.groups()))
if not rows:
    print(label, ": no data"); sys.exit()
n = len(rows)
fps = sum(r[0] for r in rows) / n
work = sum(r[1] for r in rows) / n
worst = max(r[3] for r in rows)
slow = sum(r[4] for r in rows)
print(f"{label}: {n} s, avg {fps:.1f} fps (min {min(r[0] for r in rows):.1f}), work {work:.1f} ms/frame, worst frame {worst:.1f} ms, {int(slow)} frames > 17.5 ms")
PY
rm -f "$WORK/bench_tmp.txt"
