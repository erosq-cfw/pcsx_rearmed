#!/bin/sh
# erosq-ui Extras->Games launcher for pcsx_rearmed (Milestone 3).
# ui_launch_run() invokes this with NO args: it drops DRM master, fork/execs us,
# waitpids, then reclaims master + force-modesets on our exit. So we must NOT
# stop erosq-ui here (unlike the ssh dev launcher) -- just free the PCM device +
# RAM by killing mpd (a live mpd keeps swap pages, starving pcsx -> OOM hang), run
# pcsx (its own disc browser + in-game menu handle selection and savestates), then
# relaunch mpd, which resumes the queue+position itself via state_file (mpd.conf).
# We must NOT `mpc stop` first: that would persist state=stop and kill the resume.
# Binary + this wrapper live on writable SD, so they update without a rootfs
# reflash; only the extras.tsv row ships in rootfs.
was_up=; pidof mpd >/dev/null 2>&1 && was_up=1
[ "$was_up" ] && { killall mpd 2>/dev/null; sleep 1; }
cd /media/sd/psx || exit 1
rm -rf bios                                    # force BiosDir -> ./.pcsx/bios
mkdir -p .pcsx/bios .pcsx/memcards
cp -f scph1001.bin .pcsx/bios/scph1001.bin 2>/dev/null
[ -f .pcsx/pcsx.cfg ] || printf 'Bios = scph1001.bin\nframeskip4 = 0\n' > .pcsx/pcsx.cfg

# CPU overclock for PSX only (kernels with the erosq_oc knob; no-op otherwise).
# 1200 MHz measured stable (878 integrity checks, multi-min soak, ~+15% fps);
# DDR stays at its trained 200 MHz. Restored to stock on exit -- the trap covers
# a pcsx crash/kill too -- so the rest of the system never runs overclocked.
OC=/sys/kernel/erosq_oc/cpu_mhz
if [ -w "$OC" ]; then
	trap 'echo 1008 > "$OC" 2>/dev/null' EXIT
	echo 1200 > "$OC" 2>/dev/null
fi

# A TERM/INT trap that returns would run the mpd tail below. On a critical
# battery erosq-ui SIGTERMs our group, SIGKILLs it 3 s later and starts mpd
# itself: a tail mpd straddling that SIGKILL loses its daemonize parent and dies
# of SIGPIPE at commit (mpd ignores SIGPIPE only after), after the UI's pidof
# already saw it -> no mpd, no /data/mpd.state. So exit; EXIT still restores OC.
trap 'exit 143' INT TERM
ALSA_NAME=plughw:0,0 ./pcsx                     # no -cdfile => pcsx's own disc browser
[ "$was_up" ] && mpd 2>/dev/null
