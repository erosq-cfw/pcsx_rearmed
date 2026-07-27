#!/bin/sh
# Stick-vs-d-pad split, checked in-game so the screenshot path actually runs
# (the emulator's own menu stops the render loop, so SIGUSR1 produces nothing
# there). The GT2 save state sits on the dealership menu, where a d-pad press
# moves the highlight and therefore changes the frame.
#
# Expected:
#   before the change: stick down moves the menu (stick == d-pad)
#   after  the change: stick down does NOTHING, d-pad down still moves it
#
#   usage: gt2stick.sh <binary> <tag>
set -u
BIN=$1
TAG=$2
LOG=/media/sd/psx/gt2stick-$TAG.log
SHOTDIR=/media/sd/psx/gt2shots-$TAG
DISC=/media/sd/psx/gt2.cue

cd /media/sd/psx || exit 1
killall erosq-ui 2>/dev/null; sleep 2
killall erosq-splash 2>/dev/null; sleep 2
killall erosq-splash pcsx picoarch fakepad fakestick nobind mpd 2>/dev/null; sleep 2
[ -w /sys/kernel/erosq_oc/cpu_mhz ] && echo 1200 > /sys/kernel/erosq_oc/cpu_mhz
: > "$LOG"
rm -rf "$SHOTDIR"; mkdir -p "$SHOTDIR"

shot() {
	rm -f /media/sd/psx/shot.ppm
	kill -USR1 $EMUPID 2>/dev/null
	sleep 3
	if [ -f /media/sd/psx/shot.ppm ]; then
		cp /media/sd/psx/shot.ppm "$SHOTDIR/$1.ppm"
		echo "    $1: $(md5sum /media/sd/psx/shot.ppm | cut -c1-8)"
	else
		echo "    $1: MISSING"
	fi
}

echo "=== $BIN ($TAG) ==="
rm -f /tmp/stick.fifo; mkfifo /tmp/stick.fifo
./fakestick >> "$LOG" 2>&1 < /tmp/stick.fifo &
STPID=$!
exec 3> /tmp/stick.fifo
sleep 2

ALSA_NAME=plughw:0,0 EROSQ_PROF=1 ./"$(basename "$BIN")" -cdfile "$DISC" -load 1 >> "$LOG" 2>&1 &
EMUPID=$!
sleep 25
if ! kill -0 $EMUPID 2>/dev/null; then
	echo "  EMU DIED"; tail -8 "$LOG"; exec 3>&-; kill $STPID 2>/dev/null; exit 1
fi

shot 00-baseline
echo "  -- stick down x4"
for i in 1 2 3 4; do echo "y 32000" >&3; sleep 1; echo "y 0" >&3; sleep 1; done
shot 01-after-stick
echo "  -- d-pad down x4"
for i in 1 2 3 4; do echo "hat d" >&3; sleep 1; echo "hat c" >&3; sleep 1; done
shot 02-after-dpad

echo "  -- device table"
grep -A6 "drv probed binds name" "$LOG" | tail -4 | sed 's/^/    /'
grep "^ADBG" "$LOG" | sort -u | sed 's/^/    /'
kill -0 $EMUPID 2>/dev/null && echo "  alive" || echo "  DIED"
exec 3>&-
kill -TERM $EMUPID 2>/dev/null; sleep 2; kill -KILL $EMUPID 2>/dev/null
kill $STPID 2>/dev/null
sleep 1
