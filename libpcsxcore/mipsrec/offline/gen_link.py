#!/usr/bin/env python3
"""gen_link.py SEED [NBLOCKS] [STEPS] [MODE] -> MIPS-I guest asm on stdout.

Multi-block guest programs for the block-linking differential test (hlink).
Every block starts with
    B<i>:  addiu $v1,$v1,K     <- "patch word": SMC rewrites K (same opcode)
           addiu $s7,$s7,-1
           blez  $s7, EXIT     (step budget: every program terminates)
then a few ALU / load / store ops and one random exit:
    j / jal / b<cond> (forward, backward, self) to another block,
    fall-through into the next block, jr/jalr through a table (indirect),
    or a 'call' (jal) into a leaf block that returns with jr $ra.
SMC ops (stores into code) rewrite a block's patch word, either a jump
TARGET's first word or a block's middle patch word (B<i>_m), through a
non-const pointer (table) or a const one (lui/ori), so already-compiled,
possibly linked blocks get invalidated and re-entered.

MODE: "ctl:KIND" -> a counted control loop (see ctl() below), STEPS iterations
      "term" (default) -> EXIT writes DONE_MAGIC to DONE and jumps to SENT
      "spin"           -> EXIT is an endless 3-block linked loop; the harness
                          stops it via psxRegs.stop (menu) after N horizons.
Fixed addresses: code at 0x80010000, data/table area at TAB (below).
"""
import random, sys

seed = int(sys.argv[1])
nb = int(sys.argv[2]) if len(sys.argv) > 2 else 8
steps = int(sys.argv[3]) if len(sys.argv) > 3 else 300
mode = sys.argv[4] if len(sys.argv) > 4 else "term"
R = random.Random(seed)
DONE, DONE_MAGIC = 0x80030000, 0x600dc0de
DATA = 0x80031000

# Control programs: exactly ONE constant-target exit (site KIND) per iteration;
# every other block exit is indirect (jr), so with EROSQ_BLOCKLINK=2 the exact
# mipsrec_link_taken is known: STEPS-1 for linked kinds (pass 1 finds T uncompiled), 0 for the exclusions.
#   j b jal bal bxxz bxx  -> iJumpNormal, iJumpNormal (const beq), iJumpAL,
#                            emitBxxZ andlink, emitBxxZ, emitBxx: expect STEPS-1
#   dt2-jal dt2-bal dt2-bxxz -> dt==2 workaround (BD = load read by target): 0
#   dt2-j dt2-bxx         -> same BD trick at sites w/o the workaround: STEPS-1
#   self                  -> branch to own block start (fastpath): 0
#   long<K>               -> K filler insns before 'j T' (cycles > 0x7fff
#                            test, harness sets the cycle multiplier)
CTL_SITES = {
    "j": "  j T\n  nop", "b": "  b T\n  nop", "jal": "  jal T\n  nop",
    "bal": "  bgezal $s7, T\n  nop", "bxxz": "  bgez $s7, T\n  nop",
    "bxx": "  beq $t9, $t9, T\n  nop",
    "dt2-jal": "  jal T2\n  lw $t2, 0($s6)", "dt2-bal": "  bgezal $s7, T2\n  lw $t2, 0($s6)",
    "dt2-bxxz": "  bgez $s7, T2\n  lw $t2, 0($s6)", "dt2-j": "  j T2\n  lw $t2, 0($s6)",
    "dt2-bxx": "  beq $t9, $t9, T2\n  lw $t2, 0($s6)",
}


def ctl(kind):
    o = [".set noreorder\n.set nomacro\n.text\n.globl test\ntest:",
         "  move $s5, $ra", f"  li $s7, {steps}", "  li $v0, 0",
         "  lui $t9, %hi(ctab)\n  addiu $t9, $t9, %lo(ctab)",
         "  lw $s6, 12($t9)\n  lw $t5, 0($t9)\n  nop\n  jr $t5\n  nop",
         "LOOP:\n  addiu $s7, $s7, -1"]
    if kind == "self":
        o.append("  bgtz $s7, LOOP\n  nop")
    elif kind.startswith("long"):
        o += ["  addiu $v0, $v0, 1"] * int(kind[4:]) + ["  j T\n  nop"]
    else:
        o.append(CTL_SITES[kind])
    o.append("  lw $t5, 8($t9)\n  nop\n  jr $t5\n  nop")     # not-taken: indirect to T
    o.append("T2:\n  addu $v0, $v0, $t2")
    o.append("T:\n  addiu $v0, $v0, 3")
    # back to LOOP while s7 > 0, else EXIT -- branch-free, indirect
    o.append("  slt $t0, $zero, $s7\n  subu $t0, $zero, $t0\n  lw $t1, 0($t9)\n  lw $t2, 4($t9)\n  nop"
             "\n  subu $t1, $t1, $t2\n  and $t1, $t1, $t0\n  addu $t5, $t2, $t1\n  jr $t5\n  nop")
    o.append(f"EXIT:\n  lui $t0, {DONE >> 16:#x}\n  lui $t1, {DONE_MAGIC >> 16:#x}\n  ori $t1, $t1, {DONE_MAGIC & 0xffff:#x}"
             "\n  sw $t1, 0($t0)\n  move $ra, $s5\n  jr $ra\n  nop")
    o.append(f"  .align 4\nctab:\n  .word LOOP\n  .word EXIT\n  .word T\n  .word {DATA:#x}")
    print("\n".join(o))
    sys.exit(0)


