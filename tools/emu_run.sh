#!/usr/bin/env bash
# Installs the test APK on a device, starts the game, waits, then saves the
# game's log (work/run.log) and a screenshot (work/run.png).
#   tools/emu_run.sh <serial> <apk> [seconds] [verbose 0|1]
set -u
export MSYS_NO_PATHCONV=1
SERIAL=$1
APK=$2
WAIT=${3:-40}
VERBOSE=${4:-0}
PKG=com.ubisoft.assassin.pirates
A="adb -s $SERIAL"
WORK=$(mkdir -p "$(dirname "$0")/../work" && cd "$(dirname "$0")/../work" && (pwd -W 2>/dev/null || pwd))

$A shell am force-stop $PKG
$A install --bypass-low-target-sdk-block -r "$APK" | tail -1
for perm in READ_PHONE_STATE GET_ACCOUNTS ACCESS_COARSE_LOCATION WRITE_EXTERNAL_STORAGE READ_EXTERNAL_STORAGE; do
    $A shell pm grant $PKG android.permission.$perm 2>/dev/null
done
$A shell setprop debug.acp64.verbose "$VERBOSE"
$A logcat -c
# Stream the log while the game runs: phone log buffers wrap quickly.
$A logcat -v threadtime > "$WORK/run_all.log" 2>/dev/null &
LOGPID=$!
$A shell am start -n $PKG/.SparkActivity >/dev/null
sleep 6
# The "built for an older version of Android" dialog takes focus on first launches.
if $A shell dumpsys window | grep -q "mCurrentFocus.*DeprecatedTargetSdk\|mCurrentFocus=Window{[^}]*Application Error"; then
    $A shell input keyevent KEYCODE_ENTER
fi
sleep "$WAIT"
PID=$($A shell pidof $PKG | tr -d '\r')
echo "pid after ${WAIT}s: ${PID:-<dead>}"
kill $LOGPID 2>/dev/null
grep -E "ACP64|G:|libc |DEBUG|AndroidRuntime" "$WORK/run_all.log" | grep -v "luaJIT_BC" > "$WORK/run.log"
$A exec-out screencap -p > "$WORK/run.png"
wc -l "$WORK/run.log"
