#!/usr/bin/env python3
"""Generate a random MIPS-I guest test: loads/stores through NON-CONST base
pointers into RAM (all KSEG views + mirrors), scratchpad and side-effect-free
HW registers, mixed with ALU ops, inside a loop. Output: asm on stdout.
usage: gen_lsu.py SEED [N_OPS] [LOOPS]"""
import random, sys
seed = int(sys.argv[1]); nops = int(sys.argv[2]) if len(sys.argv) > 2 else 60
loops = int(sys.argv[3]) if len(sys.argv) > 3 else 3
R = random.Random(seed)
# pointer table at 0x80018000 (filled by the harness); pointer regs s0..s5
# kinds: 0 RAM kseg0, 1 RAM kseg1 mirror, 2 RAM kuseg 2nd mirror, 3 scratchpad,
#        4 HW memctl 0x1f801000, 5 HW kseg1 0xbf801000, 6 scratch kseg0 0x9f800100
import os
KP = [int(x) for x in os.environ.get("KINDS", "0,0,0,1,2,3,3,4,5,6").split(",")]
ptr_kinds = [R.choice(KP) for _ in range(6)]
out = []
o = out.append
o(".set noreorder\n.set nomacro\n.text\n.globl test\ntest:")
o("  lui $t9, %hi(ptab)\n  addiu $t9, $t9, %lo(ptab)")
for i in range(6):
    o(f"  lw $s{i}, {i*4}($t9)")
o("  nop")
o(f"  li $s7, {loops}")
o("  li $v0, 0\n  li $v1, 0")
o("loop:")
acc = ["$v0", "$v1", "$a0", "$a1", "$a2", "$a3"]
for k in range(nops):
    p = R.randrange(6); kind = ptr_kinds[p]; preg = f"$s{p}"
    hw = kind in (4, 5)
    if hw:
        off = R.choice([0,4,8,12,16,20,24,28,0x60]) if True else 0
    else:
        off = R.randrange(0, 0x200) & ~3
    op = R.choice(["lw","lw","lh","lhu","lb","lbu","sw","sw","sh","sb","lwl","lwr","swl","swr","alu","alu"])
    t = R.choice(["$t0","$t1","$t2","$t3","$t4","$t5"])
    if op == "alu":
        a = R.choice(acc); o(f"  addu {a}, {a}, {t}\n  xor $v0, $v0, {a}")
        continue
    if op in ("lh","lhu","sh"): off += R.choice([0,2])
    if op in ("lb","lbu","sb","lwl","lwr","swl","swr"): off += R.randrange(4)
    if hw and op in ("sb","sh","swl","swr","lwl","lwr","lb","lbu","lh","lhu") and R.random() < 0.7:
        op = R.choice(["lw","sw"]); off &= ~3
    if op.startswith("s"):
        o(f"  {op} {t}, {off}({preg})")
    else:
        o(f"  {op} {t}, {off}({preg})\n  nop")
        a = R.choice(acc)
        o(f"  addu {a}, {a}, {t}")
o("  addiu $s7, $s7, -1")
o("  bnez $s7, loop")
o("  addiu $s0, $s0, 0")     # BD
o("  jr $ra\n  nop")
PTR = [0x80100000, 0xa0100400, 0x00700800, 0x1f800000, 0x1f801000, 0xbf801000, 0x9f800100]
o("  .align 4\nptab:")
for k in ptr_kinds: o(f"  .word {PTR[k]:#x}")
print("\n".join(out))
print("# kinds", ptr_kinds)
