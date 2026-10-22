#!/bin/bash
# idleskip.sh -- EROSQ_IDLESKIP differential cases on the hlink harness (qemu-user).
# Each case is a guest program holding one idle shape (V / I / A, jitsched.cpp);
# it runs with EROSQ_IDLESKIP=0 and =1 (x JITSCHED 0/1 x BLOCKLINK 0/3) and the
# STATE lines must be identical. "on" runs must also show the expected skip
# behaviour from the helper's report:
#   v-timeout  V loop runs into its timeout counter (cap): skips > 0
#   v-vcount   V loop, vcount patched at horizon 3 (HX_DMAPATCH): skips > 0
#   v-ape      V loop with Ape's shorter timeout path (M at L+0x48), to the cap
#   a-patch    A loop, polled word patched at horizon 3: skips > 0
#   i-patch    I loop, callee's 'li v0,-1' patched to 'li v0,0' at horizon 3
#              (SMC, blkext invalidates the callee block): loop exits exactly
#              as without the skip
#   i-smc      callee patched to 'li v0,-2' at horizon 3: the loop keeps
#              spinning but no longer matches -- every later entry re-checks
#              (nomatch > 0) and no skip happens after the patch: one skip per horizon
#              period before it (skips <= 3 of 20 periods)
#   i-nomatch  I loop whose callee returns 0 from the start: never skips
# Env: S (scratch dir), W (pcsx tree with built objects), NOBUILD=1 (reuse $S/hlink).
# Mutant check (2026-10-22): jitsched.o built with -DIDLESKIP_MUTANT (charge C+1)
# linked into a separate $S/hlink -> 16/20 skipping runs FAIL (v-timeout, v-vcount,
# a-patch, i-smc; i-patch's exit re-aligns and still matches).
set -u
S=${S:-/tmp/idleskip.$$}
W=${W:-$(cd "$(dirname "$0")/../../.." && pwd)}
OFF=$W/libpcsxcore/mipsrec/offline
export PATH=buildroot-2026.08/output/host/bin:$PATH
QEMU="qemu-mipsel-static -L buildroot-2026.08/output/host/mipsel-buildroot-linux-musl/sysroot"
mkdir -p $S
[ -n "${NOBUILD:-}" ] || S=$S NOBUILD= bash $OFF/hlink.sh 0 > $S/build.log 2>&1   # builds $S/hlink (0 seeds)
[ -x $S/hlink ] || { echo "no hlink binary"; cat $S/build.log; exit 1; }

DATA=0x80031000   # hlink pre-fills DATA with nonzero words; DATA+4 = 0x3c6ef372
DONE_ASM='lui $8,0x600d; ori $8,$8,0xc0de; lui $9,0x8003; sw $8,0($9); 9: b 9b; nop'

prog() {   # prog NAME -> $S/NAME.bin (assembled at 0x80010000)
  local n=$1
  mipsel-buildroot-linux-musl-gcc -mips1 -msoft-float -mno-abicalls -fno-pic -G0 -c $S/$n.S -o $S/$n.o 2>$S/$n.err &&
  mipsel-buildroot-linux-musl-ld -Ttext=0x80010000 -e start -o $S/$n.elf $S/$n.o &&
  mipsel-buildroot-linux-musl-objcopy -O binary -j .text $S/$n.elf $S/$n.bin || { echo "asm fail $n"; cat $S/$n.err; exit 1; }
}

# V: libetc VSync wait with timeout counter (L at 'L', M at L+0x4c)
v_prog() {   # v_prog NAME COUNTER [PADNOPS]  (5: M at L+0x4c as Crash/Spyro; 4: L+0x48 as Ape)
cat > $S/$1.S <<EOF
.set noreorder
.set noat
.globl start
start:
  addiu \$29,\$29,-32
  li \$8,$2
  sw \$8,16(\$29)
  lui \$4,0x7fff
  ori \$4,\$4,0xffff
  li \$3,-1
L:
  lw \$2,16(\$29)
  nop
  addiu \$2,\$2,-1
  sw \$2,16(\$29)
  lw \$2,16(\$29)
  nop
  bne \$2,\$3,M
  nop
  $DONE_ASM
  $(for i in $(seq ${3:-5}); do printf 'nop; '; done)
M:
  lui \$2,0x8003
  lw \$2,0x1004(\$2)
  nop
  slt \$2,\$2,\$4
  bnez \$2,L
  nop
  $DONE_ASM
EOF
prog $1
}

