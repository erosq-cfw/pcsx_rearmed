#!/bin/sh
# Milestone-2 standalone build: pcsx_rearmed frontend for EROSQ (X1000 mipsel musl).
# Real core + Lightrec + gpu_unai + plat_erosq (drm_out/evdev/dfsound-alsa).
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

make -j"$(nproc)" 2>&1
echo "=== BUILD RESULT ==="
ls -la pcsx 2>/dev/null && file pcsx
