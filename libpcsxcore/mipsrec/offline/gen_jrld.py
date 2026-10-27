#!/usr/bin/env python3
# gen_jrld.py KIND CASE -- one jrld.sh fixture (EROSQ_JRLD, 2026-10-23):
# a jr/jalr with a load in its delay slot, followed at the jump target by the
# CASE's first instruction. KIND: jr | jalr (rd = ra) | jalrt (rd = t5) |
# jalrs (rd = rs = t2).
# The load is 'lw t0, 4(a1)' unless the case says otherwise; t0 starts as
# 0x11111111, so every case shows whether the target saw the old or new t0.
# An exception handler copied to 0x80000080 records EPC, Cause & 0x8000007c,
# BadVAddr at 0x80030900 and ends the run. Ends: DONE_MAGIC at 0x80030000 (hlink).
import sys

kind, case = sys.argv[1], sys.argv[2]

LOAD = 'lw $t0, 4($a1)'
TARGET = '$t2'          # jump register (holds the target address)
FIRST = 'addu $t1, $t2, $t3'
SETUP = []
TGT = 'target'
SMC = False
cases = {
    'reads':      dict(first='addu $t1, $t0, $zero'),            # psxTestLoadDelay 2: target sees old t0
    'readwrite':  dict(first='addiu $t0, $t0, 1'),               # 1: helper skips the load
    'writes':     dict(first='lui $t0, 0x4242'),                 # 3: load, then overwritten
    'ignores':    dict(first='addu $t1, $t2, $t3'),              # native path (EROSQ_JRLD=1)
    'immcoll':    dict(first='addiu $t1, $t2, 0x4000'),          # imm bits 11-15 == 8: guard -> helper
    'branch':     dict(first='beq $zero, $zero, 1f'),            # branch target -> helper
    'jump':       dict(first='j 1f'),
    'jrtarget':   dict(first='jr $t4'),                          # jr at the target (t4 = 1f)
    'unmapped':   dict(tgt='0x00a00000'),                        # no RAM there: IBE via the fallback
    'adel':       dict(load='lw $t0, 5($a1)'),                   # misaligned load faults
    'rtisrs':     dict(load='lw $t2, 4($a1)'),                   # load rt == jump reg: jump to OLD t2
    'baseisrs':   dict(load='lw $t0, 0($t2)'),                   # load base == jump reg (reads the target word)
    'lb':         dict(load='lb $t0, 7($a1)'),
    'lhu':        dict(load='lhu $t0, 6($a1)'),
    'lwl':        dict(load='lwl $t0, 9($a1)'),
    'lwr':        dict(load='lwr $t0, 9($a1)'),
    'rtzero':     dict(load='lw $zero, 4($a1)'),                 # rt == $zero: helper
    'linkisrt':   dict(load='lw $ra, 4($a1)'),                   # jalr: link reg == load rt
    'linkisrt5':  dict(load='lw $t5, 4($a1)'),
    'smc':        dict(smc=True),                                # target rewritten between two passes
    'io':         dict(load='lw $t0, 0x1070($a2)'),              # I_STAT read through the delay slot
    'baseislink': dict(load='lw $t0, 0(LINK)'),                 # load base == link reg: reads the link (jalr) / old value (jr)
    'linkread':   dict(load='lw LINK, 4($a1)', first='addu $t1, LINK, $zero'), # rt == link reg, target reads it: sees the link, then the load commits
    'rom':        dict(load='lw $s7, 0x7f0($a1)', tgt='0xbfc01000', pre='addiu $s7, $zero, 5'), # BIOS target (hlink HX_ROMLOOP: 'addiu s7,s7,-1'; s7 5, loads 0): case 1 -> helper;
                                                                 # its RAM alias (psxM+0x1c1000) is 0: a missing RAM check goes native
}
c = dict(cases[case])
LINKREG = {'jr': '$ra', 'jalr': '$ra', 'jalrt': '$t5', 'jalrs': TARGET}[kind]
for k in ('load', 'first'):
    if k in c:
        c[k] = c[k].replace('LINK', LINKREG)
FIRST = c.get('first', FIRST)
LOAD = c.get('load', LOAD)
TGT = c.get('tgt', TGT)
SMC = c.get('smc', False)

