# blkext fixture: a "DMA" (psxCpu->Clear path) overwrites the SECOND word of a
# compiled block. Correct (fresh) behaviour: later calls run the new code, so
# the final v0 = 0x222. A Clear that only zeroes slots of blocks STARTING in
# the range (pre-blkext mipsrec, BLKEXT_MUTANT) keeps running the stale block:
# v0 stays 0x111. Patch: HX_DMAPATCH=<blockB+4>:24020222:5 (blkext.sh).
	.set noreorder
	.text
	.globl test
test:
	lui	$s0, 0x8003
	ori	$s0, $s0, 0x1000        # log at DATA 0x80031000 (in hlink's RAM hash)
	li	$t0, 400
loop:
	jal	blockB
	nop
	addiu	$t0, $t0, -1
	bnez	$t0, loop
	nop
	lui	$t9, 0x8003             # DONE marker
	lui	$t8, 0x600d
	ori	$t8, $t8, 0xc0de
	sw	$t8, 0($t9)
	jr	$ra
	nop
	.align 4
	.globl blockB
blockB:
	addiu	$t1, $t1, 1
	addiu	$v0, $zero, 0x111       # <- patched to 0x222 (24020222)
	sw	$v0, 0($s0)
	addiu	$s0, $s0, 4
	jr	$ra
	nop
