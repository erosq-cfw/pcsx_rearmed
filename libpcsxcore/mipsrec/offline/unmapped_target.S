# Regression (2026-10-22): compiling a jal whose target is unmapped, with a
# load in its delay slot, dereferenced the target word at compile time
# (DelayTest: NULL -> SEGV; psxtest_cpu on the device). The jal is compiled
# but never taken ($ra = SENT is nonzero at run time).
# Run: SRC=libpcsxcore/mipsrec/offline/unmapped_target.S offline/hexec.sh 1
.set noreorder
.set nomacro
.text
.globl test
test:
  bne $ra, $zero, 1f
  nop
  jal 0x8e000000
  lw $t0, 0($sp)
1:
  jr $ra
  nop
