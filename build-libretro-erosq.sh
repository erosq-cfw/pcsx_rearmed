#!/bin/sh
# Milestone-1 proving build: pcsx_rearmed libretro core for EROSQ (X1000 mipsel musl).
# Real core + Lightrec + gpu_unai, no SDL/plat. Headless loader drives it on-device.
set -e
export PATH=buildroot-2026.08/output/host/bin:$PATH
cd pcsx_rearmed

TC=mipsel-buildroot-linux-musl
export CC=${TC}-gcc
export CXX=${TC}-g++
export AR=${TC}-ar
export CC_AS=${TC}-gcc

# platform=unix -> .so + LIGHTREC_CUSTOM_MAP; arch auto-detected mipsel via -dumpmachine.
# Force gpu_unai (software) — X1000 has no GPU/NEON.
make -f Makefile.libretro clean >/dev/null 2>&1 || true
make -f Makefile.libretro \
    platform=unix \
    DYNAREC=lightrec \
    BUILTIN_GPU=unai \
    -j"$(nproc)" 2>&1

echo "=== BUILD RESULT ==="
ls -la pcsx_rearmed_libretro.so 2>/dev/null && file pcsx_rearmed_libretro.so
