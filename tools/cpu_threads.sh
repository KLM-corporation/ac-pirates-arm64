#!/usr/bin/env bash
# Per-thread CPU load of the game (top of the busiest threads), two 3-second samples.
#   tools/cpu_threads.sh <serial>
export MSYS_NO_PATHCONV=1
A="adb -s $1"
PKG=com.ubisoft.assassin.pirates
PID=$($A shell pidof $PKG | tr -d '\r')
[ -z "$PID" ] && { echo "game not running"; exit 1; }
$A shell "top -b -H -n 3 -d 3 -p $PID" 2>/dev/null | awk '/TID/{c++} c==3' | head -8 | cut -c1-112
