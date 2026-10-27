#!/bin/bash
EROSQ_ROOT=${EROSQ_ROOT:-$(cd "$(dirname "$0")/../../../.." && pwd)}
# hx_build.sh OUT -- relink the worktree's pcsx objects (after a normal build)
# into a headless binary: frontend/plat_erosq.o + drm_out.o -> plat_headless.o.
# The link line is taken from the build log (LOG, default the scratch build.log).
set -e
W=$(cd "$(dirname "$0")/../../.." && pwd)
OUT=$1
LOG=${LOG:?LOG=<build log of the pcsx build>}
export PATH=$EROSQ_ROOT/buildroot-2026.08/output/host/bin:$PATH
cd $W
mipsel-buildroot-linux-musl-gcc -O2 -Iinclude -Ilibpcsxcore -I. -c libpcsxcore/mipsrec/offline/plat_headless.c -o /tmp/plat_headless.$$.o
# qemu has no MXU: rebuild the two MXU users without their MXU defines
# (guest JIT code and the mipsrec objects are unchanged).
for o in plugins/gpu_unai/gpulib_if frontend/cspace; do
  C=$(grep -m1 -- "-o $o.o" $LOG | sed "s#-o $o.o#-o /tmp/nomxu.$$.$(basename $o).o#; s#-DGPU_UNAI_USE_MXU#-DHX_GPU_MIX#; s#-DHAVE_bgr555_to_rgb565_mxu##")
  eval "$C" 2>&1 | grep -i " error" && exit 1
done
CMD=$(grep -m1 " -o pcsx " $LOG | sed "s# -o pcsx # -o $OUT #; s# frontend/plat_erosq.o # /tmp/plat_headless.$$.o #; s# frontend/drm_out.o # #; s# plugins/gpu_unai/gpulib_if.o # /tmp/nomxu.$$.gpulib_if.o #; s# frontend/cspace.o # /tmp/nomxu.$$.cspace.o #")
eval "$CMD" 2>&1 | grep -v "abicalls\|Wmissing-profile\|coverage" || true
rm -f /tmp/plat_headless.$$.o /tmp/nomxu.$$.*.o
ls -la $OUT
