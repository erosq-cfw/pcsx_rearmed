# nclipj.S -- guest program for nclipj.sh (EROSQ_NCLIPJ differential test).
# For n vectors at IN (3 words each: SXY0 SXY1 SXY2), load the SXY FIFO with
# mtc2, run NCLIP, store MAC0 (mfc2 $24) and FLAG (cfc2 $31) to OUT (2 words
# each). n is read from CNT. Ends by writing DONE_MAGIC to DONE, then spins.
# The nops keep the R3000 load / COP2 delays honest for the interpreter.
.set noreorder
.set nomacro
.text
.globl test
test:
  lui   $a0, 0x8010          # IN  = 0x80100000
  lui   $a1, 0x8018          # OUT = 0x80180000
  lui   $t9, 0x8003
  lw    $a2, 0x0800($t9)     # CNT = 0x80030800
  nop
1:
  lw    $t0, 0($a0)
  lw    $t1, 4($a0)
  lw    $t2, 8($a0)
  nop
  mtc2  $t0, $12
  mtc2  $t1, $13
  mtc2  $t2, $14
  nop
  nop
  .word 0x4a000006           # cop2 0x0000006 = NCLIP (gte_loops.S: 0x4b400006 also NCLIP)
  nop
  nop
  mfc2  $t3, $24
  cfc2  $t4, $31
  nop
  sw    $t3, 0($a1)
  sw    $t4, 4($a1)
  addiu $a0, $a0, 12
  addiu $a1, $a1, 8
  addiu $a2, $a2, -1
  bne   $a2, $zero, 1b
  nop
  lui   $t9, 0x8003          # DONE = 0x80030000
  lui   $t8, 0x600d
  ori   $t8, $t8, 0xc0de
  sw    $t8, 0($t9)
2:
  b     2b
  nop