if mode.startswith("ctl:"):
    ctl(mode[4:])

o = []
e = o.append
e(".set noreorder\n.set nomacro\n.text\n.globl test\ntest:")
e("  lui $t9, %hi(btab)\n  addiu $t9, $t9, %lo(btab)")   # const table base
e("  lui $s6, %hi(btab)\n  lw $s6, %lo(btab)+4*{}($s6)".format(nb))  # data ptr, non-const
e("  nop")
e(f"  li $s7, {steps}")
e("  li $v0, 0\n  li $v1, 0\n  li $a0, 1\n  li $a1, 2")
e("  move $s5, $ra")
# Warm-up: a 2-block linked loop (16 passes) so every program takes linked
# exits even when its random part is mostly self-loops / indirect jumps
# (hlink FAILs a run with mipsrec_link_taken == 0).
e("  li $t6, 16\n  j W0\n  nop")
e("W0:\n  addiu $t6, $t6, -1\n  j W1\n  nop")
e("W1:\n  addiu $v0, $v0, 5\n  bgtz $t6, W0\n  nop\n  j W2\n  nop")
e("W2:")
# Structured SMC loop: L1 rewrites L0's first word (the linked jump target)
# with an iteration-dependent immediate every pass, then jumps back to L0.
# Correct only if every rewrite invalidates L0 (slot reads 0 / new block).
iters = R.randrange(3, 12)
via_table = R.random() < 0.5
e(f"  li $t6, {iters}")
e("  j L0\n  nop")
e("L0:\n  addiu $v1, $v1, 1\n  addiu $t6, $t6, -1\n  blez $t6, L2\n  nop\n  j L1\n  nop")
e("L1:")
if via_table:
    e(f"  lw $t3, {nb*4 + 4 + nb*4}($t9)\n  nop")     # &L0 from the table (non-const)
else:
    e("  lui $t3, %hi(L0)\n  addiu $t3, $t3, %lo(L0)")
e("  lui $t4, 0x2463\n  andi $t5, $t6, 0xff\n  sll $t5, $t5, 3\n  or $t4, $t4, $t5")
e("  sw $t4, 0($t3)\n  j L0\n  nop")
e("L2:\n  j B0\n  nop")

ACC = ["$v0", "$a0", "$a1", "$a2", "$a3", "$t0", "$t1"]
COND = ["$v0", "$a0", "$a1"]


def alu():
    a, b = R.choice(ACC), R.choice(ACC)
    return R.choice([f"  addu {a}, {a}, {b}", f"  xor {a}, {a}, {b}",
                     f"  addiu {a}, {a}, {R.randrange(-50, 50)}",
                     f"  sll {a}, {b}, {R.randrange(0, 4)}", f"  subu {a}, {b}, {a}"])


def ds():                     # delay-slot filler (never touches $s7/$t9/$s6/$ra)
    return R.choice(["  nop", "  nop", alu()])


