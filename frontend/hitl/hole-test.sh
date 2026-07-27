#!/bin/sh
# EROSQ HITL for the 2026-07-27 review fixes. Targets what pad-hotplug2.sh
# cannot reach: that fixture's devices all have binds, so in_probe()'s cleanup
# never removes anything and never compacts, and its pad is always the highest
# node so nothing ever sits above a hole.
#
# Shape of the test:
#   nobind comes up FIRST (lower node, no usable binds), pad comes up SECOND
#   (higher node). Then nobind is killed. That single event triggers BOTH bugs:
#     - a hole appears below the pad, and the old enumeration stopped at the
#       first hole => the pad's node was never reopened => pad goes DEAD;
#     - the bindless entry is removed from the middle of the device table, and
#       the old compaction left a duplicate of the pad in the vacated slot,
#       whose drv_data the NEXT re-probe frees while the copy still points at
#       it => use-after-free on later cycles.
#
# usage: hole-test.sh <binary>      e.g. hole-test.sh ./pcsx.pre-review
set -u
BIN=${1:-./pcsx}
LOG=/media/sd/psx/hole-$(basename "$BIN").log
DISC=/media/sd/psx/gt2.cue

cd /media/sd/psx || exit 1

# The UI init wrapper is `sh -c 'erosq-ui || erosq-splash'`, so killing the UI
# spawns splash as a fallback. One killall races that; kill in two steps.
killall erosq-ui 2>/dev/null; sleep 2
killall erosq-splash 2>/dev/null; sleep 2
killall erosq-splash pcsx fakepad nobind mpd 2>/dev/null; sleep 2
[ -w /sys/kernel/erosq_oc/cpu_mhz ] && echo 1200 > /sys/kernel/erosq_oc/cpu_mhz
: > "$LOG"

node_of() { grep -A4 "$1" /proc/bus/input/devices | grep -o 'event[0-9]*' | head -1; }

echo "=== $BIN ==="

./nobind "nobind probe" >> "$LOG" 2>&1 &
NBPID=$!
sleep 2
NBNODE=$(node_of "nobind probe")
echo "  nobind node: $NBNODE  (must be LOWER than the pad)"

rm -f /tmp/pad.fifo; mkfifo /tmp/pad.fifo
./fakepad >> "$LOG" 2>&1 < /tmp/pad.fifo &
PADPID=$!
exec 3> /tmp/pad.fifo
sleep 2
PADNODE=$(node_of "8BitDo")
echo "  pad node:    $PADNODE"

ALSA_NAME=plughw:0,0 EROSQ_PROF=1 ./"$(basename "$BIN")" -cdfile "$DISC" -load 1 >> "$LOG" 2>&1 &
EMUPID=$!
sleep 22
if ! kill -0 $EMUPID 2>/dev/null; then
	echo "  EMU DIED at startup"; tail -5 "$LOG"
	exec 3>&-; kill $PADPID $NBPID 2>/dev/null; exit 1
fi

press_mode() {
	before=$(grep -c "^PROF" "$LOG")
	echo mode >&3
	sleep 6
	n=$(( $(grep -c "^PROF" "$LOG") - before ))
	if [ "$n" -le 1 ]; then echo "  $1: INPUT WORKS (menu opened)"
	else echo "  $1: INPUT DEAD ($n PROF lines in 6s)"; fi
	echo b >&3
	sleep 4
}

press_mode "baseline (nobind + pad both up)"

echo "  -- killing nobind: leaves a hole at $NBNODE, below the pad at $PADNODE,"
echo "     and removes a bindless entry from the middle of the device table"
kill $NBPID 2>/dev/null; wait $NBPID 2>/dev/null
sleep 4
press_mode "after hole opened   "

# Second cycle: this is where a stale duplicate left by the first compaction
# would have its drv_data freed out from under it.
echo "  -- second cycle: nobind returns, then goes away again"
./nobind "nobind probe" >> "$LOG" 2>&1 &
NBPID=$!
sleep 4
press_mode "nobind back         "
kill $NBPID 2>/dev/null; wait $NBPID 2>/dev/null
sleep 4
press_mode "nobind gone again   "

echo "  -- final state"
kill -0 $EMUPID 2>/dev/null && echo "    emulator still alive" || echo "    EMULATOR DIED"
echo "    device table at last probe:"
grep -A6 "drv probed binds name" "$LOG" | tail -6 | sed 's/^/      /'
echo "    evdev error lines: $(grep -c 'ioctl failed\|error reading\|read failed' "$LOG")"
echo "    FATAL lines:       $(grep -c FATAL "$LOG")"

exec 3>&-
kill -TERM $EMUPID 2>/dev/null; sleep 2; kill -KILL $EMUPID 2>/dev/null
kill $PADPID $NBPID 2>/dev/null
sleep 1
