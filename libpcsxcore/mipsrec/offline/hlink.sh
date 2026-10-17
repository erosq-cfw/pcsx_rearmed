#!/bin/bash
# hlink.sh N [FIRST_SEED]   random programs, per-class pass counts
# hlink.sh ctl              exact-count controls per linked site kind (below)
# Block-linking differential test: generate N programs (gen_link.py), run each
# through the real mipsrec in the SAME binary with EROSQ_BLOCKLINK=0 and =2 (HL_MODE=4: trampoline)
# (qemu-user), compare final GPRs / RAM hash / psxRegs.cycle / pc / horizon
# count. A =2 run that took no linked exit FAILS (mipsrec_link_taken == 0);
# while that counter isn't exported, set LINK_COUNTER_OPTIONAL=1.
# Per seed the case mix varies: term or spin (stop inside a linked loop),
# horizon budget 7..400 cycles, start cycle 0 or just below the u32 wrap.
# Failures are minimised (fewer blocks / steps, same seed) and kept in
# $S/hl/fail-<seed>.S with a one-line repro command.
# Env: S (scratch dir), W (pcsx tree with built objects), NOBUILD=1,
# HX_TIMEOUT (s per run, 120), MINIMISE=0 (skip minimising, for mutants).
set -u
S=${S:-/tmp/hl}
W=${W:-$(cd "$(dirname "$0")/../../.." && pwd)}
OFF=$W/libpcsxcore/mipsrec/offline
export PATH=buildroot-2026.08/output/host/bin:$PATH
QEMU="qemu-mipsel-static -L buildroot-2026.08/output/host/mipsel-buildroot-linux-musl/sysroot"
mkdir -p $S
if [ -z "${NOBUILD:-}" ]; then
  ( cd $W
    OBJS=$(ls libpcsxcore/*.o libpcsxcore/mipsrec/*.o | grep -v emu_if)
    mipsel-buildroot-linux-musl-gcc -O2 -DNO_PNG -Iinclude -Ilibpcsxcore -fno-pic -mno-abicalls -G0 -c $OFF/hlink.c -o $S/hlink.o &&
    mipsel-buildroot-linux-musl-gcc -O2 -fno-pic -mno-abicalls -G0 -c $OFF/stubs.c -o $S/stubs.o &&
    mipsel-buildroot-linux-musl-g++ -no-pie -Wl,--gc-sections -Wl,--wrap=gen_interupt -o $S/hlink $S/hlink.o $S/stubs.o $OBJS -lz -lm -lpthread 2>$S/link.err ) || { echo "build failed"; cat $S/link.err | grep -v abicalls | head; exit 1; }
fi

# params for a seed: mode nblocks steps budget cycle0 stop_after
params() {
  local s=$1
  local mode=term; [ $((s % 5)) -eq 0 ] && mode=spin
  local nb=$((3 + s % 8))                 # 3..10 blocks
  local steps=$((60 + (s * 37) % 400))
  local budget=$((16 + (s * 53) % 385))   # 16..400 cycles (blocks here are 6..60)
  [ $((s % 7)) -eq 3 ] && budget=100000   # "big": horizon rarely fires -> most exits link
  local c0=0; [ $((s % 3)) -eq 1 ] && c0=$((0xffffffff - (s * 911) % 30000))
  local sa=0; [ $mode = spin ] && sa=$((50 + s % 200))
  echo "$mode $nb $steps $budget $c0 $sa"
}

# run one config; prints "PASS"/"FAIL <why>"
one() {
  local s=$1 mode=$2 nb=$3 steps=$4 budget=$5 c0=$6 sa=$7 tag=$8
  python3 $OFF/gen_link.py $s $nb $steps $mode > $S/$tag.S || { echo "FAIL gen"; return; }
  mipsel-buildroot-linux-musl-gcc -mips1 -msoft-float -mno-abicalls -fno-pic -G0 -c $S/$tag.S -o $S/$tag.o 2>$S/$tag.aserr || { echo "FAIL asm"; return; }
  mipsel-buildroot-linux-musl-ld -Ttext=0x80010000 -e test -o $S/$tag.elf $S/$tag.o && mipsel-buildroot-linux-musl-objcopy -O binary -j .text $S/$tag.elf $S/$tag.bin
  local env="HX_BUDGET=$budget HX_CYCLE0=$c0 HX_STOP_AFTER=$sa ${XENV:-}"
  local a b
  a=$(env $env EROSQ_BLOCKLINK=0 timeout ${HX_TIMEOUT:-120} $QEMU $S/hlink $S/$tag.bin 2>&1 | grep '^STATE' || echo "STATE crash-or-timeout")
  b=$(env $env EROSQ_BLOCKLINK=${HL_MODE:-2} timeout ${HX_TIMEOUT:-120} $QEMU $S/hlink $S/$tag.bin 2>&1 | grep '^STATE' || echo "STATE crash-or-timeout")
  local ka=$(echo "$a" | sed 's/ link_taken=[^ ]*//') kb=$(echo "$b" | sed 's/ link_taken=[^ ]*//')
  local lt=$(echo "$b" | grep -o 'link_taken=[^ ]*' | cut -d= -f2)
  if [ "$ka" != "$kb" ]; then echo "FAIL diff off:[$a] on:[$b]"; return; fi
  case "$a" in *crash-or-timeout*|*end=cap*|*end=running*) echo "FAIL noterm [$a]"; return;; esac
  if [ -n "${EXPECT:-}" ]; then
    [ "$lt" = "$EXPECT" ] && echo "PASS $b" || echo "FAIL taken=$lt expected=$EXPECT [$b]"; return
  fi
  if [ "$lt" = "na" ]; then
    [ -n "${LINK_COUNTER_OPTIONAL:-}" ] || { echo "FAIL no-link-counter (mipsrec_link_taken not exported)"; return; }
  elif [ "$lt" = "0" ]; then echo "FAIL no-linked-exit-taken [$b]"; return; fi
  echo "PASS $b"
}

