"""Frame statistics of the game from SurfaceFlinger's latency log.

usage: fps_sample.py <adb serial> [seconds] [label]

Every second, reads `dumpsys SurfaceFlinger --latency <layer>` (the last 128 presented frames of the game's
layer), merges the samples and prints the average frame rate, the frame time percentiles and the number of
frames that took more than 1.5 display periods (visible stutter).
"""
import re
import statistics
import subprocess
import sys
import time

PKG = "com.ubisoft.assassin.pirates"


def adb(serial, *args):
    return subprocess.run(["adb", "-s", serial, *args], capture_output=True, text=True, errors="replace").stdout


def find_layer(serial):
    """The game's buffer layer: "<package>/<activity>#<id>" (Android 16 prints RequestedLayerState{<name> parentId=..})."""
    out = adb(serial, "shell", "dumpsys", "SurfaceFlinger", "--list")
    names = []
    for ln in out.splitlines():
        m = re.search(r"[{ ](" + re.escape(PKG) + r"/[^ ]+#\d+)", ln)
        if not m or "ActivityRecord" in ln or "InputSink" in ln or "Bounds for" in ln:
            continue
        before = ln[:m.start(1)]
        if re.search(r"\{[0-9a-f]+ $", before):  # "<hash> <name>": the window container, not the buffer layer
            continue
        names.append(m.group(1))
    return sorted(names, key=lambda n: int(n.rsplit("#", 1)[1]))[-1] if names else None


def main():
    serial = sys.argv[1]
    seconds = int(sys.argv[2]) if len(sys.argv) > 2 else 10
    label = sys.argv[3] if len(sys.argv) > 3 else ""
    layer = find_layer(serial)
    if not layer:
        print("game layer not found")
        return 1
    present = set()
    period = None
    t0 = time.time()
    while time.time() - t0 < seconds:
        out = adb(serial, "shell", f"dumpsys SurfaceFlinger --latency '{layer}'").splitlines()
        if out:
            period = int(out[0]) if out[0].strip().isdigit() else period
            for ln in out[1:]:
                parts = ln.split()
                if len(parts) == 3 and parts[1] not in ("0", "9223372036854775807"):
                    present.add(int(parts[1]))
        time.sleep(1.0)
    ts = sorted(present)
    if len(ts) < 3:
        print(f"{label}: not enough frames ({len(ts)})")
        return 1
    # keep the part of the timeline covered by consecutive samples
    deltas = [(b - a) / 1e6 for a, b in zip(ts, ts[1:])]
    deltas = [d for d in deltas if d < 1000]
    span = (ts[-1] - ts[0]) / 1e9
    fps = len(ts) / span if span else 0
    period_ms = (period or 16666666) / 1e6
    janks = sum(1 for d in deltas if d > 1.5 * period_ms)
    qs = statistics.quantiles(deltas, n=100)
    print(f"{label} layer={layer.split('#')[-1]} display period {period_ms:.2f} ms ({1000 / period_ms:.0f} Hz)")
    print(f"{label} frames={len(ts)} over {span:.1f}s -> {fps:.1f} fps | frame time median {statistics.median(deltas):.1f} ms, "
          f"p95 {qs[94]:.1f} ms, p99 {qs[98]:.1f} ms, max {max(deltas):.1f} ms | late frames (>1.5 periods): {janks}/{len(deltas)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
