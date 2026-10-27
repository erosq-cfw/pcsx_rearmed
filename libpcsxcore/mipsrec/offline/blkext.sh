#!/bin/bash
EROSQ_ROOT=${EROSQ_ROOT:-$(cd "$(dirname "$0")/../../../.." && pwd)}
# blkext.sh -- DMA-overwrites-an-earlier-starting-block fixture (blkext/dmamid.S).
# A psxCpu->Clear of blockB+4 (as a DMA into RAM does) must invalidate blockB,
# which starts one word earlier: the final v0 must be 0x222 (new code), with
# block linking off and on. Exit 0 = PASS.
# Mutant (recipe, not a switch): rebuild libpcsxcore/mipsrec/jitsched.o with
#   -DBLKEXT_MUTANT (start-only clears = pre-blkext behaviour), relink: must FAIL
#   with v0 = 0x111 (stale block). 2026-10-20: fix PASS x3, mutant FAIL x3 (0x111).
# Env: S scratch dir, W tree with built objects, BR_HOST toolchain, HX_LDFLAGS.
set -u
S=${S:?S=scratch dir}
W=${W:-$(cd "$(dirname "$0")/../../.." && pwd)}
OFF=$W/libpcsxcore/mipsrec/offline
BR=${BR_HOST:-$EROSQ_ROOT/buildroot-2026.08/output/host}
export PATH=$BR/bin:$PATH
QEMU="qemu-mipsel-static -L $BR/mipsel-buildroot-linux-musl/sysroot"
mkdir -p $S
( cd $W
  OBJS=$(ls libpcsxcore/*.o libpcsxcore/mipsrec/*.o | grep -v emu_if)
  mipsel-buildroot-linux-musl-gcc -O2 -DNO_PNG -Iinclude -Ilibpcsxcore -fno-pic -mno-abicalls -G0 -c $OFF/hlink.c -o $S/hlink.o &&
  mipsel-buildroot-linux-musl-gcc -O2 -fno-pic -mno-abicalls -G0 -c $OFF/stubs.c -o $S/stubs.o &&
  mipsel-buildroot-linux-musl-g++ -no-pie ${HX_LDFLAGS:-} -Wl,--gc-sections -o $S/hlink $S/hlink.o $S/stubs.o $OBJS -lz -lm -lpthread 2>$S/link.err ) ||
  { echo "build failed"; grep -v abicalls $S/link.err | head; exit 1; }
mipsel-buildroot-linux-musl-gcc -mips1 -msoft-float -mno-abicalls -fno-pic -G0 -c $OFF/blkext/dmamid.S -o $S/t.o &&
mipsel-buildroot-linux-musl-ld -Ttext=0x80010000 -e test -o $S/t.elf $S/t.o &&
mipsel-buildroot-linux-musl-objcopy -O binary -j .text $S/t.elf $S/t.bin || { echo "asm failed"; exit 1; }
B=$(mipsel-buildroot-linux-musl-nm $S/t.elf | awk '$3=="blockB"{print $1}')
P=$(printf %x $((0x$B + 4)))
rc=0
for mode in 0 2 3; do
  out=$(HX_DMAPATCH=$P:24020222:5 HX_BUDGET=64 HX_REGS=1 EROSQ_BLOCKLINK=$mode timeout 120 $QEMU $S/hlink $S/t.bin 2>&1)
  v0=$(echo "$out" | awk '$1=="r2=00000222"||$1=="r2=00000111"{sub("r2=","",$1); print $1}')
  end=$(echo "$out" | grep -o 'end=[a-z]*')
  if [ "$v0" = 00000222 ] && [ "$end" = end=done ]; then echo "blocklink=$mode: PASS (v0=$v0, $end)"; else echo "blocklink=$mode: FAIL (v0=${v0:-?}, ${end:-no STATE})"; rc=1; fi
done
[ $rc = 0 ] && echo "blkext: PASS" || echo "blkext: FAIL"
exit $rc
