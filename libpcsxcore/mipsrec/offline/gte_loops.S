	.set noreorder
	.set nomacro
	.text
# typical PsyQ-style transform loop: per polygon load 3 vertices, RTPT,
# NCLIP, store screen XYs, AVSZ3, write OT entry
	.globl xform_polys
	.type xform_polys,@function
xform_polys:
1:	lw	$t0, 0($a0)
	lw	$t1, 4($a0)
	lw	$t2, 8($a0)
	lwc2	$0, 0($t0)
	lwc2	$1, 4($t0)
	lwc2	$2, 0($t1)
	lwc2	$3, 4($t1)
	lwc2	$4, 0($t2)
	lwc2	$5, 4($t2)
	nop
	.word	0x4a280030		# RTPT
	nop
	.word	0x4b400006		# NCLIP
	mfc2	$t3, $24
	nop
	blez	$t3, 2f
	nop
	swc2	$12, 8($a1)
	swc2	$13, 16($a1)
	swc2	$14, 24($a1)
	.word	0x4b58002d		# AVSZ3
	mfc2	$t4, $7
	nop
	sll	$t4, $t4, 2
	addu	$t4, $t4, $a2
	lw	$t5, 0($t4)
	sw	$a1, 0($t4)
	sw	$t5, 0($a1)
	addiu	$a1, $a1, 32
2:	addiu	$a3, $a3, -1
	bnez	$a3, 1b
	addiu	$a0, $a0, 12
	jr	$ra
	move	$v0, $a1
	.size xform_polys, .-xform_polys

	.globl light_verts
	.type light_verts,@function
light_verts:
	ctc2	$a2, $0
	ctc2	$a3, $1
1:	lwc2	$0, 0($a0)
	lwc2	$1, 4($a0)
	lw	$t0, 0($a1)
	nop
	mtc2	$t0, $6
	nop
	.word	0x4a18041b		# NCCS
	nop
	swc2	$22, 0($a1)
	addiu	$a0, $a0, 8
	addiu	$t9, $t9, -1
	bnez	$t9, 1b
	addiu	$a1, $a1, 4
	jr	$ra
	nop
	.size light_verts, .-light_verts

	.globl set_matrix
	.type set_matrix,@function
set_matrix:
	lw	$t0, 0($a0)
	lw	$t1, 4($a0)
	lw	$t2, 8($a0)
	lw	$t3, 12($a0)
	lw	$t4, 16($a0)
	ctc2	$t0, $0
	ctc2	$t1, $1
	ctc2	$t2, $2
	ctc2	$t3, $3
	ctc2	$t4, $4
	lw	$t0, 20($a0)
	lw	$t1, 24($a0)
	lw	$t2, 28($a0)
	ctc2	$t0, $5
	ctc2	$t1, $6
	jr	$ra
	ctc2	$t2, $7
	.size set_matrix, .-set_matrix
