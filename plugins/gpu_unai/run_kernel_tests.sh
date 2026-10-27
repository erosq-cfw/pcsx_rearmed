#!/bin/bash
EROSQ_ROOT=${EROSQ_ROOT:-$(cd "$(dirname "$0")/../../.." && pwd)}
# run_kernel_tests.sh -- GPU/cspace kernel differential tests (offline bar for any
# gpu_unai / cspace kernel or compiler change). Each test is built twice: host g++
# and the MIPS ship toolchain + XBurst cc1 with the ship opt flags, run under qemu-user.
# XONLY tests need __mips__ (gpu_inner.h includes the MXU kernels only there;
# GPU_MXU_HOST_TEST supplies C models of the MXU ops). HOSTONLY: SIGBUS under qemu.
# Env: BR_HOST (toolchain, default output-gcc15), XB (patched cc1 dir), T (scratch)
# (raw-MXU tests excluded: qemu-user lacks MXU logic ops).
W=$(cd "$(dirname "$0")/../.." && pwd)

T=${T:-$(mktemp -d)}; mkdir -p "$T"   # default: private 0700 dir (a fixed /tmp path could be pre-planted)
BR=${BR_HOST:-$EROSQ_ROOT/buildroot-2026.08/output-gcc15/host}
XB=${XB:-$EROSQ_ROOT/gcc15-xburst-cc1}
OPT="-Ofast -B$XB/ -mtune=24kc --param max-unroll-times=2"
cd $W
pass=0; fail=0
while read name src extra; do
  case $src in *.c) HC=cc; XC=$BR/bin/mipsel-buildroot-linux-musl-gcc;; *) HC=g++; XC=$BR/bin/mipsel-buildroot-linux-musl-g++;; esac
  # XONLY: cross+qemu tests (MXU header only under __mips__); HOSTONLY: SIGBUS under qemu with any gcc
  mode=both; case "$extra" in *XONLY*) mode=x;; *HOSTONLY*) mode=h;; esac; extra=${extra/XONLY/}; extra=${extra/HOSTONLY/}
  h=0; [ $mode = x ] || h=$($HC -O2 $extra -o "$T"/$name.host $src 2>&1 && "$T"/$name.host > "$T"/$name.host.out 2>&1; echo $?)
  x=0; [ $mode = h ] || x=$($XC $OPT $extra -o "$T"/$name.mips $src 2>"$T"/$name.mips.err && qemu-mipsel-static -L $BR/mipsel-buildroot-linux-musl/sysroot "$T"/$name.mips > "$T"/$name.mips.out 2>&1; echo $?)
  out="$T"/$name.mips.out; [ $mode = h ] && out="$T"/$name.host.out
  r=PASS; [ "${h##*$'\n'}" = 0 ] && [ "${x##*$'\n'}" = 0 ] || { r=FAIL; fail=$((fail+1)); }
  [ $r = PASS ] && pass=$((pass+1))
  printf "%-22s host rc=%s  gcc15-mips rc=%s  %s | %s\n" $name "${h##*$'\n'}" "${x##*$'\n'}" $r "[$mode] $(tail -1 "$out" | cut -c1-70)"
done <<'LIST'
blend_swar plugins/gpu_unai/test_blend_swar.cpp
prelight_clut plugins/gpu_unai/test_prelight_clut.cpp -Iinclude
pskip plugins/gpu_unai/test_pskip.cpp -Iinclude -fpermissive XONLY
pskip_mask plugins/gpu_unai/test_pskip_mask.cpp
sprite_tile plugins/gpu_unai/test_sprite_tile_kernels.cpp -Iinclude -fpermissive XONLY
cspace888 frontend/test_cspace888.cpp
cspace_avg frontend/test_cspace_avg.cpp -fpermissive -Iinclude
cspace_scaled frontend/test_cspace_scaled.c -Iinclude
blit_pref frontend/test_blit_pref.c -Iinclude -Ifrontend HOSTONLY
LIST
echo "run_kernel_tests: pass=$pass fail=$fail"