# A: GT2 VSync wait (self loop)
cat > $S/a.S <<EOF
.set noreorder
.set noat
.globl start
start:
  lui \$4,0x8003
  ori \$4,\$4,0x1000
  lui \$5,0x7fff
  ori \$5,\$5,0xffff
A:
  lw \$2,4(\$4)
  nop
  slt \$2,\$2,\$5
  bnez \$2,A
  nop
  $DONE_ASM
EOF
prog a

# I: MGS idle thread; T = jr ra; li v0,RET
i_prog() {   # i_prog NAME RET
cat > $S/$1.S <<EOF
.set noreorder
.set noat
.globl start
start:
  addiu \$29,\$29,-24
H:
  lui \$2,0x8003
  lw \$2,0x1000(\$2)
  nop
  beqz \$2,H+8
  nop
  jal T
  nop
  addu \$16,\$2,\$0
  bltz \$16,H
  nop
  $DONE_ASM
T:
  jr \$31
  addiu \$2,\$0,$2
EOF
prog $1
}
v_prog v-timeout 20000
v_prog v-vcount 0x7fffffff
v_prog v-ape 20000 4
i_prog i-patch -1
i_prog i-nomatch 0

# guest words the shapes must assemble to (else the cases test nothing)
chk() { mipsel-buildroot-linux-musl-objdump -d $S/$1.elf | grep -q "$2" || { echo "FAIL $1: shape word $2 missing"; exit 1; }; }
chk v-timeout 1443000c; chk v-timeout 1440ffe8; chk v-ape 1443000b; chk v-ape 1440ffe9; chk a 1440fffc; chk i-patch 0600fff7; chk i-patch 2402ffff; chk i-patch 00408021

T_ADDR=$(mipsel-buildroot-linux-musl-nm $S/i-patch.elf | awk '$3=="T"{print $1}')
T4=$(printf %x $((0x$T_ADDR + 4)))

pass=0; fail=0
run() {   # run CASE BIN EXPECT [ENV...]   EXPECT: skips | smc | none
  local c=$1 b=$2 e=$3; shift 3
  for js in 0 1; do for bl in 0 3; do
    local env="HX_BUDGET=50000 HX_STOP_AFTER=${STOP:-400} EROSQ_JITSCHED=$js EROSQ_BLOCKLINK=$bl $*"
    local o=$(env $env EROSQ_IDLESKIP=0 timeout 120 $QEMU $S/hlink $S/$b.bin 2>/dev/null | grep '^STATE')
    local n=$(env $env EROSQ_IDLESKIP=1 timeout 120 $QEMU $S/hlink $S/$b.bin 2>$S/$c.err | grep '^STATE')
    local sk=$(awk '/idleskip [VIA]:/{s+=$8} END{print s+0}' $S/$c.err)
    local nm=$(awk '/nomatch-calls/{print $3}' $S/$c.err)
    local ok=1
    [ -n "$o" ] && [ "$o" = "$n" ] || ok=0
    case $e in skips) [ "$sk" -gt 0 ] || ok=0;; smc) [ "$sk" -gt 0 ] && [ "$sk" -le 3 ] && [ "${nm:-0}" -gt 0 ] || ok=0;; none) [ "$sk" = 0 ] || ok=0;; esac
    if [ $ok = 1 ]; then pass=$((pass+1)); else fail=$((fail+1)); fi
    printf "%-10s js=%s bl=%s %s skips=%s nomatch=%s | %s\n" $c $js $bl $([ $ok = 1 ] && echo PASS || echo FAIL) $sk ${nm:-0} "${n:-<none>}"
  done; done
}
run v-timeout v-timeout skips
run v-vcount  v-vcount  skips   HX_DMAPATCH=80031004:7fffffff:3
run v-ape     v-ape     skips
run a-patch   a         skips   HX_DMAPATCH=80031004:7fffffff:3
run i-patch   i-patch   skips   HX_DMAPATCH=$T4:24020000:3
STOP=20 run i-smc i-patch smc   HX_DMAPATCH=$T4:2402fffe:3
run i-nomatch i-nomatch none
echo "idleskip: pass=$pass fail=$fail"
