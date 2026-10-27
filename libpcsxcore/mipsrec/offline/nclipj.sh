#!/bin/bash
# nclipj.sh -- EROSQ_NCLIPJ differential test (nclipj.c / nclipj.S), 2026-10-23.
# Every MAC0/FLAG the guest reads back after NCLIP must equal the C handler
# (gte_h_NCLIP = gte.c gteNCLIP_) on the same SXY FIFO, for:
#   jit1 / jit1fm   EROSQ_NCLIPJ=1 (inline NCLIP), faultmem off / on
#   jit0            EROSQ_NCLIPJ=0 (JAL to the C handler: harness control)
#   interp          interpreter (harness control)
# Also counts boundary hits (the vector set has to set FLAG in some cases).
# Env: S scratch dir, W tree with built objects (build-erosq-pgo-xb.sh), NOBUILD=1, HX_NRAND.
# Mutant check (recipe): flip the bit-15/16 choice in opcodes.h recNCLIP_inl
# (SRLV -> no shift), rebuild recompiler.o (grep -E -- '-c libpcsxcore/mipsrec/recompiler.cpp' BUILDLOG | tail -1 | sh),
# rerun: jit1 must FAIL, then restore.
set -u
EROSQ_ROOT=${EROSQ_ROOT:-$(cd "$(dirname "$0")/../../../.." && pwd)}
S=${S:-${TMPDIR:-/tmp}/erosq-nclipj}
W=${W:-$(cd "$(dirname "$0")/../../.." && pwd)}
OFF=$W/libpcsxcore/mipsrec/offline
export PATH=$EROSQ_ROOT/buildroot-2026.08/output/host/bin:$PATH
QEMU="qemu-mipsel-static -L $EROSQ_ROOT/buildroot-2026.08/output/host/mipsel-buildroot-linux-musl/sysroot"
mkdir -p $S
if [ -z "${NOBUILD:-}" ]; then
  ( cd $W
    OBJS=$(ls libpcsxcore/*.o libpcsxcore/mipsrec/*.o | grep -v emu_if)
    mipsel-buildroot-linux-musl-gcc -O2 -DNO_PNG -Iinclude -Ilibpcsxcore -fno-pic -mno-abicalls -G0 -c $OFF/nclipj.c -o $S/nclipj.o &&
    mipsel-buildroot-linux-musl-gcc -O2 -fno-pic -mno-abicalls -G0 -c $OFF/stubs.c -o $S/stubs.o &&
    mipsel-buildroot-linux-musl-g++ -no-pie -Wl,--gc-sections -Wl,-T,libpcsxcore/mipsrec/jitsched.ld -o $S/nclipj $S/nclipj.o $S/stubs.o $OBJS -lz -lm -lpthread 2>$S/link.err &&
    mipsel-buildroot-linux-musl-gcc -mips1 -msoft-float -mno-abicalls -fno-pic -G0 -c $OFF/nclipj.S -o $S/nclipj_g.o &&
    mipsel-buildroot-linux-musl-ld -Ttext=0x80010000 -e test -o $S/nclipj_g.elf $S/nclipj_g.o &&
    mipsel-buildroot-linux-musl-objcopy -O binary -j .text $S/nclipj_g.elf $S/nclipj.bin ) ||
    { echo "build failed"; grep -v abicalls $S/link.err | head; exit 1; }
fi
fail=0
for arm in "jit1:EROSQ_NCLIPJ=1" "jit1fm:EROSQ_NCLIPJ=1 HX_FM=1" "jit0:EROSQ_NCLIPJ=0" "interp:HX_INT=1"; do
  IFS=: read name env <<< "$arm"
  out=$(env $env timeout ${HX_TIMEOUT:-1800} $QEMU $S/nclipj $S/nclipj.bin 2>&1)
  rc=$?
  line=$(echo "$out" | grep '^nclipj:' || echo "nclipj: no result (rc=$rc)")
  echo "  $name: $line"
  echo "$out" | grep MISMATCH | head -4
  [ $rc = 0 ] || fail=$((fail+1))
done
echo "nclipj: fail=$fail"
exit $fail