leaf = set(R.sample(range(1, nb), min(2, nb - 1))) if nb > 3 else set()
for i in range(nb):
    e(f"B{i}:")
    e(f"  addiu $v1, $v1, {R.randrange(1, 100)}")
    e("  addiu $s7, $s7, -1")
    e("  blez $s7, EXIT\n  nop")
    nonleaf = [x for x in range(nb) if x not in leaf]
    tgt = R.choice(nonleaf)
    for _ in range(R.randrange(1, 5)):
        r = R.random()
        if r < 0.45:
            e(alu())
        elif r < 0.6:
            e(f"  lw $t2, {R.randrange(0, 64) * 4}($s6)\n  nop\n  addu $v0, $v0, $t2")
        elif r < 0.68:
            e(f"  sw {R.choice(ACC)}, {R.randrange(0, 64) * 4}($s6)")
        else:
            # SMC: rewrite a patch word of block j (first word = jump target,
            # or the middle patch word), via the table (non-const) or const
            j = tgt if R.random() < 0.5 else R.randrange(nb)   # often the block we jump to next
            k = R.randrange(1, 1000) + 1000 * R.randrange(0, 30)
            word = 0x24630000 | (k & 0xffff)          # addiu $v1,$v1,k
            if R.random() < 0.6:
                # &B<j> (first word = jump target) or &B<j>_m (middle word)
                slot = j if R.random() < 0.6 else nb + 1 + j
                e(f"  lw $t3, {slot * 4}($t9)\n  nop")
                e(f"  lui $t4, {word >> 16:#x}\n  ori $t4, $t4, {word & 0xffff:#x}")
                e("  sw $t4, 0($t3)")
            else:
                e(f"  lui $t3, %hi(B{j})\n  addiu $t3, $t3, %lo(B{j})")
                e(f"  lui $t4, {word >> 16:#x}\n  ori $t4, $t4, {word & 0xffff:#x}")
                e("  sw $t4, 0($t3)")
    e(f"B{i}_m:")                                  # 16 bytes past B<i>: middle patch word
    e(f"  addiu $v1, $v1, {R.randrange(1, 100)}")
    if i in leaf:                                  # leaf 'function': return
        e("  jr $ra")
        e(ds())
        continue
    nxt = i + 1 if (i + 1 < nb and (i + 1) not in leaf) else R.choice(nonleaf)
    r = R.random()
    if r < 0.2:
        e(f"  j B{tgt}"); e(ds())
    elif r < 0.45:
        c = R.choice(COND)
        br = R.choice([f"beq {c}, $zero", f"bne {c}, $zero", f"bgez {c}", f"bltz {c}",
                       f"blez {c}", f"bgtz {c}", f"beq {c}, {R.choice(COND)}"])
        e(f"  {br}, B{tgt}"); e(ds())
        e(f"  j B{nxt}\n  nop")
    elif r < 0.6 and leaf:
        lf = R.choice(sorted(leaf))
        e(f"  jal B{lf}"); e(ds())
        e(f"  j B{tgt}\n  nop")
    elif r < 0.72:
        e(f"  lw $t5, {tgt * 4}($t9)\n  nop")
        e("  jr $t5"); e(ds())
    elif r < 0.8 and leaf:
        lf = R.choice(sorted(leaf))
        e(f"  lw $t5, {lf * 4}($t9)\n  nop")
        e("  jalr $t5"); e(ds())
        e(f"  j B{tgt}\n  nop")
    elif r < 0.84:
        c = R.choice(COND)                         # BAL family (emitBxxZ andlink)
        bl = R.choice([f"bgezal {c}", f"bltzal {c}", "bgezal $zero"])
        e(f"  {bl}, B{R.choice(sorted(leaf))}" if leaf and R.random() < 0.5 else f"  {bl}, D2")
        e(ds())
        e(f"  j B{tgt}\n  nop")
    elif r < 0.88:
        # dt==2: BD-slot load whose result D2's first insn reads -> workaround
        # path (jal / bal / bgez: not linked; j / beq: linked as usual)
        site = R.choice(["jal D2", f"bgezal {R.choice(COND)}, D2", "j D2r",
                         f"bgez {R.choice(COND)}, D2r", f"bne {R.choice(COND)}, $zero, D2r"])
        e(f"  {site}\n  lw $t2, {R.randrange(0, 64) * 4}($s6)")
        e(f"  j B{tgt}\n  nop")
    elif r < 0.9:
        e(f"  j B{nxt}\n  nop")
    else:
        e(f"  b B{tgt}"); e(ds())

# D2: leaf whose first insn reads $t2 (dt==2 target for calls);
# D2r: same, but continues to B0 (dt==2 target for plain jumps/branches)
e("D2:\n  addu $v0, $v0, $t2\n  addiu $v1, $v1, 7\n  jr $ra\n  nop")
e("D2r:\n  addu $a0, $a0, $t2\n  j B0\n  nop")
e("EXIT:")
if mode == "spin":
    e("SPIN0:\n  addiu $v0, $v0, 1\n  j SPIN1\n  nop")
    e("SPIN1:\n  addiu $a0, $a0, 3\n  bgez $a0, SPIN2\n  nop\n  j SPIN0\n  nop")
    e("SPIN2:\n  xor $a1, $a1, $v0\n  j SPIN0\n  nop")
else:
    e(f"  lui $t0, {DONE >> 16:#x}\n  lui $t1, {DONE_MAGIC >> 16:#x}\n  ori $t1, $t1, {DONE_MAGIC & 0xffff:#x}")
    e("  sw $t1, 0($t0)")
    e("  move $ra, $s5\n  jr $ra\n  nop")
e("  .align 4\nbtab:")
for i in range(nb):
    e(f"  .word B{i}")
e(f"  .word {DATA:#x}")
for i in range(nb):
    e(f"  .word B{i}_m")
e("  .word L0")
print("\n".join(o))
