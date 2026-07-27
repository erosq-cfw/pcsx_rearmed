#!/bin/sh
# Harder BT-pad hotplug cases for pcsx (see pad-hotplug.sh for the observable:
# PROF lines stop when the emu menu opens, so "frames kept coming" = input dead).
#
#   1. three disconnect/reconnect cycles in a row
#   2. a reconnect that lands on a DIFFERENT node number (a decoy uinput device
#      grabs the freed event4 first, so the pad comes back as event5)
#   3. the on-device gpio-keys must survive all the re-probing
#
#   usage: pad-hotplug2.sh [binary]
BIN=${1:-./pcsx}
LOG=/media/sd/psx/pad2.log
DISC=/media/sd/psx/gt2.cue

cd /media/sd/psx || exit 1
killall erosq-ui erosq-splash mpd pcsx fakepad 2>/dev/null; sleep 2
[ -w /sys/kernel/erosq_oc/cpu_mhz ] && echo 1200 > /sys/kernel/erosq_oc/cpu_mhz
: > "$LOG"

pad_up() {
	rm -f /tmp/pad.fifo; mkfifo /tmp/pad.fifo
	./fakepad >> "$LOG" 2>&1 < /tmp/pad.fifo &
	PADPID=$!
	exec 3> /tmp/pad.fifo
	sleep 2
	echo "    pad node: $(grep -A4 '8BitDo' /proc/bus/input/devices | grep -o 'event[0-9]*')"
}
pad_down() { exec 3>&-; sleep 1; kill $PADPID 2>/dev/null; wait $PADPID 2>/dev/null; sleep 2; }

decoy_up() {
	rm -f /tmp/dec.fifo; mkfifo /tmp/dec.fifo
	./fakepad "decoy pad" >> "$LOG" 2>&1 < /tmp/dec.fifo &
	DECPID=$!
	exec 4> /tmp/dec.fifo
	sleep 2
	echo "    decoy node: $(grep -A4 'decoy pad' /proc/bus/input/devices | grep -o 'event[0-9]*')"
}
decoy_down() { exec 4>&-; sleep 1; kill $DECPID 2>/dev/null; wait $DECPID 2>/dev/null; sleep 2; }

press_mode() {   # $1 = label; leaves the menu again afterwards
	before=$(grep -c "^PROF" "$LOG")
	echo mode >&3
	sleep 6
	n=$(( $(grep -c "^PROF" "$LOG") - before ))
	if [ "$n" -le 1 ]; then echo "  $1: INPUT WORKS (menu opened)"
	else echo "  $1: INPUT DEAD ($n PROF lines in 6s)"; fi
	echo b >&3      # PBTN_MBACK -> back out of the menu
	sleep 4
	resumed=$(( $(grep -c "^PROF" "$LOG") - before - n ))
	[ "$resumed" -ge 1 ] || echo "    (note: game did not resume, $resumed PROF)"
}

pad_up
ALSA_NAME=plughw:0,0 EROSQ_PROF=1 ./$BIN -cdfile "$DISC" -load 1 >> "$LOG" 2>&1 &
EMUPID=$!
sleep 22
kill -0 $EMUPID 2>/dev/null || { echo "EMU DIED"; tail -5 "$LOG"; exit 1; }

press_mode "cycle 0 (baseline)"

i=1
while [ $i -le 3 ]; do
	pad_down
	sleep 3
	pad_up
	sleep 3
	press_mode "cycle $i (same node)"
	i=$((i+1))
done

echo "  -- reconnect onto a different node number"
pad_down
decoy_up          # decoy takes the freed event4
pad_up            # pad comes back one higher
sleep 3
press_mode "cycle 4 (different node)"
decoy_down

echo "  -- final state"
kill -0 $EMUPID 2>/dev/null && echo "    emulator still alive" || echo "    EMULATOR DIED"
echo "    input devices seen by the emulator (last probe dump):"
grep -A6 "drv probed binds name" "$LOG" | tail -7
echo "    evdev error lines total: $(grep -c 'ioctl failed\|error reading\|read failed' "$LOG")"
echo "    FATAL lines: $(grep -c FATAL "$LOG")"

kill -TERM $EMUPID 2>/dev/null; sleep 2; kill -KILL $EMUPID 2>/dev/null
pad_down
