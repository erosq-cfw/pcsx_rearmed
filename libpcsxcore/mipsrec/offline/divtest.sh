#!/bin/bash
EROSQ_ROOT=${EROSQ_ROOT:-$(cd "$(dirname "$0")/../../../.." && pwd)}
# divtest.sh -- MDU const-operand differential test (divtest.c), 2026-10-20.
# One generated guest program covers DIV/DIVU/MULT/MULTU over
#   dividends {0,1,-1,7,-7,6,-6,INT_MIN,INT_MAX,0x12345678,-0x12345678} x
#   divisors  {0,1,-1,0x80000000,3,-3,7} + {+2^k,-2^k : k=1..30}  (2^31 = 0x80000000)
# in four operand forms (li => the recompiler const-tracks the operand,
# lw from a data table => it cannot):
#   cc  rs=li rt=li   (both const: folded)
#   mc  rs=lw rt=li   (const rt: /0, /1, power-of-two, x1/-1 shortcuts)
#   cm  rs=li rt=lw   (const rs)
#   mm  rs=lw rt=lw   (control: generic path)
# The interpreter is the reference; mipsrec must store identical LO/HI for
# every case, with and without faultmem (HX_FM=1).
# Env: S scratch dir, W tree with built objects (build-erosq-pgo-xb.sh), NOBUILD=1.
# Mutant check (recipe, not a switch): break one const path in rec_mdu.cpp.h,
# recompile only recompiler.o with its command line from the build log
#   grep -E -- '-c libpcsxcore/mipsrec/recompiler.cpp' BUILDLOG | tail -1 | sh
# rerun divtest.sh (must FAIL), restore the file and recompiler.o. 2026-10-20:
#   /0 LO back to MOVN(TEMP_1,TEMP_2,TEMP_1) -> 22 fails (div cc+mc, rt=0)
#   drop the pot bias SRL+ADDU           -> 418 fails (div mc)
#   both-const fold back to u32 '/'       -> 393 fails (div cc)
set -u
S=${S:-/tmp/divtest}
W=${W:-$(cd "$(dirname "$0")/../../.." && pwd)}
OFF=$W/libpcsxcore/mipsrec/offline
export PATH=$EROSQ_ROOT/buildroot-2026.08/output/host/bin:$PATH
QEMU="qemu-mipsel-static -L $EROSQ_ROOT/buildroot-2026.08/output/host/mipsel-buildroot-linux-musl/sysroot"
mkdir -p $S
if [ -z "${NOBUILD:-}" ]; then
  ( cd $W
    OBJS=$(ls libpcsxcore/*.o libpcsxcore/mipsrec/*.o | grep -v emu_if)
    mipsel-buildroot-linux-musl-gcc -O2 -DNO_PNG -Iinclude -Ilibpcsxcore -fno-pic -mno-abicalls -G0 -c $OFF/divtest.c -o $S/divtest.o &&
    mipsel-buildroot-linux-musl-gcc -O2 -fno-pic -mno-abicalls -G0 -c $OFF/stubs.c -o $S/stubs.o &&
    mipsel-buildroot-linux-musl-g++ -no-pie -Wl,--gc-sections -o $S/divtest $S/divtest.o $S/stubs.o $OBJS -lz -lm -lpthread 2>$S/link.err ) ||
    { echo "build failed"; grep -v abicalls $S/link.err | head; exit 1; }
fi

python3 - $S/t.S $S/cases.txt <<'EOF' || exit 1
import sys
A = [0, 1, -1, 7, -7, 6, -6, -0x80000000, 0x7fffffff, 0x12345678, -0x12345678]
B = [0, 1, -1, -0x80000000, 3, -3, 7] + [s * (1 << k) for k in range(1, 31) for s in (1, -1)]
OPS = ['div', 'divu', 'mult', 'multu']
asm = ['\t.set noreorder', '\t.set noat', '\t.text', '\t.globl test', 'test:',
       '\tlui $s0, 0x8010', '\tla $s1, dtab']
cases = []
def ld(reg, form, val, idx):
    if form == 'c': asm.append('\tli %s, %d' % (reg, val))
    else: asm.extend(['\tlw %s, %d($s1)' % (reg, 4 * idx), '\tnop'])
for op in OPS:
    for form in ('cc', 'mc', 'cm', 'mm'):
        for ia, a in enumerate(A):
            for ib, b in enumerate(B):
                i = len(cases)
                if i % 2048 == 0 and i: asm.append('\taddiu $s0, $s0, 16384')
                off = 8 * (i % 2048)
                ld('$t0', form[0], a, ia)
                ld('$t1', form[1], b, len(A) + ib)
                asm.append('\t%s $t0, $t1' % op if op.startswith('mult') else '\t%s $zero, $t0, $t1' % op)
                asm += ['\tmfhi $t2', '\tmflo $t3', '\tsw $t3, %d($s0)' % off, '\tsw $t2, %d($s0)' % (off + 4)]
                cases.append('%s %s rs=%#010x rt=%#010x' % (op, form, a & 0xffffffff, b & 0xffffffff))
asm += ['\tlui $t9, 0x8018', '\tli $t8, 0x600dc0de', '\tsw $t8, 0($t9)', '\tjr $ra', '\tnop',
        '\t.align 2', 'dtab:'] + ['\t.word %d' % (v & 0xffffffff) for v in A + B]
open(sys.argv[1], 'w').write('\n'.join(asm) + '\n')
open(sys.argv[2], 'w').write(''.join('%d %s\n' % (i, c) for i, c in enumerate(cases)))
EOF
N=$(wc -l < $S/cases.txt)
mipsel-buildroot-linux-musl-gcc -mips1 -msoft-float -mno-abicalls -fno-pic -G0 -c $S/t.S -o $S/t.o 2>$S/as.err || { echo "asm failed"; head -3 $S/as.err; exit 1; }
mipsel-buildroot-linux-musl-ld -Ttext=0x80010000 -e test -o $S/t.elf $S/t.o && mipsel-buildroot-linux-musl-objcopy -O binary -j .text $S/t.elf $S/t.bin || exit 1

timeout 300 $QEMU $S/divtest $S/t.bin $N int > $S/int.txt 2>/dev/null
grep -q 'end=done' $S/int.txt || { echo "reference did not finish: $(tail -1 $S/int.txt)"; exit 1; }
rc=0
for fm in 0 1; do
  fmenv="EROSQ_FAULTMEM=0"; [ $fm = 1 ] && fmenv="HX_FM=1"
  env $fmenv timeout 300 $QEMU $S/divtest $S/t.bin $N rec 2>/dev/null > $S/rec$fm.txt
  grep -q 'end=done' $S/rec$fm.txt || { echo "fm$fm: rec did not finish: $(tail -1 $S/rec$fm.txt)"; rc=1; continue; }
  awk 'FILENAME==ARGV[1] {d=$0; sub(/^[0-9]+ /, "", d); c[$1]=d; next}
       FILENAME==ARGV[2] {if ($1=="R") {lo[$2]=$3; hi[$2]=$4}; next}
       $1=="R" && (lo[$2]!=$3 || hi[$2]!=$4) {printf "  %s  int lo=%s hi=%s  rec lo=%s hi=%s\n", c[$2], lo[$2], hi[$2], $3, $4}' \
    $S/cases.txt $S/int.txt $S/rec$fm.txt > $S/mis$fm.txt
  nb=$(wc -l < $S/mis$fm.txt)
  echo "fm$fm: cases=$N mismatches=$nb"
  [ $nb = 0 ] || { rc=1; awk '{print $1, $2}' $S/mis$fm.txt | sort | uniq -c | sed 's/^/   by op\/form: /'; head -${SHOW:-40} $S/mis$fm.txt; }
done
[ $rc = 0 ] && echo "divtest: PASS ($N cases x 2 configs)" || echo "divtest: FAIL"
exit $rc
