#!/bin/sh
# picoarch half of the 2026-07-27 review HITL. Same shape as hole-test.sh, but
# picoarch prints nothing per frame, so the observable is CPU time: emulating
# burns most of a core and picoarch's own menu stops the core loop dead, so a
# collapse in utime after MODE means the pad's input arrived.
#
#   usage: pico-hole-test.sh [binary]
set -u
BIN=${1:-picoarch}
LOG=/media/sd/picoarch/hole-$(basename "$BIN").log
ROM=/media/sd/games/gb/Libbet.gb

cd /media/sd/picoarch || exit 1
killall erosq-ui 2>/dev/null; sleep 2
killall erosq-splash 2>/dev/null; sleep 2
killall erosq-splash mpd picoarch fakepad nobind 2>/dev/null; sleep 2
[ -w /sys/kernel/erosq_oc/cpu_mhz ] && echo 1200 > /sys/kernel/erosq_oc/cpu_mhz
: > "$LOG"

node_of() { grep -A4 "$1" /proc/bus/input/devices | grep -o 'event[0-9]*' | head -1; }
cpu_ticks() { awk '{print $14 + $15}' /proc/$EMUPID/stat 2>/dev/null; }
busy() { a=$(cpu_ticks); sleep 4; b=$(cpu_ticks); echo $((b - a)); }

echo "=== $BIN ==="

/media/sd/psx/nobind "nobind probe" >> "$LOG" 2>&1 &
NBPID=$!
sleep 2
echo "  nobind node: $(node_of 'nobind probe')  (must be LOWER than the pad)"

rm -f /tmp/pad.fifo; mkfifo /tmp/pad.fifo
/media/sd/psx/fakepad >> "$LOG" 2>&1 < /tmp/pad.fifo &
PADPID=$!
exec 3> /tmp/pad.fifo
sleep 2
echo "  pad node:    $(node_of '8BitDo')"

SDCARD_PATH=/media/sd HOME=/media/sd/picoarch CONTENT_DIR=/media/sd/games/gb \
	./"$(basename "$BIN")" cores/gambatte_libretro.so "$ROM" >> "$LOG" 2>&1 &
EMUPID=$!
sleep 12
if ! kill -0 $EMUPID 2>/dev/null; then
	echo "  PICOARCH DIED at startup"; tail -10 "$LOG"
	exec 3>&-; kill $PADPID $NBPID 2>/dev/null; exit 1
fi

press_mode() {
	idle_before=$(busy)
	echo mode >&3
	sleep 2
	idle_after=$(busy)
	if [ "$idle_after" -lt $((idle_before / 3)) ]; then
		echo "  $1: INPUT WORKS (${idle_before} -> ${idle_after} jiffies/4s, core stopped)"
	else
		echo "  $1: INPUT DEAD (${idle_before} -> ${idle_after} jiffies/4s, core still running)"
	fi
	echo b >&3
	sleep 3
}

press_mode "baseline (nobind + pad up)"

echo "  -- killing nobind: hole below the pad + a bindless entry removed"
kill $NBPID 2>/dev/null; wait $NBPID 2>/dev/null
sleep 4
press_mode "after hole opened        "

echo "  -- second cycle"
/media/sd/psx/nobind "nobind probe" >> "$LOG" 2>&1 &
NBPID=$!
sleep 4
press_mode "nobind back              "
kill $NBPID 2>/dev/null; wait $NBPID 2>/dev/null
sleep 4
press_mode "nobind gone again        "

echo "  -- final state"
kill -0 $EMUPID 2>/dev/null && echo "    picoarch still alive" || echo "    PICOARCH DIED"
echo "    FATAL/segv lines: $(grep -ciE 'fatal|segmentation' "$LOG")"

exec 3>&-
kill -TERM $EMUPID 2>/dev/null; sleep 2; kill -KILL $EMUPID 2>/dev/null
kill $PADPID $NBPID 2>/dev/null
sleep 1
