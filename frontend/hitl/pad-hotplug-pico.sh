#!/bin/sh
# BT-pad hotplug test for picoarch (GB/GBC/GBA). Same shape as the pcsx one, but
# picoarch prints nothing per frame, so the observable is CPU time: emulating
# burns most of a core, and opening picoarch's own menu stops the core loop dead.
# So a big drop in utime after pressing MODE == the pad's input arrived.
#
#   usage: pad-hotplug-pico.sh [binary]
BIN=${1:-picoarch}
LOG=/media/sd/picoarch/pad.log
ROM=/media/sd/games/gb/Libbet.gb

cd /media/sd/picoarch || exit 1
killall erosq-ui erosq-splash mpd picoarch fakepad 2>/dev/null; sleep 2
[ -w /sys/kernel/erosq_oc/cpu_mhz ] && echo 1200 > /sys/kernel/erosq_oc/cpu_mhz
: > "$LOG"

pad_up() {
	rm -f /tmp/pad.fifo; mkfifo /tmp/pad.fifo
	/media/sd/psx/fakepad >> "$LOG" 2>&1 < /tmp/pad.fifo &
	PADPID=$!
	exec 3> /tmp/pad.fifo
	sleep 2
	echo "    pad node: $(grep -A4 '8BitDo' /proc/bus/input/devices | grep -o 'event[0-9]*')"
}
pad_down() { exec 3>&-; sleep 1; kill $PADPID 2>/dev/null; wait $PADPID 2>/dev/null; sleep 2; }

cpu_ticks() { awk '{print $14 + $15}' /proc/$EMUPID/stat 2>/dev/null; }

# jiffies burned over 4s: emulating is hundreds, a stopped core is a handful
busy() { a=$(cpu_ticks); sleep 4; b=$(cpu_ticks); echo $((b - a)); }

press_mode() {
	idle_before=$(busy)
	echo mode >&3
	sleep 2
	idle_after=$(busy)
	echo "  $1: cpu jiffies/4s ${idle_before} -> ${idle_after}"
	if [ "$idle_after" -lt $((idle_before / 3)) ]; then echo "    INPUT WORKS (core stopped => menu opened)"
	else echo "    INPUT DEAD (core still running)"; fi
	echo b >&3       # PBTN_MBACK -> leave the menu
	sleep 3
	back=$(busy)
	echo "    after back: ${back} jiffies/4s"
}

pad_up
SDCARD_PATH=/media/sd HOME=/media/sd/picoarch CONTENT_DIR=/media/sd/games/gb \
	./$BIN cores/gambatte_libretro.so "$ROM" >> "$LOG" 2>&1 &
EMUPID=$!
sleep 12
kill -0 $EMUPID 2>/dev/null || { echo "PICOARCH DIED"; tail -10 "$LOG"; exit 1; }

press_mode "baseline (pad present)"

echo "  -- disconnect + reconnect"
pad_down
sleep 3
pad_up
sleep 3
press_mode "after reconnect"

kill -0 $EMUPID 2>/dev/null && echo "  picoarch still alive" || echo "  PICOARCH DIED"
echo "  evdev error lines: $(grep -c 'ioctl failed\|error reading\|read failed' "$LOG")"
grep -A6 "drv probed binds name" "$LOG" | tail -5
kill -TERM $EMUPID 2>/dev/null; sleep 2; kill -KILL $EMUPID 2>/dev/null
pad_down