if kind == 'jr':
    JUMP = 'jr %s' % TARGET
elif kind == 'jalr':
    JUMP = 'jalr %s' % TARGET
elif kind == 'jalrt':
    JUMP = 'jalr $t5, %s' % TARGET
elif kind == 'jalrs':
    JUMP = '.word 0x01405009'                            # jalr $t2, $t2 (rd == rs, gas refuses it): jumps to the old rs
else:
    sys.exit('bad kind')

def la(reg, sym):
    if sym.startswith('0x'):
        v = int(sym, 16)
        return ['lui %s, 0x%x' % (reg, v >> 16), 'ori %s, %s, 0x%x' % (reg, reg, v & 0xffff)]
    return ['lui %s, %%hi(%s)' % (reg, sym), 'addiu %s, %s, %%lo(%s)' % (reg, reg, sym)]

out = ['.set noreorder', '.set nomacro', '.text', '.globl test', 'test:']
# install the exception handler at 0x80000080
out += la('$t0', 'handler') + ['lui $t1, 0x8000', 'ori $t1, $t1, 0x80', 'addiu $t2, $zero, 24',
        'hcopy:', 'lw $t3, 0($t0)', 'addiu $t0, $t0, 4', 'addiu $t2, $t2, -1', 'sw $t3, 0($t1)',
        'bne $t2, $zero, hcopy', 'addiu $t1, $t1, 4']
out += ['lui $a1, 0x8003', 'ori $a1, $a1, 0x1000',      # DATA (hlink pattern)
        'lui $a2, 0x1f80',                                # I/O base
        'lui $t3, 0x3333', 'ori $t3, $t3, 0x3333']
out += la('$t4', 'after')
out += ['addiu $s0, $zero, %d' % (2 if SMC else 1)]      # passes
out += [c['pre']] if 'pre' in c else []
out += ['b again', 'nop',                              # pass 1 enters the jump's block at 'again' too
        'again:', 'lui $t0, 0x1111', 'ori $t0, $t0, 0x1111', 'lui $t5, 0x5555']
out += la('$t2', TGT)
out += [JUMP, LOAD, 'nop', 'break 0']                     # never reached
out += ['target:', FIRST, 'nop',
        '1:', 'after:', 'nop']
if SMC:
    # pass 1 ran 'ignores'; store 'addu t1, t0, zero' (reads t0) over the target's first word
    out += ['addiu $s0, $s0, -1', 'beq $s0, $zero, fin', 'nop',
            'sw $t0, 0x804($a1)', 'sw $t1, 0x808($a1)',
            'lui $t6, 0x0100', 'ori $t6, $t6, 0x4821']    # addu t1, t0, zero = 0x01004821
    out += la('$t7', 'target') + ['sw $t6, 0($t7)', 'b again', 'nop']
out += ['fin:', 'sw $t0, 0x800($a1)', 'sw $t1, 0x80c($a1)', 'sw $t2, 0x810($a1)', 'sw $ra, 0x814($a1)',
        'sw $t5, 0x818($a1)',
        'lui $t9, 0x8003', 'lui $t8, 0x600d', 'ori $t8, $t8, 0xc0de', 'sw $t8, 0($t9)',
        'spin:', 'b spin', 'nop']
# handler (copied to 0x80000080; k0/k1 only, nops after every mfc0)
out += ['handler:',
        'mfc0 $k0, $14', 'nop', 'lui $k1, 0x8003', 'sw $k0, 0x900($k1)',
        'mfc0 $k0, $13', 'nop', 'lui $k1, 0x8000', 'ori $k1, $k1, 0x7c', 'and $k0, $k0, $k1',
        'lui $k1, 0x8003', 'sw $k0, 0x904($k1)',
        'mfc0 $k0, $8', 'nop', 'sw $k0, 0x908($k1)',
        'sw $t0, 0x90c($k1)',
        'lui $k0, 0x600d', 'ori $k0, $k0, 0xc0de', 'sw $k0, 0($k1)',
        'hspin:', 'b hspin', 'nop', 'nop', 'nop']
print('\n'.join(('  ' + l if not l.endswith(':') else l) for l in out))
