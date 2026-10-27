#!/bin/bash
EROSQ_ROOT=${EROSQ_ROOT:-$(cd "$(dirname "$0")/../../../.." && pwd)}
# smcrun.sh N [FIRST_SEED] -- SMC stress (gen_smc.py + smcstress.c, ideas/jitsize-pageprot).
# Per seed: the interpreter is the reference; mipsrec must match it (final
# GPRs, code/data RAM hash, v1, end=done) in every config below, each with
# and without faultmem (EROSQ_FAULTMEM=0). Page-protection configs must also have taken
# protection faults (pp field), so the path under test really ran.
#   inline   EROSQ_PAGEPROT=0           (the existing inline invalidation)
#   pp       page protection, default storm threshold
#   storm    EROSQ_PPSTORM=2            (storms + verify prologues)
# Env: S scratch dir, W tree with built objects, NOBUILD=1, CONFIGS to override.
set -u
S=${S:-${TMPDIR:-/tmp}/erosq-jitsize-smc}
W=${W:-$(cd "$(dirname "$0")/../../.." && pwd)}
OFF=$W/libpcsxcore/mipsrec/offline
export PATH=$EROSQ_ROOT/buildroot-2026.08/output/host/bin:$PATH
QEMU="qemu-mipsel-static -L $EROSQ_ROOT/buildroot-2026.08/output/host/mipsel-buildroot-linux-musl/sysroot"
mkdir -p $S
if [ -z "${NOBUILD:-}" ]; then
  ( cd $W
    OBJS=$(ls libpcsxcore/*.o libpcsxcore/mipsrec/*.o | grep -v emu_if)
    mipsel-buildroot-linux-musl-gcc -O2 -DNO_PNG -Iinclude -Ilibpcsxcore -fno-pic -mno-abicalls -G0 -c $OFF/smcstress.c -o $S/smcstress.o &&
    mipsel-buildroot-linux-musl-gcc -O2 -fno-pic -mno-abicalls -G0 -c $OFF/stubs.c -o $S/stubs.o &&
    mipsel-buildroot-linux-musl-g++ -no-pie -Wl,--gc-sections -o $S/smcstress $S/smcstress.o $S/stubs.o $OBJS -lz -lm -lpthread 2>$S/link.err ) ||
    { echo "build failed"; grep -v abicalls $S/link.err | head; exit 1; }
fi
CONFIGS=${CONFIGS:-"inline:EROSQ_PAGEPROT=0 pp: storm:EROSQ_PPSTORM=2"}
N=${1:-50}; F=${2:-1}
pass=0; fail=0; faults=0; bd=0; storms=0; vfail=0
for s in $(seq $F $((F + N - 1))); do
  python3 $OFF/gen_smc.py $s ${ITERS:-40} > $S/t.S || { echo "seed $s: gen"; fail=$((fail+1)); continue; }
  mipsel-buildroot-linux-musl-gcc -mips1 -msoft-float -mno-abicalls -fno-pic -G0 -c $S/t.S -o $S/t.o 2>$S/as.err || { echo "seed $s: asm"; cat $S/as.err | head -3; fail=$((fail+1)); continue; }
  mipsel-buildroot-linux-musl-ld -Ttext=0x80010000 -e test -o $S/t.elf $S/t.o && mipsel-buildroot-linux-musl-objcopy -O binary -j .text $S/t.elf $S/t.bin
  declare -A sym=()
  while read a t n; do sym[$n]=$a; done < <(mipsel-buildroot-linux-musl-nm $S/t.elf)
  sym[HWREG]=1f801074
  : > $S/plan.txt
  grep -o '#TAB [0-9]* [A-Za-z0-9]*' $S/t.S | while read _ i n; do echo "TAB $i ${sym[$n]}"; done >> $S/plan.txt
  grep -o '#DMA [A-Za-z0-9]* 0x[0-9a-f]*' $S/t.S | while read _ n w; do echo "DMA ${sym[$n]} ${w#0x}"; done >> $S/plan.txt
  echo "P2 ${sym[P2]}" >> $S/plan.txt
  ref=$(timeout 120 $QEMU $S/smcstress $S/t.bin $S/plan.txt int 2>&1 | grep '^STATE' || echo "STATE crash")
  case "$ref" in *end=done*) ;; *) echo "seed $s: reference did not finish [$ref]"; fail=$((fail+1)); continue;; esac
  kref=$(echo "$ref" | sed 's/ pp=.*//')
  bad=""
  for c in $CONFIGS; do
    name=${c%%:*}; env=${c#*:}
    for fm in 0 1; do
      fmenv="EROSQ_FAULTMEM=0"; [ $fm = 1 ] && fmenv="HX_FM=1"   # fm0: classic dual-path series (stores in host delay slots)
      out=$(env $env $fmenv timeout 120 $QEMU $S/smcstress $S/t.bin $S/plan.txt rec 2>&1 | grep '^STATE' || echo "STATE crash")
      k=$(echo "$out" | sed 's/ pp=.*//')
      [ "$k" = "$kref" ] || { bad="$bad $name/fm$fm:[$out]"; continue; }
      pp=$(echo "$out" | grep -o 'pp=.*' | cut -d= -f2)
      IFS=/ read pf pb ps pu pv pr pvb <<< "$pp"
      if [ $name != inline ]; then
        [ "${pf:-0}" -gt 0 ] || bad="$bad $name/fm$fm:no-faults"
        faults=$((faults + pf)); bd=$((bd + pb)); storms=$((storms + ps)); vfail=$((vfail + pv))
      fi
    done
  done
  if [ -z "$bad" ]; then pass=$((pass+1)); [ -n "${VERBOSE:-}" ] && echo "seed $s ok $kref";
  else fail=$((fail+1)); echo "seed $s: ref [$ref] mismatch:$bad"; cp $S/t.S $S/fail-$s.S; cp $S/plan.txt $S/fail-$s.plan; fi
done
echo "smcstress: pass=$pass fail=$fail  (pp faults=$faults bd=$bd storms=$storms verifyfail=$vfail)"
