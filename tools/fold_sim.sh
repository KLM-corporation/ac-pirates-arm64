#!/usr/bin/env bash
# Simulates foldable screens on a regular phone with `wm size`, then restores
# the original resolution (trap on exit).
#   tools/fold_sim.sh <serial> <apk>
set -u
export MSYS_NO_PATHCONV=1
S=$1
APK=$2
PKG=com.ubisoft.assassin.pirates
A="adb -s $S"
WORK=$(mkdir -p "$(dirname "$0")/../work" && cd "$(dirname "$0")/../work" && (pwd -W 2>/dev/null || pwd))
OUT=$WORK
OUTER=904x2316   # Galaxy Z Fold5 cover screen, 2.56:1
INNER=1856x2160  # Galaxy Z Fold6 inner screen, 1.16:1

restore() {
    $A shell wm size reset
    echo "restored: $($A shell wm size | tr '\n' ' ')"
}
trap restore EXIT

$A install --bypass-low-target-sdk-block -r "$APK" | tail -1
$A logcat -c
$A logcat -v threadtime > "$OUT/fold_sim.log" 2>/dev/null &
LOGPID=$!

run_game() {  # $1 = size, $2 = screenshot name
    $A shell wm size "$1"
    $A shell am force-stop $PKG
    sleep 2
    $A shell am start -n $PKG/.SparkActivity >/dev/null
    sleep 50
    $A exec-out screencap -p > "$OUT/$2.png"
    echo "$2 ($1): pid=$($A shell pidof $PKG | tr -d '\r')"
}

run_game $OUTER sim_outer
run_game $INNER sim_inner

# Fold, then unfold, while the game is running.
$A shell wm size $OUTER
sleep 10
$A exec-out screencap -p > "$OUT/sim_fold.png"
echo "after fold: pid=$($A shell pidof $PKG | tr -d '\r')"
$A shell wm size $INNER
sleep 10
$A exec-out screencap -p > "$OUT/sim_unfold.png"
echo "after unfold: pid=$($A shell pidof $PKG | tr -d '\r')"

kill $LOGPID 2>/dev/null
grep -E "ACP64|F DEBUG|FATAL|AndroidRuntime" "$OUT/fold_sim.log" | grep -vE "luaJIT_BC|dlsym" > "$OUT/fold_sim_game.log"