if [ "${1:-}" = ctl ]; then
  # one constant-target exit per iteration, everything else indirect:
  # taken must equal ITERS-1 for linked kinds and 0 for the exclusions.
  # long<K>: cycle_multiplier 0x2000 = 32 cycles/insn; block = addiu + K + j + nop.
  IT=50; L=$((IT - 1)); cf=0   # first pass: target not compiled yet -> slot 0 -> loop
  for spec in j:$L b:$L jal:$L bal:$L bxxz:$L bxx:$L dt2-j:$L dt2-bxx:$L \
              dt2-jal:0 dt2-bal:0 dt2-bxxz:0 self:0 long1020:$L long1021:0 j:$L:4294967040 bxx:$L:4294967040; do   # last two: start 256 cycles below the u32 wrap
    IFS=: read k x c0 <<< "$spec"; c0=${c0:-0}
    case $k in long*) XENV="HX_MULT=0x2000"; bud=4000000;; *) XENV=; bud=100000;; esac
    r=$(XENV=$XENV EXPECT=$x one 1 ctl:$k 0 $IT $bud $c0 0 c); [[ $r == PASS* ]] || cf=$((cf+1))
    printf "  ctl %-9s c0=%-10s expect=%-3s %s\n" $k $c0 $x "$r"
  done
  echo "hlink ctl: fail=$cf"; exit $cf
fi

N=${1:-20}; F=${2:-1}
pass=0; fail=0; declare -A cp cn ct
for s in $(seq $F $((F + N - 1))); do
  read mode nb steps budget c0 sa <<< "$(params $s)"
  r=$(one $s $mode $nb $steps $budget $c0 $sa t)
  cl=$mode; [ $c0 != 0 ] && cl=$mode-wrap; [ $budget = 100000 ] && cl=$cl-big; cn[$cl]=$((${cn[$cl]:-0}+1))
  if [[ $r == PASS* ]]; then pass=$((pass+1)); cp[$cl]=$((${cp[$cl]:-0}+1))
    t=$(echo "$r" | grep -o 'link_taken=[0-9]*' | cut -d= -f2); ct[$cl]="${ct[$cl]:-} $t"; [ -n "${VERBOSE:-}" ] && echo "seed $s ok ($mode nb=$nb steps=$steps budget=$budget c0=$c0) $r"
  else
    fail=$((fail+1)); echo "seed $s: $r"
    [ "${MINIMISE:-1}" = 0 ] && continue
    # minimise: shrink blocks then steps while it still fails the same way
    bn=$nb; bs=$steps
    for t in 1 2 3 4 5 6; do
      nn=$((bn > 3 ? bn - 1 : bn)); ns=$((bs > 20 ? bs / 2 : bs))
      r2=$(one $s $mode $nn $bs $budget $c0 $sa m); [[ $r2 == FAIL* ]] && bn=$nn
      r2=$(one $s $mode $bn $ns $budget $c0 $sa m); [[ $r2 == FAIL* ]] && bs=$ns
    done
    one $s $mode $bn $bs $budget $c0 $sa m > /dev/null
    cp $S/m.S $S/fail-$s.S
    echo "  minimised: gen_link.py $s $bn $bs $mode ; HX_BUDGET=$budget HX_CYCLE0=$c0 HX_STOP_AFTER=$sa -> $S/fail-$s.S"
  fi
done
for cl in "${!cn[@]}"; do
  echo "  class $cl: ${cp[$cl]:-0}/${cn[$cl]} pass; link_taken min/median/max = $(echo ${ct[$cl]:-} | tr ' ' '\n' | sort -n | awk '{a[NR]=$1} END{if(NR) print a[1]"/"a[int((NR+1)/2)]"/"a[NR]; else print "-"}')"
done
echo "hlink: pass=$pass fail=$fail"
