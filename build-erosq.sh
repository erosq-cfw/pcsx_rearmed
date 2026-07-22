#!/bin/sh
# Milestone-2 standalone build: pcsx_rearmed frontend for EROSQ (X1000 mipsel musl).
# Real core + Lightrec + gpu_unai + plat_erosq (drm_out/evdev/dfsound-alsa).
# ⚠️ DEV BUILD ONLY — ship builds go through build-erosq-pgo.sh (PGO, +2-4%).
# Two 2026-07 ships silently lost the PGO layout by using this script.
echo "NOTE: dev build (no PGO). Ship via build-erosq-pgo.sh." >&2
set -e
export PATH=buildroot-2026.05/output/host/bin:$PATH
cd pcsx_rearmed

TC=mipsel-buildroot-linux-musl
export CC=${TC}-gcc
export CXX=${TC}-g++
export AS=${TC}-as
export CC_AS=${TC}-gcc
export AR=${TC}-ar
export LD=${TC}-ld
export OBJCOPY=${TC}-objcopy

if [ "$1" = "conf" ] || [ ! -f config.mak ]; then
	./configure --platform=erosq --gpu=unai --dynarec=lightrec
fi

# USE_ASYNC_CDROM: CD prefetch thread hides SD/CHD read latency (47-188ms
# spikes stalled the emu thread and underran XA audio in PaRappa). Other
# async options stay off (single-core X1000). NOTE: if you toggle this flag,
# purge libpcsxcore/cdrom-async.o frontend/menu.o frontend/main.o first.
make -j"$(nproc)" USE_ASYNC_CDROM=1 2>&1
echo "=== BUILD RESULT ==="
ls -la pcsx 2>/dev/null && file pcsx
