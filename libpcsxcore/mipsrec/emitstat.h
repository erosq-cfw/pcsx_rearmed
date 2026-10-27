/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 erosqcfw */
/* EROSQ: EROSQ_EMITSTAT=1 -- where do the bytes of the JIT code cache go?
 *
 * The X1000 I-cache is capacity-bound and the JIT code cache is >=29% of
 * Crash's I$ misses (psx PMON 10-17), so this attributes every emitted host
 * byte to the guest opcode class that emitted it, plus sub-buckets inside the
 * load/store series emitter (range check / direct / SMC invalidation /
 * indirect slow path) and the block prologue (reg preload). Cumulative table
 * on stderr every 4096 compiled blocks ("EMITSTAT" lines) and from
 * mipsrec_emitstat_dump(). Compile-time only (the recompiler runs rarely):
 * zero cost in emitted code, one predictable branch per compiled insn when
 * off. Included once, from recompiler.cpp, before the emitters. */
#ifndef MIPSREC_EMITSTAT_H
#define MIPSREC_EMITSTAT_H

#include <stdio.h>
#include <stdlib.h>

enum {
	ES_ALU = 0,	/* SPECIAL ALU/shift, I-type ALU, LUI */
	ES_MDU,		/* MULT/DIV/MFHI/MFLO/MTHI/MTLO */
	ES_BRANCH,	/* BEQ/BNE/BLEZ/BGTZ/REGIMM (incl. delay slot + exits) */
	ES_JUMP,	/* J/JAL/JR/JALR (incl. delay slot + exits) */
	ES_LSU,		/* LB..SWR series (see sub-buckets) */
	ES_GTE_MEM,	/* LWC2/SWC2 */
	ES_GTE_MOV,	/* MFC2/CFC2/MTC2/CTC2 */
	ES_GTE_OP,	/* GTE commands */
	ES_COP0,	/* MFC0/MTC0/RFE */
	ES_SYS,		/* SYSCALL/BREAK/other */
	ES_NCLASS
};
static const char *const es_name[ES_NCLASS] = {
	"alu", "mdu", "branch", "jump", "lsu", "gte_mem", "gte_mov", "gte_op",
	"cop0", "sys",
};
enum { ES_LSU_CHECK = 0, ES_LSU_DIRECT, ES_LSU_INVAL, ES_LSU_INDIRECT, ES_LSU_N };
static const char *const es_lsu_name[ES_LSU_N] = { "range_check", "direct", "smc_inval", "indirect" };

static int es_on = -1;
static unsigned long long es_bytes[ES_NCLASS], es_calls[ES_NCLASS], es_guest[ES_NCLASS];
static unsigned long long es_lsu[ES_LSU_N];
static unsigned long long es_series[3], es_series_ops[3];  /* direct-only, dual, indirect-only */
static unsigned long long es_flush_bytes, es_flush_calls, es_brflush_bytes, es_brflush_calls;
static unsigned long long es_blocks, es_block_bytes, es_prologue, es_block_guest;

static inline int emitstat_enabled(void)
{
	if (es_on < 0)
		es_on = getenv("EROSQ_EMITSTAT") ? 1 : 0;
	return es_on;
}

static inline int es_classify(u32 op)
{
	switch (op >> 26) {
	case 0x00:
		switch (op & 0x3f) {
		case 0x08: case 0x09: return ES_JUMP;
		case 0x0c: case 0x0d: return ES_SYS;
		case 0x10: case 0x11: case 0x12: case 0x13:
		case 0x18: case 0x19: case 0x1a: case 0x1b: return ES_MDU;
		default: return ES_ALU;
		}
	case 0x01: case 0x04: case 0x05: case 0x06: case 0x07: return ES_BRANCH;
	case 0x02: case 0x03: return ES_JUMP;
	case 0x10: return ES_COP0;
	case 0x12: return (op & (1u << 25)) ? ES_GTE_OP : ES_GTE_MOV;
	case 0x32: case 0x3a: return ES_GTE_MEM;
	default:
		if ((op >> 26) >= 0x20 && (op >> 26) <= 0x2e) return ES_LSU;
		if ((op >> 26) >= 0x08 && (op >> 26) <= 0x0f) return ES_ALU;
		return ES_SYS;
	}
}

static void mipsrec_emitstat_dump_(FILE *f)
{
	unsigned long long tot = 0, g = 0;
	for (int i = 0; i < ES_NCLASS; i++) { tot += es_bytes[i]; g += es_guest[i]; }
	tot += es_prologue;
	fprintf(f, "EMITSTAT blocks=%llu bytes=%llu (%.1f B/block, %.2f B/guest insn) guest=%llu prologue=%llu\n",
		es_blocks, es_block_bytes, es_blocks ? (double)es_block_bytes / es_blocks : 0.0,
		g ? (double)tot / g : 0.0, g, es_prologue);
	for (int i = 0; i < ES_NCLASS; i++)
		if (es_calls[i])
			fprintf(f, "EMITSTAT %-8s bytes=%9llu %5.1f%%  guest=%8llu  %6.2f B/guest\n",
				es_name[i], es_bytes[i], tot ? 100.0 * es_bytes[i] / tot : 0.0,
				es_guest[i], es_guest[i] ? (double)es_bytes[i] / es_guest[i] : 0.0);
	fprintf(f, "EMITSTAT   exit-flush (regClearJump) bytes=%llu %.1f%% of total, calls=%llu\n",
		es_flush_bytes, tot ? 100.0 * es_flush_bytes / tot : 0.0, es_flush_calls);
	fprintf(f, "EMITSTAT   side-exit flush (regClearBranch) bytes=%llu %.1f%% of total, calls=%llu\n",
		es_brflush_bytes, tot ? 100.0 * es_brflush_bytes / tot : 0.0, es_brflush_calls);
	fprintf(f, "EMITSTAT   series direct-only=%llu (%llu ops) dual=%llu (%llu ops) indirect-only=%llu (%llu ops)\n",
		es_series[0], es_series_ops[0], es_series[1], es_series_ops[1], es_series[2], es_series_ops[2]);
	for (int i = 0; i < ES_LSU_N; i++)
		fprintf(f, "EMITSTAT   lsu.%-12s bytes=%9llu %5.1f%% of total\n", es_lsu_name[i],
			es_lsu[i], tot ? 100.0 * es_lsu[i] / tot : 0.0);
}

extern "C" void mipsrec_emitstat_dump(void) { mipsrec_emitstat_dump_(stderr); }

/* LSU sub-bucket marks: es_mark() at a segment start, es_lsu_add() at its end */
#define ES_MARK(var)          u8 *var = (u8 *)recMem
#define ES_LSU_ADD(bucket, from) do { if (es_on > 0) es_lsu[(bucket)] += (u8 *)recMem - (from); } while (0)

#endif
