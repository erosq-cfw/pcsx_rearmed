#!/bin/sh
# Does the stick actually drive the analog axes? Get into Ape Escape gameplay,
# then hold the stick and see whether Spike moves.
#
# On the new binary the stick no longer produces d-pad presses (proved
# separately by gt2stick.sh), so any movement here can only have arrived
# through in_adev/update_analogs -> in_analog_left -> the PSX pad.
#
#   usage: apemove.sh <binary> <tag>
set -u
BIN=$1
TAG=$2
LOG=/media/sd/psx/apemove-$TAG.log
SHOTDIR=/media/sd/psx/movshots-$TAG
DISC="/media/sd/psx/Ape Escape (USA).cue"

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

ALSA_NAME=plughw:0,0 EROSQ_PROF=1 ./"$(basename "$BIN")" -cdfile "$DISC" >> "$LOG" 2>&1 &
EMUPID=$!
sleep 35
kill -0 $EMUPID 2>/dev/null || { echo "  EMU DIED"; tail -8 "$LOG"; exec 3>&-; kill $STPID 2>/dev/null; exit 1; }

# skip intro into gameplay (same sequence apeanalog.sh landed gameplay with)
i=1
while [ $i -le 6 ]; do
	echo start >&3; sleep 2
	echo start >&3; sleep 2
	echo a >&3;     sleep 2
	sleep 6
	i=$((i+1))
done

echo "  -- idle baseline (stick centred), two shots 6s apart to show idle drift"
shot 00-idle-a
sleep 6
shot 01-idle-b

echo "  -- stick held LEFT for 8s"
echo "x -32000" >&3
sleep 8
shot 02-stick-left-held
echo "x 0" >&3
sleep 3
shot 03-stick-centred

echo "  -- stick held UP for 8s"
echo "y -32000" >&3
sleep 8
shot 04-stick-up-held
echo "y 0" >&3
sleep 3

echo "  -- d-pad held LEFT for 8s (must still work)"
echo "hat l" >&3
sleep 8
shot 05-dpad-left-held
echo "hat c" >&3
sleep 3

grep "^ADBG" "$LOG" | sort -u | sed 's/^/    /'
kill -0 $EMUPID 2>/dev/null && echo "  alive" || echo "  DIED"
exec 3>&-
kill -TERM $EMUPID 2>/dev/null; sleep 2; kill -KILL $EMUPID 2>/dev/null
kill $STPID 2>/dev/null
sleep 1
