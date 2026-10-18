#!/usr/bin/env python3
"""gen_smc.py SEED [ITERS] -> MIPS-I guest asm (SMC stress, ideas/jitsize-pageprot).

Well-defined self-modifying code for BOTH the interpreter (sees every store
at once) and mipsrec (a block start's slot is zeroed by a store to that
word; re-entry through a lookup or a linked exit recompiles it):
only FIRST words of blocks are patched, and every patched block is entered
from ANOTHER block (jal / j), never through its own fastpath self-loop.

  targets T0..T{n-1}  leaf blocks "addiu v1,v1,K ; xor v1,v1,s2 ; jr ra"
                      in one code page, with data words in the SAME page
  driver loop         per iteration: build a new "addiu v1,v1,imm" word,
                      patch a target's first word with sw / sh / sb /
                      swr+swl (random width), optionally from a branch
                      delay slot, through a NON-const base (loaded from a
                      table), do several data stores into the code page
                      (non-const and const bases), then jal the target(s)
  ping-pong           blocks A and B (same page): A patches B's first word,
                      j B; B patches A's, j A -- a self-patching loop
  phase 2 (DMA)       the harness overwrites target first words from the C
                      side (psxM + psxCpu->Clear, as CD DMA does) and
                      re-enters at P2, which calls every target once more
Ends by writing DONE_MAGIC to DONE and jumping to SENT.
DMA table at DMAT: n, then n x (addr, word); P2 entry address at DMAT-4.
"""
import random, sys

seed = int(sys.argv[1])
iters = int(sys.argv[2]) if len(sys.argv) > 2 else 40
R = random.Random(seed)
DONE, DONE_MAGIC = 0x80030000, 0x600dc0de
TAB = 0x80032000          # base-address table (non-const bases)
DMAT = 0x80033000
NT = R.randint(2, 5)
o = []
e = o.append


def li(r, v):
    e(f"  lui {r}, 0x{(v >> 16) & 0xffff:x}")
    e(f"  ori {r}, {r}, 0x{v & 0xffff:x}")


e("  .set noreorder\n  .set noat\n  .text\n  .globl test\ntest:")
e("  move $v1, $zero")
e(f"  li $s2, 0x{R.randint(1, 0x7fff):x}")
li("$s3", TAB)
e(f"  li $s0, {iters}")
e("D:")      # driver loop head (entered from R_k blocks, not by a self-branch)
for it in range(R.randint(1, 3)):
    t = R.randrange(NT)
    width = R.choice(["sw", "sh", "sb", "swlr"])
    e(f"  lw $t0, {4 * t}($s3)\n  nop")        # non-const base: &T_t (nop: no load-use hazard)
    if R.random() < 0.5:   # + a loaded 0: no fuzzy-RAM class -> dual-path series (store in a host BD)
        e(f"  lw $t5, {4 * (NT + 3)}($s3)\n  nop\n  addu $t0, $t0, $t5")
    e("  sll $t1, $s0, 3")
    e(f"  addiu $t1, $t1, {R.randint(1, 200)}")
    e("  andi $t1, $t1, 0xff" if width == "sb" else "  andi $t1, $t1, 0x7fff")
    li("$t2", 0x24630000)                        # addiu v1,v1,0
    e("  or $t1, $t1, $t2")
    if width == "swlr":
        e("  swr $t1, 0($t0)")
        e("  swl $t1, 3($t0)")
    elif R.random() < 0.4:                       # store in a branch delay slot
        e(f"  b 1f\n  {width} $t1, 0($t0)\n1:")
    else:
        e(f"  {width} $t1, 0($t0)")
    for d in range(R.randint(1, 6)):            # data stores into the code page
        if R.random() < 0.3:
            li("$t4", 0x80011800 + 4 * R.randrange(64))   # const base
            e(f"  sw $s0, 0($t4)")
        else:
            e(f"  lw $t3, {4 * NT}($s3)\n  nop")   # &data area (non-const)
            e(f"  {R.choice(['sw', 'sh', 'sb'])} $s0, {4 * R.randrange(64)}($t3)")
    if R.random() < 0.5:   # HW read via a non-const base: faultmem turns this block into a slow (dual-path) one
        e(f"  lw $t7, {4 * (NT + 4)}($s3)\n  nop\n  lw $t6, 0($t7)\n  nop\n  move $t6, $zero")
    e(f"  jal T{t}\n  nop")
    for t2 in R.sample(range(NT), R.randint(0, NT)):
        e(f"  jal T{t2}\n  nop")
e("  addiu $s0, $s0, -1")
e("  bgtz $s0, D\n  nop")
# ping-pong self-patching loop
e(f"  li $s1, {R.randint(5, 40)}")
e("  j A\n  nop")
e("  .org 0x1000")                               # code page 0x80011000: targets, A, B, data
for t in range(NT):
    e(f"T{t}:\n  addiu $v1, $v1, {R.randint(1, 999)}\n  xor $v1, $v1, $s2\n  jr $ra\n  nop")
for me, other, slot in (("A", "B", 4 * (NT + 1)), ("B", "A", 4 * (NT + 2))):
    e(f"{me}:\n  addiu $v1, $v1, {R.randint(1, 999)}")
    e(f"  lw $t0, {slot}($s3)\n  nop")
    if R.random() < 0.5:
        e(f"  lw $t5, {4 * (NT + 3)}($s3)\n  nop\n  addu $t0, $t0, $t5")
    e("  sll $t1, $s1, 4\n  andi $t1, $t1, 0x7fff")
    li("$t2", 0x24630000)
    e("  or $t1, $t1, $t2")
    e("  addiu $s1, $s1, -1")
    e("  blez $s1, FIN\n  nop")
    if R.random() < 0.5:
        e(f"  j {other}\n  sw $t1, 0($t0)")        # patch in the jump's delay slot
    else:
        e(f"  sw $t1, 0($t0)\n  j {other}\n  nop")
e("FIN:")
li("$t0", DONE)
li("$t1", DONE_MAGIC)
e("  sw $t1, 0($t0)")
e("FINL:\n  j 0x80020000\n  nop")
e("P2:")      # phase 2: after the C-side 'DMA' over the targets
for t in range(NT):
    e(f"  jal T{t}\n  nop")
li("$t0", DONE)
li("$t1", DONE_MAGIC)
e("  sw $t1, 0($t0)\n  j 0x80020000\n  nop")
e("  .org 0x1800")
e("  .equ HWREG, 0x1f801074")   # I_MASK: stable, timing-independent
e("DATA:")
for i in range(64):
    e(f"  .word 0x{R.getrandbits(32):08x}")
# tables (absolute addresses, filled by the harness from the symbol layout below)
src = "\n".join(o)
sys.stdout.write(src + "\n")
# side file: TAB contents and the DMA plan, as "TAB i SYMBOL" / "DMA SYMBOL WORD" / "P2" lines
plan = [f"#TAB {t} T{t}" for t in range(NT)] + [f"#TAB {NT} DATA", f"#TAB {NT + 1} B", f"#TAB {NT + 2} A", f"#TAB {NT + 4} HWREG"]   # TAB[NT+3] = 0
for t in R.sample(range(NT), R.randint(1, NT)):
    plan.append(f"#DMA T{t} 0x{0x24630000 | R.randint(1, 0x7fff):08x}")
sys.stdout.write("\n".join("  " + p for p in plan) + "\n")
