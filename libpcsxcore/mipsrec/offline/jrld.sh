#!/bin/bash
# jrld.sh -- EROSQ_JRLD fixture (perf/jrld-nclip, 2026-10-23): jr / jalr with a
# load in the delay slot, JIT vs interpreter, both switch states.
# For each KIND (jr, jalr rd=ra, jalr rd=t5) x CASE (gen_jrld.py: target reads
# rt / reads+writes rt (load skipped) / writes rt / ignores rt / branch, jump,
# jr at the target / unmapped target / AdEL load / rt == jump reg / base ==
# jump reg / lb lhu lwl lwr / rt == $zero / link == rt / SMC rewrite of the
# target between two passes of the same compiled jump block / I/O load /
# BIOS target (hlink HX_ROMLOOP)),
# runs the program through hlink in four arms:
#   j1 EROSQ_JRLD=1   j0 EROSQ_JRLD=0   (JIT, faultmem on; FM=0 for off)
#   ix interpreter with exact AdEL (HX_EXACT=1)   i interpreter as configured
# and compares final GPRs + RAM (hlink STATE gpr/ram/end; the exception
# handler stores EPC, Cause&0x8000007c, BadVAddr into hashed RAM).
# PASS per case: j1 == j0, except where EXPECT_J1_NE_J0 lists the case (then
# j1 must equal ix). JIT-vs-interpreter results are reported per case.
# Env: S scratch dir, W tree with built objects, NOBUILD=1, FM (faultmem 1/0).
set -u
EROSQ_ROOT=${EROSQ_ROOT:-$(cd "$(dirname "$0")/../../../.." && pwd)}
S=${S:-${TMPDIR:-/tmp}/erosq-jrld}
W=${W:-$(cd "$(dirname "$0")/../../.." && pwd)}
OFF=$W/libpcsxcore/mipsrec/offline
export PATH=$EROSQ_ROOT/buildroot-2026.08/output/host/bin:$PATH
QEMU="qemu-mipsel-static -L $EROSQ_ROOT/buildroot-2026.08/output/host/mipsel-buildroot-linux-musl/sysroot"
mkdir -p $S
if [ -z "${NOBUILD:-}" ]; then
  ( cd $W
    OBJS=$(ls libpcsxcore/*.o libpcsxcore/mipsrec/*.o | grep -v emu_if)
    mipsel-buildroot-linux-musl-gcc -O2 -DNO_PNG -Iinclude -Ilibpcsxcore -fno-pic -mno-abicalls -G0 -c $OFF/hlink.c -o $S/hlink.o &&
    mipsel-buildroot-linux-musl-gcc -O2 -fno-pic -mno-abicalls -G0 -c $OFF/stubs.c -o $S/stubs.o &&
    mipsel-buildroot-linux-musl-g++ -no-pie -Wl,--gc-sections -o $S/hlink $S/hlink.o $S/stubs.o $OBJS -lz -lm -lpthread 2>$S/link.err ) ||
    { echo "build failed"; grep -v abicalls $S/link.err | head; exit 1; }
fi
# the AdEL load in a jr delay slot: the helper interprets it with the
# interpreter's non-exact table (aligned-down read, no exception); the native
# path raises the exact AdEL as every other JIT load does.
EXPECT_J1_NE_J0=${EXPECT_J1_NE_J0:-"jr:adel"}
CASES="reads readwrite writes ignores immcoll branch jump jrtarget unmapped adel rtisrs baseisrs lb lhu lwl lwr rtzero linkisrt linkisrt5 smc io rom"
key() { echo "$1" | awk '/^STATE/{print $2, $3, $9}'; }
pass=0; fail=0; n=0; jsame=0; jdiff=0
for kind in jr jalr jalrt; do
  for c in $CASES; do
    python3 $OFF/gen_jrld.py $kind $c > $S/t.S &&
    mipsel-buildroot-linux-musl-gcc -mips1 -msoft-float -mno-abicalls -fno-pic -G0 -c $S/t.S -o $S/t.o 2>$S/as.err &&
    mipsel-buildroot-linux-musl-ld -Ttext=0x80010000 -e test -o $S/t.elf $S/t.o &&
    mipsel-buildroot-linux-musl-objcopy -O binary -j .text $S/t.elf $S/t.bin || { echo "  $kind:$c build FAIL"; cat $S/as.err | head -3; fail=$((fail+1)); continue; }
    rl=; [ $c = rom ] && rl=HX_ROMLOOP=1
    run() { env "$@" $rl HX_BUDGET=50 timeout ${HX_TIMEOUT:-120} $QEMU $S/hlink $S/t.bin 2>&1 | grep '^STATE' || echo "STATE crash-or-timeout"; }
    j1=$(run HX_APPLYCFG=1 EROSQ_FAULTMEM=${FM:-1} EROSQ_JRLD=1); j0=$(run HX_APPLYCFG=1 EROSQ_FAULTMEM=${FM:-1} EROSQ_JRLD=0)
    ix=$(run HX_INT=1 HX_EXACT=1); i=$(run HX_INT=1)
    k1=$(key "$j1"); k0=$(key "$j0"); kx=$(key "$ix"); ki=$(key "$i")
    n=$((n+1)); res=PASS
    case "$j1" in *crash-or-timeout*|*end=cap*|*end=running*) res="FAIL(noterm)";; esac
    if [[ " $EXPECT_J1_NE_J0 " == *" $kind:$c "* ]]; then
      [ "$k1" != "$k0" ] && [ "$k1" = "$kx" ] || res="FAIL(expected j1!=j0, j1==ix)"
    else
      [ "$k1" = "$k0" ] || res="FAIL(j1!=j0)"
    fi
    [ $res = PASS ] && pass=$((pass+1)) || fail=$((fail+1))
    vs="j1==ix:$([ "$k1" = "$kx" ] && echo y || echo n) j1==i:$([ "$k1" = "$ki" ] && echo y || echo n) j0==i:$([ "$k0" = "$ki" ] && echo y || echo n)"
    [ "$k1" = "$kx" ] && jsame=$((jsame+1)) || jdiff=$((jdiff+1))
    printf "  %-6s %-10s %-4s %s  [j1 %s]\n" $kind $c $res "$vs" "$k1"
  done
done
echo "jrld: cases=$n pass=$pass fail=$fail (JIT(jrld=1) == exact interpreter in $jsame, differs in $jdiff; faultmem=${FM:-1})"
exit $fail
