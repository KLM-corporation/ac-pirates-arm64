#!/usr/bin/env bash
# Test helper: from a fresh game start, skips the intro, waits for JOUER, goes to the map, then sails out from the
# current node (3D scene). Uses screen coordinates of the 2688x1216 landscape layout of the test phone.
#   tools/goto_sea.sh <serial>
export MSYS_NO_PATHCONV=1
A="adb -s $1"
WORK=$(mkdir -p "$(dirname "$0")/../work" && cd "$(dirname "$0")/../work" && (pwd -W 2>/dev/null || pwd))
SHOT="$WORK/goto.png"
# bright <x0> <x1> <y>: 1 if a pixel of that row segment of the last screenshot is near white
bright() {
    python -I - "$SHOT" "$1" "$2" "$3" <<'PY'
import struct, sys, zlib
path, x0, x1, y = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
data = open(path, 'rb').read()
w, h = struct.unpack('>II', data[16:24])
chunks, p = [], 8
while p < len(data):
    n = struct.unpack('>I', data[p:p + 4])[0]
    if data[p + 4:p + 8] == b'IDAT':
        chunks.append(data[p + 8:p + 8 + n])
    p += 12 + n
raw = zlib.decompress(b''.join(chunks))
bpp = 4 if data[25] == 6 else 3
stride = w * bpp + 1
prev = bytearray(w * bpp)
for row in range(y + 1):
    line = raw[row * stride:(row + 1) * stride]
    f, cur = line[0], bytearray(line[1:])
    for i in range(len(cur)):
        a = cur[i - bpp] if i >= bpp else 0
        b = prev[i]
        c = prev[i - bpp] if i >= bpp else 0
        if f == 1: cur[i] = (cur[i] + a) & 255
        elif f == 2: cur[i] = (cur[i] + b) & 255
        elif f == 3: cur[i] = (cur[i] + (a + b) // 2) & 255
        elif f == 4:
            q = a + b - c; pa, pb, pc = abs(q - a), abs(q - b), abs(q - c)
            cur[i] = (cur[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
    prev = cur
print(1 if any(min(prev[x * bpp:x * bpp + 3]) > 225 for x in range(x0, x1)) else 0)
PY
}
sleep 20
$A shell input tap 228 127           # skip the intro cinematic
# Wait until JOUER is lit (the profile loads in the background), closing the "Plus de jeux" page if it opened.
for i in $(seq 1 40); do
    sleep 3
    $A exec-out screencap -p > "$SHOT"
    if [ "$(bright 20 60 300)" = 1 ]; then
        $A shell input tap 2586 70   # web page (white): Close
        continue
    fi
    if [ "$(bright 1250 1500 880)" = 1 ]; then
        break
    fi
done
$A shell input tap 1367 878          # JOUER
sleep 14
$A shell input tap 870 600           # current map node: sail out
sleep 30
$A exec-out screencap -p > "$SHOT"
echo "at sea (screenshot $SHOT)"
