/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 erosqcfw */
/*
 * jitsched.cpp -- mipsrec code that must not move existing code or data:
 * EROSQ_JITSCHED glue for the block scheduler (jitsched.h), the offline test
 * hook, and the block-extent table (below). Built without per-function
 * sections and with its .text and .rodata renamed to .jitsched_text /
 * .jitsched_rodata (Makefile), which jitsched.ld places after every existing
 * section; its .bss comes last: every pre-existing symbol keeps its address.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "jitsched.h"
#include "../r3000a.h"
#include "../psxmem.h"
#include "../psxcounters.h"

/* all zero-initialised (.bss, appended last): a .data entry would move
 * every pre-existing .data/.bss symbol */
static int jitsched_inited, jitsched_on, jitsched_stat, blkext_stat;
static unsigned long js_blocks, js_changed, js_regions;

/* EROSQ_IDLESKIP (2026-10-22; default ON since its device A/B). Guest spin loops
 * that only wait for the next event (IDLESIZE.md: Crash 44%, MGS 37%, Spyro 26%
 * of emulated cycles) are run in bulk instead of one iteration per dispatch:
 *   V  libetc VSync wait with timeout (Crash/Spyro/Ape): L = lw v0,16(sp);
 *      addiu -1; sw; lw; bne v0,v1,M -> M = lui/lw vcount; slt v0,v0,a0; bnez L
 *      (M's distance follows the bne: the timeout path differs per game)
 *   I  MGS idle thread: lui/lw flag; beqz; jal T (T = jr ra; li v0,-1);
 *      move s0,v0; bltz s0 -> head
 *   A  GT2 VSync wait: lw v0,4(a0); slt v0,v0,a1; bnez self
 * A block whose guest code at its start matches gets 5 entry words (filled by
 * jitsched_tail): lui a0,hi(pc); move a1,ra; jal mipsrec_idle_entry;
 * ori a0,lo(pc); move ra,v1. The helper returns pc in v0 (the block's v0 == pc
 * entry contract) and the caller's ra in v1; everything else it clobbers is
 * caller-saved scratch, dead at block entry (guest regs are in psxRegs until
 * rec_preload_regs, which comes after this room). It re-matches the guest
 * words (incl. T) every call, so SMC'd code simply stops skipping.
 * Exactness: no event can fire inside the loop (all sources schedule through
 * next_interupt; polled words are RAM/scratchpad, never HW). With C = the
 * loop's exact per-iteration charge (sum of its blocks' ADJUST_CLOCK, the
 * value the blocks' exits add) it skips m iterations with cycle + m*C <
 * next_interupt, i.e. none of their block exits would have reached the event
 * check, applies their net guest effect, and returns: the real block then
 * runs the iteration that reaches the event, unmodified. V also caps m at
 * its timeout counter, so the real iteration takes the timeout path exactly
 * when the plain run would. */
/* EROSQ_IDLESKIP2 (2026-10-22, round 2; default off): game/library loops
 * the round-1 shapes miss (idlesurvey: CTR 15.7%, Tekken 3 15-18% of emulated
 * cycles). Same mechanism and exactness argument:
 *   P  CTR frame-pacing wait: lw v1,X(gp); lw v0,Y(v1); sltiu v0,K; beqz
 *      v0,timeout; lbu v0,Z(v1); bnez v0,L; lw v0,W(gp); bgtz v0,L -- loads
 *      only (the flags change in IRQ callbacks); the path taken (busy byte or
 *      countdown) fixes the charge: 11 or 15 words.
 *   R  Tekken 3 title/fight wait: jal T (T: x = 5x+1 at a RAM word);
 *      y = 69069*(y+1) at another; loop while a vsync-flag word is 0. The m
 *      skipped iterations' net effect is both LCGs jumped m steps ahead
 *      (closed form), written back; every register the loop reads is
 *      rewritten before the read in the real iteration that follows.
 *   D  libgpu DrawSync (Ape, Rollcage; same PsyQ code elsewhere): jal H
 *      (H: VSync(-1) vs a deadline, ++counter, give up past 0xf0000); then
 *      poll DMA2 CHCR busy (bit 24) and GPUSTAT ready (bit 26). Its inputs
 *      are hardware: CHCR changes only at the GPU-DMA event (next_interupt);
 *      GPUSTAT's ready bit comes from psxRegs.gpuIdleAfter, timer 1 from
 *      rcnts[1] -- both cycle functions on the emu thread (USE_ASYNC_GPU is
 *      off). A skip needs the previous hook call at the same pc exactly C
 *      cycles earlier with next_interupt unchanged (a whole real iteration,
 *      no event: its GPUSTAT read already flushed gpulib's command buffer),
 *      and m stays below the next timer-1 tick, the gpuIdleAfter flip and
 *      the counter's give-up. Net effect: counter += m, a0 = old count of
 *      the last skipped iteration; every other register and stack slot is
 *      what the previous iteration left. */
enum { IS_NONE, IS_V, IS_I, IS_A, IS_P, IS_R, IS_D, IS_N };
static void jitsched_init(void);
static int idleskip_on, idleskip_stat, idleskip2_on;
static int cur_words, cur_idle;   /* entry room of the block being compiled */
static uint32_t cur_pc;
static unsigned long is_blocks[IS_N], is_calls[IS_N], is_skips[IS_N], is_nomatch;
static unsigned long long is_iters[IS_N];
extern u32 cycle_multiplier;              /* recompiler.cpp: 24.8 cycles per guest insn */
extern "C" void mipsrec_code_range(uintptr_t *lo, uintptr_t *hi);
extern "C" uint64_t mipsrec_idle_entry(uint32_t pc, uint32_t ra);

static inline uint32_t is_clk(uint32_t insns) { return (insns * cycle_multiplier) >> 8; }   /* = ADJUST_CLOCK */

/* word-aligned guest RAM (+mirrors, KUSEG/KSEG0/KSEG1) or scratchpad, else NULL */
static uint32_t *is_ptr(uint32_t a)
{
	const uint32_t p = a & 0x1fffffff;
	if (a & 3)
		return NULL;
	if (p < 0x800000)
		return (uint32_t *)(psxRegs.ptrs.psxM + (p & 0x1ffffc));
	if (p >= 0x1f800000 && p < 0x1f800400)
		return (uint32_t *)(psxRegs.ptrs.psxH + (p & 0x3fc));
	return NULL;
}

/* shape of the guest code at pc (RAM only); aux = polled address [, callee] */
static int idle_match(uint32_t pc, uint32_t *aux)
{
	static const uint32_t vl[8] = { 0x8fa20010, 0, 0x2442ffff, 0xafa20010, 0x8fa20010, 0, 0x14430000, 0 };
	static const uint32_t ga[5] = { 0x8c820004, 0, 0x0045102a, 0x1440fffc, 0 };
	if ((pc & 0x1fffffff) >= 0x800000 || (pc & 0x1ffffc) > 0x200000 - 0x80)
		return IS_NONE;
	const uint32_t *w = is_ptr(pc);
	if (!w)
		return IS_NONE;
	int i;
	for (i = 0; i < 8 && (i == 6 ? w[i] & 0xffff0000 : w[i]) == vl[i]; i++) ;
	if (i == 8) {
		/* M = bne target (Crash/Spyro pc+0x4c, Ape pc+0x48: the timeout
		 * path's length differs); M's bnez must branch back to pc */
		const int32_t off = (int16_t)w[6];
		if (off < 1 || off > 24)
			return IS_NONE;
		const uint32_t *m = w + 7 + off;
		if ((m[0] & 0xffff0000) == 0x3c020000 && (m[1] & 0xffff0000) == 0x8c420000 &&
		    m[2] == 0 && m[3] == 0x0044102a && m[4] == (0x14400000 | ((uint32_t)(-(7 + off + 5)) & 0xffff)) && m[5] == 0) {
			aux[0] = (m[0] << 16) + (uint32_t)(int32_t)(int16_t)m[1];
			return IS_V;
		}
		return IS_NONE;
	}
	for (i = 0; i < 5 && w[i] == ga[i]; i++) ;
	if (i == 5)
		return IS_A;
	if ((w[0] & 0xffff0000) == 0x3c020000 && (w[1] & 0xffff0000) == 0x8c420000 && w[2] == 0 &&
	    w[3] == 0x1040fffe && w[4] == 0 && (w[5] >> 26) == 0x03 && w[6] == 0 &&
	    w[7] == 0x00408021 && w[8] == 0x0600fff7 && w[9] == 0) {
		const uint32_t t = ((pc + 0x18) & 0xf0000000) | ((w[5] & 0x03ffffff) << 2);
		const uint32_t *tw = is_ptr(t);
		if (tw && (t & 0x1fffffff) < 0x800000 && (t & 0x1ffffc) <= 0x200000 - 8 &&
		    tw[0] == 0x03e00008 && tw[1] == 0x2402ffff) {
			aux[0] = (w[0] << 16) + (uint32_t)(int32_t)(int16_t)w[1];
			aux[1] = t;
			return IS_I;
		}
	}
	if (idleskip2_on <= 0)
		return IS_NONE;
	/* P: offsets kept in aux (X, Y, K, Z, W) -- checked per call */
	if ((w[0] & 0xffff0000) == 0x8f830000 && w[1] == 0 && (w[2] & 0xffff0000) == 0x8c620000 && w[3] == 0 &&
	    (w[4] & 0xffff0000) == 0x2c420000 && (w[5] & 0xffff0000) == 0x10400000 && w[6] == 0 &&
	    (w[7] & 0xffff0000) == 0x90620000 && w[8] == 0 && w[9] == 0x1440fff6 && w[10] == 0 &&
	    (w[11] & 0xffff0000) == 0x8f820000 && w[12] == 0 && w[13] == 0x1c40fff2 && w[14] == 0)
		return IS_P;
	/* R: jal T; nop; lw v1,P(s0); lw a0,Q(s1); addiu v1,1; v0 = 69069*v1 by
	 * shifts; beqz a0,L; sw v0,P(s0) <BD> -- T = lui a0,H; lw v1,Y(a0); nop;
	 * v0 = 5*v1 + 1; jr ra; sw v0,Y(a0) <BD> */
	static const uint32_t rmul[10] = { 0x00031140, 0x00431021, 0x00021080, 0x00431023, 0x00021080,
					   0x00431023, 0x00021080, 0x00431021, 0x00021940, 0x00431021 };
	if ((w[0] >> 26) == 0x03 && w[1] == 0 && (w[2] & 0xffff0000) == 0x8e030000 &&
	    (w[3] & 0xffff0000) == 0x8e240000 && w[4] == 0x24630001 && w[15] == 0x1080fff0 &&
	    w[16] == (0xae020000 | (w[2] & 0xffff)) && !memcmp(w + 5, rmul, sizeof rmul)) {
		const uint32_t t = ((pc + 4) & 0xf0000000) | ((w[0] & 0x03ffffff) << 2);
		const uint32_t *tw = is_ptr(t);
		if (tw && (t & 0x1fffffff) < 0x800000 && (t & 0x1ffffc) <= 0x200000 - 32 &&
		    (tw[0] & 0xffff0000) == 0x3c040000 && (tw[1] & 0xffff0000) == 0x8c830000 && tw[2] == 0 &&
		    tw[3] == 0x00031080 && tw[4] == 0x00431021 && tw[5] == 0x24420001 && tw[6] == 0x03e00008 &&
		    tw[7] == (0xac820000 | (tw[1] & 0xffff))) {
			aux[0] = (tw[0] << 16) + (uint32_t)(int32_t)(int16_t)tw[1];   /* x */
			aux[1] = t;
			return IS_R;
		}
	}
	/* D: DrawSync loop at pc; aux = H, VSync (checked below), H's return */
	{
		static const uint32_t dl[20] = { 0, 0, 0x14400000, 0x2402ffff, 0x3c020000, 0x8c420000, 0, 0x8c420000,
			0x3c030100, 0x00431024, 0x1440fff5, 0, 0x3c020000, 0x8c420000, 0, 0x8c420000, 0x3c030400,
			0x00431024, 0x1040ffed, 0x00001021 };
		static const uint32_t dm[20] = { 0, ~0u, 0xffff0000, ~0u, 0xffff0000, 0xffff0000, ~0u, ~0u, ~0u, ~0u, ~0u,
			~0u, 0xffff0000, 0xffff0000, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u };
		static const uint32_t hl[21] = { 0x27bdffe0, 0xafbf0018, 0, 0x2404ffff, 0x3c030000, 0x8c630000, 0,
			0x0062182a, 0x1460000c, 0, 0x3c030000, 0x24630000, 0x8c620000, 0, 0x00402021, 0x24420001,
			0xac620000, 0x3c02000f, 0x0044102a, 0x10400038, 0 };
		static const uint32_t vl[28] = { 0x3c020000, 0x8c420000, 0x3c050000, 0x8ca50000, 0x27bdffd8, 0xafbf0020,
			0xafb1001c, 0xafb00018, 0x8c500000, 0x8ca20000, 0, 0xafa20010, 0x8fa30010, 0x8ca20000, 0,
			0x1462fff9, 0, 0x8fa20010, 0x3c030000, 0x8c630000, 0, 0x00431023, 0x04810005, 0x3051ffff,
			0x3c020000, 0x8c420000, 0, 0 };
		static const uint32_t ep[5] = { 0x8fbf0020, 0x8fb1001c, 0x8fb00018, 0x03e00008, 0x27bd0028 };
		static const uint32_t rt[5] = { 0x00001021, 0x8fbf0018, 0x27bd0020, 0x03e00008, 0 };
		if ((w[0] >> 26) == 0x03) {
			for (i = 0; i < 20 && (w[i] & dm[i]) == dl[i]; i++) ;
			if (i == 20) {
				const uint32_t h = ((pc + 4) & 0xf0000000) | ((w[0] & 0x03ffffff) << 2);
				const uint32_t *hw = is_ptr(h);
				if (hw && (h & 0x1fffffff) < 0x800000 && (h & 0x1ffffc) <= 0x200000 - 0x140) {
					int k;
					for (k = 0; k < 21; k++) {
						const uint32_t m = (k == 2) ? 0 : (k == 4 || k == 5 || k == 10 || k == 11) ? 0xffff0000 : ~0u;
						if (k != 2 && (hw[k] & m) != hl[k]) break;
					}
					const uint32_t v = ((h + 12) & 0xf0000000) | ((hw[2] & 0x03ffffff) << 2);
					const uint32_t *vw = is_ptr(v), *rw = hw + 20 + 0x38;   /* beqz at k=19: target = word 20 + 0x38 */
					if (k == 21 && (hw[2] >> 26) == 0x03 && vw && (v & 0x1fffffff) < 0x800000 &&
					    (v & 0x1ffffc) <= 0x200000 - 0x80 && !memcmp(rw, rt, sizeof rt)) {
						for (k = 0; k < 28; k++) {
							const uint32_t m = (k == 0 || k == 1 || k == 2 || k == 3 || k == 18 || k == 19 ||
									    k == 24 || k == 25) ? 0xffff0000 : (k == 26) ? 0 : ~0u;
							if (k != 26 && (vw[k] & m) != vl[k]) break;
						}
						const uint32_t e = ((v + 0x6c) & 0xf0000000) | ((vw[26] & 0x03ffffff) << 2);
						const uint32_t *ew = is_ptr(e);
						if (k == 28 && (vw[26] >> 26) == 0x02 && ew && (e & 0x1ffffc) <= 0x200000 - 20 &&
						    !memcmp(ew, ep, sizeof ep)) {
							aux[0] = h;
							aux[1] = v;
							return IS_D;
						}
					}
				}
			}
		}
	}
	return IS_NONE;
}

/* z -> a*z + b (mod 2^32), applied m times */
static uint32_t lcg_jump(uint32_t z, uint32_t a, uint32_t b, uint32_t m)
{
	uint32_t ra = 1, rb = 0;                 /* accumulated map, identity */
	while (m) {
		if (m & 1) { rb = a * rb + b; ra *= a; }
		b = a * b + b; a *= a;               /* square the step map */
		m >>= 1;
	}
	return ra * z + rb;
}

static void idleskip_init(void)
{
	const char *e = getenv("EROSQ_IDLESKIP");
	idleskip_on = e ? atoi(e) : 1;   /* default ON (device A/B 10-22: MGS +8%, Spyro +5%, Ape +7%); EROSQ_IDLESKIP=0 is the kill-switch */
	idleskip_stat = getenv("EROSQ_IDLESKIP_STAT") != NULL;
	{
		const char *e2 = getenv("EROSQ_IDLESKIP2");
		idleskip2_on = e2 ? atoi(e2) : 1;   /* round 2: default ON (10-23: bit-exact, ~+1% Tekken 3 fight, never negative); kill EROSQ_IDLESKIP2=0 */
	}
	if (idleskip_on > 0) {
		uintptr_t lo, hi;
		mipsrec_code_range(&lo, &hi);
		const uintptr_t h = (uintptr_t)mipsrec_idle_entry;
		if (((lo ^ h) | ((hi - 1) ^ h)) & 0xf0000000) {   /* jal reaches one 256 MB segment */
			fprintf(stderr, "mipsrec: idleskip disabled (helper out of jal range)\n");
			idleskip_on = 0;
			return;
		}
		fprintf(stderr, "mipsrec: idleskip on%s\n", idleskip2_on > 0 ? " (+round 2)" : "");
	}
}

static void idleskip_print(void)
{
	static const char *const nm[IS_N] = { "-", "V", "I", "A", "P", "R", "D" };
	for (int s = 1; s < IS_N; s++)
		fprintf(stderr, "idleskip %s: blocks %lu calls %lu skips %lu iters %llu\n",
			nm[s], is_blocks[s], is_calls[s], is_skips[s], is_iters[s]);
	fprintf(stderr, "idleskip nomatch-calls %lu\n", is_nomatch);
}

extern "C" void mipsrec_idleskip_report(void) { if (idleskip_on > 0) idleskip_print(); }

/* D: the previous hook call (whole-real-iteration check) and the counter's address */
static uint32_t d_pc, d_cycle, d_nexti, d_ctr;

extern "C" uint64_t mipsrec_idle_entry(uint32_t pc, uint32_t ra)
{
	const uint64_t ret = ((uint64_t)ra << 32) | pc;
	uint32_t aux[2], *r = psxRegs.GPR.r, C, cap = 0xffffffffu, *ctr = NULL;
	const int s = idle_match(pc, aux);       /* re-checked every call: SMC */
	if (s == IS_NONE) {
		is_nomatch++;
		return ret;
	}
	is_calls[s]++;
	switch (s) {
	case IS_V: {
		const uint32_t *vc = is_ptr(aux[0]);
		ctr = is_ptr(r[29] + 16);
		if (!vc || !ctr || r[3] != 0xffffffffu || !((int32_t)*vc < (int32_t)r[4]))
			return ret;
		cap = *ctr;                          /* decrements before it reads -1 */
		C = is_clk(8) + is_clk(6);
		break; }
	case IS_I: {
		const uint32_t *f = is_ptr(aux[0]);
		if (!f || *f == 0)
			return ret;
		C = is_clk(7) + is_clk(2) + is_clk(3);
		break; }
	case IS_P: {
		const uint32_t *w = is_ptr(pc);
		const uint32_t *pv1 = is_ptr(r[28] + (uint32_t)(int32_t)(int16_t)w[0]);
		if (!pv1) return ret;
		const uint32_t v1 = *pv1;
		const uint32_t *cnt = is_ptr(v1 + (uint32_t)(int32_t)(int16_t)w[2]);
		const uint32_t zb = v1 + (uint32_t)(int32_t)(int16_t)w[7];
		const uint32_t *busy = is_ptr(zb & ~3u);
		const uint32_t *cd = is_ptr(r[28] + (uint32_t)(int32_t)(int16_t)w[11]);
		if (!cnt || !busy || !cd || !(*cnt < (uint32_t)(int32_t)(int16_t)w[4]))
			return ret;                      /* timeout path next */
		if ((*busy >> ((zb & 3) * 8)) & 0xff)
			C = is_clk(11);                  /* exits at bnez v0,L */
		else if ((int32_t)*cd > 0)
			C = is_clk(15);                  /* exits at bgtz v0,L */
		else
			return ret;                      /* leaves the loop */
		break; }
	case IS_R: {
		const uint32_t *w = is_ptr(pc);
		const uint32_t *flag = is_ptr(r[17] + (uint32_t)(int32_t)(int16_t)w[3]);
		if (!is_ptr(aux[0]) || !is_ptr(r[16] + (uint32_t)(int32_t)(int16_t)w[2]) || !flag || *flag)
			return ret;
		C = is_clk(2) + is_clk(8) + is_clk(15);   /* jal; T; return block to the beqz */
		break; }
	case IS_D: {
		const uint32_t *w = is_ptr(pc), *hw = is_ptr(aux[0]), *vw = is_ptr(aux[1]);
#define D_ADDR(hi, lo) (((hi) << 16) + (uint32_t)(int32_t)(int16_t)(lo))
		const uint32_t *pc_chcr = is_ptr(D_ADDR(w[4], w[5])), *pc_gpu = is_ptr(D_ADDR(w[12], w[13]));
		const uint32_t *pv_gpu = is_ptr(D_ADDR(vw[0], vw[1])), *pv_tim = is_ptr(D_ADDR(vw[2], vw[3]));
		const uint32_t *dead = is_ptr(D_ADDR(hw[4], hw[5])), *vcnt = is_ptr(D_ADDR(vw[24], vw[25]));
		d_ctr = D_ADDR(hw[10], hw[11]);
		ctr = is_ptr(d_ctr);
#undef D_ADDR
		const uint32_t c0 = psxRegs.cycle;
		const int consecutive = pc == d_pc && psxRegs.next_interupt == d_nexti;
		const uint32_t prev = d_cycle;
		d_pc = pc; d_cycle = c0; d_nexti = psxRegs.next_interupt;
		if (!pc_chcr || !pc_gpu || !pv_gpu || !pv_tim || !dead || !vcnt || !ctr ||
		    *pc_chcr != 0x1f8010a8 || *pc_gpu != 0x1f801814 || *pv_gpu != 0x1f801814 || *pv_tim != 0x1f801110)
			return ret;
		if ((int32_t)*dead < (int32_t)*vcnt)
			return ret;                      /* the helper takes its timeout path */
		const int idle = (int32_t)(psxRegs.gpuIdleAfter - c0) < 0;
		if (psxHu32(0x10a8) & 0x01000000)
			C = is_clk(2) + is_clk(4) + is_clk(28) + is_clk(5) + is_clk(17) + is_clk(5) + is_clk(10);
		else if (!((psxHu32(0x1814) | (idle ? 0x04000000u : 0)) & 0x04000000))
			C = is_clk(2) + is_clk(4) + is_clk(28) + is_clk(5) + is_clk(17) + is_clk(5) + is_clk(18);
		else
			return ret;                      /* leaves the loop */
		if (!consecutive || c0 - prev != C)
			return ret;                      /* not right after a whole real iteration */
		/* inputs constant from the previous iteration's start through the skipped ones */
		const uint32_t rate = rcnts[1].rate;
		if (rate <= 1)
			return ret;
		const uint32_t tick = rcnts[1].cycleStart + ((prev - rcnts[1].cycleStart) / rate + 1) * rate;
		int32_t lim = (int32_t)(tick - c0);
		if ((int32_t)(psxRegs.gpuIdleAfter - prev) >= 0) {   /* busy at the previous iteration */
			const int32_t g = (int32_t)(psxRegs.gpuIdleAfter - c0) + 1;   /* reads at c <= gpuIdleAfter */
			if (g < lim) lim = g;
		}
		if (lim <= 0 || (int32_t)*ctr > 0xf0000)
			return ret;
		cap = (uint32_t)lim / C;             /* c0 + m*C <= tick / gpuIdleAfter + 1 */
		if (0xf0000 - (int32_t)*ctr + 1 < (int32_t)cap)
			cap = 0xf0000 - *ctr + 1;        /* old counts of the skipped iterations stay <= 0xf0000 */
		break; }
	default: {   /* IS_A */
		const uint32_t *v = is_ptr(r[4] + 4);
		if (!v || !((int32_t)*v < (int32_t)r[5]))
			return ret;
		C = is_clk(5);
		break; }
	}
#ifdef IDLESKIP_MUTANT   /* offline mutant (idleskip.sh): per-iteration charge off by one */
	C += 1;
#endif
	const int32_t d = (int32_t)(psxRegs.next_interupt - psxRegs.cycle);
	if (!C || d <= (int32_t)C)
		return ret;
	uint32_t m = (uint32_t)(d - 1) / C;       /* cycle + m*C < next_interupt */
	if (m > cap)
		m = cap;
	if (!m)
		return ret;
	switch (s) {
	case IS_V:
		*ctr -= m;
		psxCpu->Clear((r[29] + 16) & ~3u, 1);   /* as the guest store would (SMC) */
		r[2] = 1;
		break;
	case IS_I:
		r[2] = r[16] = 0xffffffffu;
		r[31] = pc + 0x1c;
		break;
	/* registers as the last skipped iteration leaves them: an IRQ inside the
	 * crossing iteration makes the BIOS save them all to RAM */
	case IS_D:
		r[4] = *ctr + m - 1;                 /* a0 = the last skipped iteration's old count */
		*ctr += m;
		psxCpu->Clear(d_ctr & ~3u, 1);       /* as the guest stores would (SMC) */
		d_cycle = psxRegs.cycle + m * C;     /* the next hook call is one real iteration later */
		break;
	case IS_P: {
		const uint32_t *w = is_ptr(pc);
		const uint32_t v1 = *is_ptr(r[28] + (uint32_t)(int32_t)(int16_t)w[0]);
		const uint32_t zb = v1 + (uint32_t)(int32_t)(int16_t)w[7];
		const uint32_t busy = (*is_ptr(zb & ~3u) >> ((zb & 3) * 8)) & 0xff;
		r[3] = v1;
		r[2] = busy ? busy : *is_ptr(r[28] + (uint32_t)(int32_t)(int16_t)w[11]);
		break; }
	case IS_R: {
		const uint32_t *w = is_ptr(pc);
		const uint32_t ya = r[16] + (uint32_t)(int32_t)(int16_t)w[2];
		uint32_t *x = is_ptr(aux[0]), *y = is_ptr(ya);
		*x = lcg_jump(*x, 5, 1, m);
		const uint32_t yp = lcg_jump(*y, 69069, 69069, m - 1);
		*y = 69069 * (yp + 1);               /* y = 69069*(y+1) */
		r[2] = *y;                           /* v0 = new y */
		r[3] = 66976 * (yp + 1);             /* v1 = (2093*(y+1)) << 5, the shift chain's last write */
		r[4] = 0;                            /* a0 = the vsync flag, still 0 */
		r[31] = pc + 8;                      /* ra from the jal */
		psxCpu->Clear(aux[0] & ~3u, 1);      /* as the guest stores would (SMC) */
		psxCpu->Clear(ya & ~3u, 1);
		break; }
	default:
		r[2] = 1;
		break;
	}
	psxRegs.cycle += m * C;
	is_skips[s]++;
	is_iters[s] += m;
	if (idleskip_stat && (is_skips[s] & 4095) == 0)
		idleskip_print();
	return ret;
}

#ifdef HX_TTY
/* harness only: the BIOS A0/B0/C0 call stubs in RAM print through psxJumpTest
 * (the interpreter's -psxout path; mipsrec never calls it). Guest regs are in
 * psxRegs at block entry (preload comes after the room). */
extern "C" uint64_t mipsrec_tty_entry(uint32_t pc, uint32_t ra)
{
	const uint32_t save = psxRegs.pc;
	psxRegs.pc = pc;
	psxJumpTest();
	psxRegs.pc = save;
	return ((uint64_t)ra << 32) | pc;
}
#endif
/* EROSQ_GTENF (2026-10-22, default off): a GTE command whose FLAG result can
 * never be read gets the flagless handler (gteGetHandler_nf, the same bodies
 * ari64 uses; data results are identical). FLAG is read only by cfc2 $31, and
 * every GTE command (and ctc2 $31) overwrites it. gtenf_dead() walks the guest
 * code after the command: straight-line, both paths of each branch (its delay
 * slot first), into jal callees. A path is dead when it reaches a GTE command
 * or ctc2 $31 first, live at cfc2 $31. jr/jalr/syscall/break, a return from a
 * callee, running out of budget or unmapped code all count as live, so the
 * flagless handler is only used where no path can read the FLAG. Census
 * (interpreter, 10-22): provably dead GT2 43%, Crash 19%, Spyro 14% of GTE
 * commands. */
static int gtenf_on = -1;
static uint32_t gtenf_hi;   /* RAM end of the code walked for the block being compiled (bx_block widens its extent) */
static unsigned long gtenf_n, gtenf_nf;
/* SMC: the walk only reads RAM at or after the block's start (or ROM from a ROM
 * block), and bx_block registers the block's extent up to the highest word it
 * read, so a Clear() of any word the decision relied on recompiles the block */
static uint32_t gtenf_word(uint32_t a, int *ok)
{
	const uint32_t pa = a & 0x1fffffff, ps = psxRegs.pc & 0x1fffffff;
	const int ram = pa < 0x800000, ram_blk = ps < 0x800000;
	const void *p = psxm(a, 0);
	*ok = p != INVALID_PTR && ram == ram_blk && (!ram || ((pa & 0x1ffffc) >= (ps & 0x1ffffc)
	      && (pa & 0x1ffffc) - (ps & 0x1ffffc) < 0x10000));
	if (*ok && ram && (pa & 0x1ffffc) + 4 > gtenf_hi)
		gtenf_hi = (pa & 0x1ffffc) + 4;
	return *ok ? *(const uint32_t *)p : 0;
}
static int gtenf_cls(uint32_t w)   /* 1 = FLAG overwritten, -1 = FLAG read, 0 = neither */
{
	const uint32_t op = w >> 26, rs = (w >> 21) & 31, rd = (w >> 11) & 31;
	if (op != 0x12) return 0;
	if (rs & 0x10) return 1;
	if (rd == 31 && rs == 6) return 1;
	if (rd == 31 && rs == 2) return -1;
	return 0;
}
/* 1 = every path from pc overwrites FLAG before reading it */
static int gtenf_dead(uint32_t pc, int *budget, int depth)
{
	for (;;) {
		int ok, c;
		if (--*budget < 0 || depth > 12) return 0;
		const uint32_t w = gtenf_word(pc, &ok), op = w >> 26, fn = w & 63;
		if (!ok) return 0;
		if ((c = gtenf_cls(w))) return c > 0;
		if (op == 0 && (fn == 8 || fn == 9 || fn == 0x0c || fn == 0x0d)) return 0;
		const int is_j = op == 2 || op == 3, is_b = op == 1 || (op >= 4 && op <= 7);
		if (is_j || is_b) {
			const uint32_t ds = gtenf_word(pc + 4, &ok);
			if (!ok) return 0;
			if ((c = gtenf_cls(ds))) return c > 0;
			if (op == 0 && ((ds & 63) == 8 || (ds & 63) == 9)) return 0;
			if (is_b) {
				if (!gtenf_dead(pc + 4 + ((uint32_t)(int32_t)(int16_t)w << 2), budget, depth + 1))
					return 0;
				pc += 8;
				continue;
			}
			const uint32_t t = ((pc + 4) & 0xf0000000) | ((w & 0x03ffffff) << 2);
			if (op == 3)
				return gtenf_dead(t, budget, depth + 1);   /* the callee must settle it: its return is unknown */
			pc = t;
			continue;
		}
		pc += 4;
	}
}
extern "C" int mipsrec_gte_flag_dead(uint32_t next_pc, int in_delay_slot)
{
	if (gtenf_on < 0) {
		const char *e = getenv("EROSQ_GTENF");
		gtenf_on = e ? atoi(e) : 0;
	}
	int budget = 400;
	if (gtenf_on <= 0 || in_delay_slot)
		return 0;
	const int d = gtenf_dead(next_pc, &budget, 0);
	if (gtenf_on > 1 && (++gtenf_n, gtenf_nf += d, (gtenf_n & 0x7f) == 0))   /* EROSQ_GTENF=2: compile-time stat */
		fprintf(stderr, "gtenf: %lu of %lu compiled GTE commands flagless\n", gtenf_nf, gtenf_n);
	return d;
}

extern u32 *recMem;   /* recompiler.cpp: next emitted word */

/* recRecompile, at a block's first word: reserve the entry room (jitsched_tail fills it) */
/* EROSQ load delay (2026-10-22, psxtest_cpu _d tests; Tekken 3's
 * `beq t0,zero; mfc2 t0,$24 <BD>; bgtz t0`). The R3000 commits a load
 * (and mfc0/mfc2/cfc2) one instruction late: the next instruction still
 * reads the old rt, except LWL/LWR, which merge into the pending value (the
 * interpreter's dload*). mipsrec writes rt at once. ldhaz_scan() looks for
 * such a pair in the guest code the block will hold: straight-line, or a
 * load in a branch delay slot read by the not-taken fall-through. Read by
 * the taken target is opt-in (EROSQ_LDHAZ=2, ari64's target scan; jr/jalr
 * targets are unknown): hlink's dt2-* control tests pin how those sites
 * compile today (DelayTest's dt == 2 workaround / plain links). Such a block
 * starts with a 5-word call to mipsrec_ldhaz_run, which interprets it with
 * exact delay semantics and returns to the dispatch loop. The normal
 * compile follows as dead code, so recRecompile (PGO-profiled) is untouched
 * and every hazard-free block is emitted exactly as before.
 * EROSQ_LDHAZ=0 disables it, EROSQ_LDHAZ_LOG=1 logs each such block. */
u64 opcodeGetReads(const u32 op);           /* mips_codegen.cpp */
extern "C" uint32_t mipsrec_ldhaz_run(uint32_t pc);
extern "C" uintptr_t mipsrec_block_ret_addr(void);
static int ldhaz_mode = -1, ldhaz_log;
extern "C" { unsigned long mipsrec_ldhaz_blocks, mipsrec_ldhaz_runs; }

static uint32_t ldhaz_word(uint32_t a)      /* 0 (nop) if unmapped */
{
	const void *p = psxm(a, 0);
	return p == INVALID_PTR ? 0 : *(const uint32_t *)p;
}

static int ldhaz_is_load(uint32_t w)
{
	const uint32_t op = w >> 26, rs = (w >> 21) & 31;
	return ((op >= 0x20 && op <= 0x26) || ((op == 0x10 || op == 0x12) && (rs == 0 || rs == 2))) &&
	       ((w >> 16) & 31) != 0;
}

/* does the instruction after load w read the old rt (and so differ)? */
static int ldhaz_pair(uint32_t w, uint32_t next)
{
	const uint32_t rt = (w >> 16) & 31, nop = next >> 26;
	if (!next || ((nop == 0x22 || nop == 0x26) && ((next >> 16) & 31) == rt))
		return 0;   /* nop; LWL/LWR merge into the pending value */
	return (opcodeGetReads(next) >> rt) & 1;
}

static int ldhaz_jit_on(void);
static int ldhaz_scan(uint32_t start)
{
	for (uint32_t k = start, n = 0; n < 1024; k += 4, n++) {
		const uint32_t w = ldhaz_word(k), op = w >> 26, fn = w & 63;
		const int is_jump = op == 2 || op == 3 || (op == 0 && (fn == 8 || fn == 9));
		const int is_branch = op == 1 || (op >= 4 && op <= 7);
		if (op == 0 && (fn == 0x0c || fn == 0x0d))
			return 0;                          /* syscall/break end the block */
		if (is_jump || is_branch) {
			const uint32_t ds = ldhaz_word(k + 4);
			if (!ldhaz_is_load(ds))
				return 0;
			if (is_branch && !ldhaz_jit_on() && ldhaz_pair(ds, ldhaz_word(k + 8)))
				return 1;                      /* not taken: the fall-through (else the JIT handles it) */
			if (ldhaz_mode < 2)
				return 0;
			if (is_branch)
				return ldhaz_pair(ds, ldhaz_word(k + 4 + ((uint32_t)(int32_t)(int16_t)w << 2)));
			if (op == 2 || op == 3)
				return ldhaz_pair(ds, ldhaz_word((k & 0xf0000000) | ((w & 0x03ffffff) << 2)));
			return 0;                          /* jr/jalr: target unknown */
		}
		if (ldhaz_is_load(w) && ldhaz_pair(w, ldhaz_word(k + 4)))
			return 1;
	}
	return 0;
}

static int ldhaz_block(uint32_t pc)
{
	if (ldhaz_mode < 0) {
		const char *e = getenv("EROSQ_LDHAZ");
		ldhaz_mode = e ? atoi(e) : 0;   /* variant (i): default OFF (output == shipped); 1 = straight-line pairs */
		ldhaz_log = getenv("EROSQ_LDHAZ_LOG") != NULL;
	}
	return ldhaz_mode && ldhaz_scan(pc);
}

/* rec_bcu.cpp.h emitBxx/emitBxxZ: the not-taken fall-through of a
 * conditional branch reads the load in its delay slot (EROSQ_LDHAZ_JIT,
 * default on; 0 disables) */
static int ldhaz_jit = -1;
static int ldhaz_jit_on(void)
{
	if (ldhaz_jit < 0) {
		const char *e = getenv("EROSQ_LDHAZ_JIT");
		ldhaz_jit = e ? atoi(e) : 0;   /* variant (i): default OFF */
	}
	return ldhaz_jit;
}
extern "C" int mipsrec_ldhaz_ft_hazard(uint32_t ds, uint32_t ft)
{
	return ldhaz_jit_on() && ldhaz_is_load(ds) && ldhaz_pair(ds, ft);
}

/* lui a0,hi(pc); jal mipsrec_ldhaz_run; ori a0,lo(pc) <BD>; j loop; move v1,zero <BD> */
static void ldhaz_emit(uint32_t pc)
{
	const uintptr_t f = (uintptr_t)mipsrec_ldhaz_run, loop = mipsrec_block_ret_addr(), here = (uintptr_t)recMem;
	if (!loop || ((here ^ f) & 0xf0000000) || ((here ^ loop) & 0xf0000000))
		return;                                /* indirect-return mode / out of jal range: JIT as before */
	recMem[0] = 0x3c040000 | (pc >> 16);
	recMem[1] = 0x0c000000 | ((f >> 2) & 0x03ffffff);
	recMem[2] = 0x34840000 | (pc & 0xffff);
	recMem[3] = 0x08000000 | ((loop >> 2) & 0x03ffffff);
	recMem[4] = 0x00001821;
	recMem += 5;
	mipsrec_ldhaz_blocks++;
	if (ldhaz_log)
		fprintf(stderr, "ldhaz: block %08x interpreted\n", pc);
}

extern "C" void mipsrec_entry_room(uint32_t pc)
{
	uint32_t aux[2];
	if (!jitsched_inited)
		jitsched_init();
	cur_pc = pc;
	cur_idle = idleskip_on > 0 ? idle_match(pc, aux) : IS_NONE;
#ifdef HX_TTY   /* offline harness build only: -psxout text under the JIT */
	if (!cur_idle && Config.PsxOut && !Config.HLE &&
	    ((pc & 0x1fffffff) == 0xa0 || (pc & 0x1fffffff) == 0xb0 || (pc & 0x1fffffff) == 0xc0))
		cur_idle = IS_N;   /* "shape" IS_N = BIOS call stub: hook calls mipsrec_tty_entry */
#endif
	cur_words = cur_idle ? 5 : 0;
	recMem += cur_words;
	if (ldhaz_block(pc))   /* load delay: the block runs in the interpreter */
		ldhaz_emit(pc);
}

/* Code outside RAM/BIOS (2026-10-22; ps1-tests cpu/code-in-io: scratchpad, IO
 * ports). Block-pointer slots exist only for RAM (+mirrors) and BIOS, so the
 * dispatch loop's slot load (lw t0,0(t2), t2 = lookup of v0) faults for any
 * other pc: psxRecLUT[pc >> 16] == 0 in the LUT dispatch (the shipped one;
 * EXP1 0x1f00xxxx included, so it never aliases RAM slots). The mmap dispatch
 * (Part B, compiled out) would fault past its 4 recRAM mirrors but alias EXP1
 * onto RAM (24-bit lookup) -- revisit if Part B is ever enabled. This SIGSEGV handler
 * (chained in front of faultmem's) recognises exactly that instruction and
 * fault address, hands the loop a stub instead of a block pointer, and the
 * stub interprets until pc is back in RAM/BIOS, then re-enters the loop top
 * with v1 = 0 (the interpreter charged the cycles). The normal path is
 * untouched: no check runs unless the slot load faults. */
#include <signal.h>
#include <stddef.h>
#include <ucontext.h>
#define CIO_REC_RAM_VADDR 0x21000000u      /* = REC_RAM_VADDR (recompiler.cpp, Part B only) */
extern "C" void mipsrec_branch_test(void);   /* plugin.c: gen_interupt seam */
static struct sigaction cio_prev;
static uint32_t cio_stub[8] __attribute__((aligned(32)));
static uintptr_t cio_loop;                   /* dispatch loop top the stub returns to */
static unsigned long cio_runs, cio_blocks;

static int cio_has_slots(uint32_t pc)        /* RAM (+mirrors) or BIOS, any segment */
{
	const uint32_t p = pc & 0x1fffffff;
	return ((pc >> 29) == 0 || (pc >> 29) == 4 || (pc >> 29) == 5) &&
	       (p < 0x800000 || (p >= 0x1fc00000 && p < 0x1fc80000));
}

/* Instruction fetch from IO space while the fallback interprets: hardware
 * executes code placed in the DMA (0x1f801080-0x1f8010ff) and SPU
 * (0x1f801c00-0x1f801fff) register ranges -- the fetch is a bus read of the
 * registers -- while scratchpad, MDEC, the IRQ controller etc. raise IBE
 * (ps1-tests cpu/code-in-io, hardware psx.log 7/7). The interpreter's own
 * fetch IBEs on all of them (psxm_lut maps no IO); wrapping its fetch pointer
 * here instead of editing psxinterpreter.c keeps every pre-existing object's
 * layout (psxinterpreter.o links early). IO is never cached. */
extern "C" u32 psxHwRead32(u32 add);
typedef u32 (*cio_fetch_fn)(psxRegisters *, const uintptr_t *, u32);
static cio_fetch_fn cio_orig_fetch;
static u32 cio_fetch(psxRegisters *r, const uintptr_t *luts, u32 pc)
{
	const u32 a = pc & 0x1ffffffc;
	if ((a >= 0x1f801080 && a < 0x1f801100) || (a >= 0x1f801c00 && a < 0x1f802000))
		return psxHwRead32(a);
	return cio_orig_fetch(r, luts, pc);
}

extern "C" void intEnsureExactAccess(void);   /* psxinterpreter.c */
extern "C" uint32_t mipsrec_cio_run(void)
{
	cio_runs++;
	if (psxRegs.pc & 3) {   /* jr/jalr to a misaligned target: AdEL on the fetch, EPC = BadVAddr = target (as doBranchRegE) */
		psxRegs.CP0.n.BadVAddr = psxRegs.pc;
		psxException(R3000E_AdEL << 2, R3000A_BRANCH_NONE_OR_EXCEPTION, &psxRegs.CP0);
		return psxRegs.pc;
	}
	intEnsureExactAccess();   /* exact AdEL/AdES as under the JIT */
	cio_orig_fetch = (cio_fetch_fn)psxRegs.ptrs.intFetch;
	psxRegs.ptrs.intFetch = cio_fetch;
	do {   /* psxRegs.pc was stored by the dispatch loop before it jumped here */
		psxInt.ExecuteBlock(&psxRegs, EXEC_CALLER_OTHER);
		cio_blocks++;
		if ((int32_t)(psxRegs.cycle - psxRegs.next_interupt) >= 0)
			mipsrec_branch_test();
	} while (!cio_has_slots(psxRegs.pc) && !psxRegs.stop);
	psxRegs.ptrs.intFetch = cio_orig_fetch;
	psxInt.Notify(R3000ACPU_NOTIFY_BEFORE_SAVE, NULL);   /* flush the load-delay pipeline */
	return psxRegs.pc;
}

/* recompiler.cpp rec_ldhaz_block: interpret one block that has a load-use
 * hazard, with the interpreter's exact load-delay semantics. Keep going
 * while a load is still pending at a block end (a load in the delay slot),
 * so the target's first instruction also sees the old value. Then flush
 * (mipsrec has no delay state), as mipsrec_cio_run does. */
/* the fall-through at ftpc runs with r's load pending: GPR r holds the old
 * value (saved by the block before its delay slot), the new one commits after
 * the next instruction, as the interpreter's doLoad leaves it */
extern "C" { uint32_t mipsrec_ldhaz_old; unsigned long mipsrec_ldhaz_ft_runs; }
extern "C" uint32_t mipsrec_ldhaz_ft(uint32_t ftpc, uint32_t r)
{
	mipsrec_ldhaz_ft_runs++;
	const int sel = psxRegs.dloadSel ^ 1;
	psxRegs.dloadReg[sel] = r;
	psxRegs.dloadVal[sel] = psxRegs.GPR.r[r];
	psxRegs.GPR.r[r] = mipsrec_ldhaz_old;
	return mipsrec_ldhaz_run(ftpc);
}

extern "C" uint32_t mipsrec_ldhaz_run(uint32_t pc)
{
	mipsrec_ldhaz_runs++;
	intEnsureExactAccess();
	psxRegs.pc = pc;
	do
		psxInt.ExecuteBlock(&psxRegs, EXEC_CALLER_OTHER);
	while ((psxRegs.dloadReg[0] | psxRegs.dloadReg[1]) && !psxRegs.stop);
	psxInt.Notify(R3000ACPU_NOTIFY_BEFORE_SAVE, NULL);   /* flush the load-delay pipeline */
	if ((int32_t)(psxRegs.cycle - psxRegs.next_interupt) >= 0)
		mipsrec_branch_test();
	return psxRegs.pc;
}

static void cio_emit(uintptr_t loop)
{
	const uintptr_t f = (uintptr_t)mipsrec_cio_run;
	cio_stub[0] = 0x0c000000 | ((f >> 2) & 0x03ffffff);      /* jal  mipsrec_cio_run (v0 = pc) */
	cio_stub[1] = 0;                                          /* nop */
	cio_stub[2] = 0x08000000 | ((loop >> 2) & 0x03ffffff);   /* j    loop top */
	cio_stub[3] = 0x00001821;                                 /* move v1, zero <BD> */
	__builtin___clear_cache((char *)cio_stub, (char *)(cio_stub + 4));
	cio_loop = loop;
}

/* the dispatch loop's slot load (lw t0,0(t2)) faulted for v0 = pc: hand the
 * loop the stub instead of a block pointer. 1 = redirected. */
static int cio_redirect(ucontext_t *uc, uintptr_t hpc)
{
	const uint32_t lw_t3_cycle = 0x8fcb0000 | (uint32_t)offsetof(psxRegisters, cycle);
	for (int k = 1; k <= 16; k++) {   /* LUT loop: top is 10 words up */
		const uintptr_t a = hpc - 4 * k;
		if (*(const uint32_t *)a == lw_t3_cycle) {           /* loop: lw t3, cycle(fp) */
			if (((a ^ (uintptr_t)cio_stub) & 0xf0000000) != 0)
				return 0;                                /* j can't reach: chain (crash) */
			if (a != cio_loop)
				cio_emit(a);
			uc->uc_mcontext.gregs[8] = (uintptr_t)cio_stub;   /* t0 = "block" */
			uc->uc_mcontext.pc = hpc + 4;
			return 1;
		}
	}
	return 0;
}

static void cio_sigsegv(int sig, siginfo_t *si, void *ucv)
{
	ucontext_t *uc = (ucontext_t *)ucv;
	const uintptr_t hpc = (uintptr_t)uc->uc_mcontext.pc;
	const uint32_t v0 = (uint32_t)uc->uc_mcontext.gregs[2], t2 = (uint32_t)uc->uc_mcontext.gregs[10];
	if (hpc && !(hpc & 3) && *(const uint32_t *)hpc == 0x8d480000 &&     /* lw t0, 0(t2) */
	    (uint32_t)(uintptr_t)si->si_addr == t2 &&
	    (t2 == (CIO_REC_RAM_VADDR | (v0 & 0x00ffffff)) ||                /* mmap dispatch (Part B) */
	     (t2 < 0x10000 && t2 == (v0 & 0xffff))) &&                       /* LUT, psxRecLUT[] == 0 */
	    cio_redirect(uc, hpc))
		return;
	if ((cio_prev.sa_flags & SA_SIGINFO) && cio_prev.sa_sigaction) {
		cio_prev.sa_sigaction(sig, si, ucv);
		return;
	}
	if (cio_prev.sa_handler == SIG_IGN)
		return;
	signal(SIGSEGV, SIG_DFL);   /* re-faults into the default action */
}

static void cio_install(void)
{
	if ((((uintptr_t)cio_stub ^ (uintptr_t)mipsrec_cio_run) & 0xf0000000) != 0) {
		fprintf(stderr, "mipsrec: code-outside-RAM fallback disabled (jal range)\n");
		return;
	}
	struct sigaction sa;
	memset(&sa, 0, sizeof sa);
	sa.sa_sigaction = cio_sigsegv;
	sa.sa_flags = SA_SIGINFO;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGSEGV, &sa, &cio_prev);
}

extern "C" void mipsrec_cio_stats(unsigned long *runs, unsigned long *blocks) { *runs = cio_runs; *blocks = cio_blocks; }

/* Unaligned host accesses (2026-10-22, step 1 of the exact guest AdEL/AdES
 * work). The kernel fixes unaligned user loads/stores up silently by default
 * (TIF_FIXADE); the emu thread turns that off (sysmips MIPS_FIXADE 0) so every
 * one arrives here as SIGBUS. This handler emulates the access exactly as the
 * kernel fixup does (step pc; when pc is a branch the access sat in its delay
 * slot: emulate the branch too) and counts it: host C code (packed structs,
 * plugins, libc) keeps working, and the counts show who does it
 * (EROSQ_PROF prints them at exit). Faults inside the JIT code cache are
 * guest unaligned accesses; until the exact AdEL/AdES path exists they get
 * the same kernel-equivalent completion and are counted as "jit". Signal-safe:
 * only context reads/writes. */
#include <sys/syscall.h>
#include <unistd.h>
static struct sigaction ua_prev;
static unsigned long ua_host, ua_jit, ua_bad;
static uintptr_t ua_jlo, ua_jhi;

static inline uint32_t ua_rd(const greg_t *g, int r) { return r ? (uint32_t)g[r] : 0; }
static inline void ua_wr(greg_t *g, int r, uint32_t v) { if (r) g[r] = (greg_t)(int32_t)v; }
static inline uint32_t *ua_fpr(mcontext_t *mc, int i) { return (uint32_t *)&mc->fpregs.fp_r.fp_dregs[i]; }   /* o32 FR=0: low word */

/* emulate the load/store word w at address a; 0 = not a handled access */
static int ua_access(mcontext_t *mc, uint32_t w)
{
	greg_t *g = mc->gregs;
	const int op = w >> 26, rs = (w >> 21) & 31, rt = (w >> 16) & 31;
	uint8_t *a = (uint8_t *)(uintptr_t)(ua_rd(g, rs) + (uint32_t)(int32_t)(int16_t)w);
	uint32_t v;
	switch (op) {
	case 0x21: ua_wr(g, rt, (uint32_t)(int32_t)(int16_t)(a[0] | a[1] << 8)); return 1;            /* lh  */
	case 0x25: ua_wr(g, rt, (uint32_t)(a[0] | a[1] << 8)); return 1;                               /* lhu */
	case 0x23: ua_wr(g, rt, (uint32_t)a[0] | a[1] << 8 | a[2] << 16 | (uint32_t)a[3] << 24); return 1;  /* lw */
	case 0x29: v = ua_rd(g, rt); a[0] = v; a[1] = v >> 8; return 1;                               /* sh  */
	case 0x2b: v = ua_rd(g, rt); a[0] = v; a[1] = v >> 8; a[2] = v >> 16; a[3] = v >> 24; return 1;   /* sw */
	case 0x31: *ua_fpr(mc, rt) = (uint32_t)a[0] | a[1] << 8 | a[2] << 16 | (uint32_t)a[3] << 24; return 1;  /* lwc1 */
	case 0x39: v = *ua_fpr(mc, rt); a[0] = v; a[1] = v >> 8; a[2] = v >> 16; a[3] = v >> 24; return 1;     /* swc1 */
	case 0x35:                                                                                     /* ldc1 */
		*ua_fpr(mc, rt) = (uint32_t)a[0] | a[1] << 8 | a[2] << 16 | (uint32_t)a[3] << 24;
		*ua_fpr(mc, rt | 1) = (uint32_t)a[4] | a[5] << 8 | a[6] << 16 | (uint32_t)a[7] << 24;
		return 1;
	case 0x3d:                                                                                     /* sdc1 */
		v = *ua_fpr(mc, rt); a[0] = v; a[1] = v >> 8; a[2] = v >> 16; a[3] = v >> 24;
		v = *ua_fpr(mc, rt | 1); a[4] = v; a[5] = v >> 8; a[6] = v >> 16; a[7] = v >> 24;
		return 1;
	}
	return 0;
}

/* branch/jump at pc (word w): 1 + *next = pc after it and its delay slot
 * (incl. link-register writes); 0 = not a branch */
static int ua_branch(mcontext_t *mc, uint32_t pc, uint32_t w, uint32_t *next)
{
	greg_t *g = mc->gregs;
	const int op = w >> 26, rs = (w >> 21) & 31, rt = (w >> 16) & 31, fn = w & 63;
	const int32_t s = (int32_t)ua_rd(g, rs), t = (int32_t)ua_rd(g, rt);
	const uint32_t tgt = pc + 4 + ((uint32_t)(int32_t)(int16_t)w << 2), fall = pc + 8;
	int take;
	switch (op) {
	case 0x00:
		if (fn == 0x08) { *next = (uint32_t)s; return 1; }                                  /* jr */
		if (fn == 0x09) { ua_wr(g, (w >> 11) & 31, fall); *next = (uint32_t)s; return 1; }  /* jalr (rs read first) */
		return 0;
	case 0x01:
		take = (rt & 1) ? s >= 0 : s < 0;                                                   /* bltz/bgez(+al/l) */
		if (rt & 0x10) ua_wr(g, 31, fall);
		*next = take ? tgt : fall; return 1;   /* likely variants never fault in their slot when not taken */
	case 0x02: *next = (pc & 0xf0000000) | ((w & 0x03ffffff) << 2); return 1;               /* j */
	case 0x03: ua_wr(g, 31, fall); *next = (pc & 0xf0000000) | ((w & 0x03ffffff) << 2); return 1;   /* jal */
	case 0x04: case 0x14: *next = s == t ? tgt : fall; return 1;                            /* beq(l) */
	case 0x05: case 0x15: *next = s != t ? tgt : fall; return 1;                            /* bne(l) */
	case 0x06: case 0x16: *next = s <= 0 ? tgt : fall; return 1;                            /* blez(l) */
	case 0x07: case 0x17: *next = s > 0 ? tgt : fall; return 1;                             /* bgtz(l) */
	case 0x11:
		if (rs == 0x08) {                                                                   /* bc1f/bc1t(l) */
			const int cc = (mc->fpc_csr >> 23) & 1;
			*next = (((w >> 16) & 1) ? cc : !cc) ? tgt : fall; return 1;
		}
		return 0;
	}
	return 0;
}

/* the trapping add/addi/sub w as the wrapping one (the pre-Ov behaviour);
 * 0 = not one */
static int ua_arith(mcontext_t *mc, uint32_t w)
{
	greg_t *g = mc->gregs;
	const int op = w >> 26, rs = (w >> 21) & 31, rt = (w >> 16) & 31, rd = (w >> 11) & 31;
	if (op == 0x08) { ua_wr(g, rt, ua_rd(g, rs) + (uint32_t)(int32_t)(int16_t)w); return 1; }
	if (op == 0 && (w & 0x7ff) == 0x20) { ua_wr(g, rd, ua_rd(g, rs) + ua_rd(g, rt)); return 1; }
	if (op == 0 && (w & 0x7ff) == 0x22) { ua_wr(g, rd, ua_rd(g, rs) - ua_rd(g, rt)); return 1; }
	return 0;
}

/* kernel-equivalent completion of the unaligned access (step 1) or the
 * overflowing add/sub at mc->pc; 0 = not a decodable one */
static int ua_complete(mcontext_t *mc)
{
	const uint32_t pc = (uint32_t)mc->pc, w = *(const uint32_t *)(uintptr_t)pc;
	uint32_t next, w2;
	if (ua_access(mc, w) || ua_arith(mc, w))
		mc->pc = pc + 4;
	else if (ua_branch(mc, pc, w, &next) &&
		 (w2 = *(const uint32_t *)(uintptr_t)(pc + 4), ua_access(mc, w2) || ua_arith(mc, w2)))
		mc->pc = next;   /* the access was in the branch's delay slot (EPC = branch) */
	else
		return 0;
	return 1;
}

/* Exact guest AdEL/AdES (2026-10-22, step 2; exactexc.h). A guest lh/lhu/lw/
 * sh/sw on an unaligned address faults in JIT code. The SIGBUS handler only
 * saves the context and redirects to ee_resolve (normal context, on the
 * interrupted stack). ee_resolve finds the block (ee_idx: host order since the
 * last cache reset), forks, and the child dry-run recompiles it in place to
 * learn the faulting guest instruction and the guest register file at that
 * point (undoing the jitsched permutation: ALU words scheduled after the
 * access but originally before it are emulated; ones hoisted above it that
 * overwrote a live guest register force the fallback). The parent then
 * raises the exception the interpreter would (EPC/BD/BT, BadVAddr, CE bits)
 * and resumes at the dispatch loop with v0 = the vector, v1 = 0. Otherwise
 * (counted: "fallback") the access completes as step 1 did. Either way the
 * saved context comes back through a deliberate trap (ee_trap: break) whose
 * SIGTRAP handler copies it into the ucontext, so sigreturn restores every
 * register atomically -- no k0/k1. EROSQ_EE=0 disables (step-1 behaviour),
 * EROSQ_EE=2 forces the fallback (tests the restore path). */
#include <sys/mman.h>
#include <sys/wait.h>
#include <errno.h>
#include "exactexc.h"
extern "C" { int mipsrec_dry_on; struct dry_acc *mipsrec_dry_acc; int mipsrec_dry_nacc; }

struct ee_blk { uint32_t host, gpc; };
#define EE_IDX_MAX (1u << 17)              /* 1 MB reserved, demand-zero */
static struct ee_blk *ee_idx;
static uint32_t ee_n, ee_peak;
static int ee_idx_bad;

static void ee_index(uint32_t *host, uint32_t gpc)
{
	if (!ee_idx) {
		if (ee_idx_bad)
			return;
		void *p = mmap(NULL, EE_IDX_MAX * sizeof *ee_idx, PROT_READ | PROT_WRITE,
			       MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
		if (p == MAP_FAILED) { ee_idx_bad = 1; return; }
		ee_idx = (struct ee_blk *)p;
	}
	if (ee_n && (uint32_t)(uintptr_t)host < ee_idx[ee_n - 1].host)
		ee_n = 0;                          /* code cache reset */
	if (ee_n < EE_IDX_MAX)                 /* full: later blocks fail the dry-run compare */
		ee_idx[ee_n++] = (struct ee_blk){ (uint32_t)(uintptr_t)host, gpc };
	if (ee_n > ee_peak)
		ee_peak = ee_n;
}

static mcontext_t ee_ctx;
static volatile int ee_pending;
static int ee_mode = 1;
extern "C" { unsigned long mipsrec_ovf_sites; }
static unsigned long ee_exact_n, ee_ovf_n, ee_fb_n, ee_why[8], ee_emul, ee_hoist;
static int ee_prof;   /* EROSQ_PROF: running counts from ee_resolve */
extern "C" void ee_trap(void);
asm(".pushsection .jitsched_text,\"ax\",@progbits\n"
    ".globl ee_trap\n.type ee_trap,@function\n.set push\n.set noreorder\n"
    "ee_trap:\n\tbreak\n\tnop\n.set pop\n.size ee_trap,8\n.popsection");

/* host branch/jump (the access then sits in its delay slot) */
static int ee_is_branch(uint32_t w)
{
	const int op = w >> 26;
	return op == 0x01 || (op >= 0x02 && op <= 0x07) || (op >= 0x14 && op <= 0x17) ||
	       (op == 0 && ((w & 63) == 0x08 || (w & 63) == 0x09)) || (op == 0x11 && ((w >> 21) & 31) == 0x08);
}

/* emulate one jitsched-movable ALU word (js_decode's set); 0 = not one */
static int ee_alu(uint32_t *g, uint32_t *hi, uint32_t *lo, uint32_t w)
{
	const int op = w >> 26, rs = (w >> 21) & 31, rt = (w >> 16) & 31, rd = (w >> 11) & 31,
		  sa = (w >> 6) & 31, fn = w & 63;
	const uint32_t s = g[rs], t = g[rt], imm = (uint32_t)(int32_t)(int16_t)w, immu = w & 0xffff;
	uint32_t v;
	int dst = rd;
	uint64_t acc;
	switch (op) {
	case 0x00:
		switch (fn) {
		case 0x00: v = t << sa; break;
		case 0x02: v = t >> sa; break;
		case 0x03: v = (uint32_t)((int32_t)t >> sa); break;
		case 0x04: v = t << (s & 31); break;
		case 0x06: v = t >> (s & 31); break;
		case 0x07: v = (uint32_t)((int32_t)t >> (s & 31)); break;
		case 0x0a: if (t) return 1; v = s; break;   /* movz */
		case 0x0b: if (!t) return 1; v = s; break;  /* movn */
		case 0x10: v = *hi; break;
		case 0x12: v = *lo; break;
		case 0x11: *hi = s; return 1;
		case 0x13: *lo = s; return 1;
		case 0x18: acc = (uint64_t)((int64_t)(int32_t)s * (int32_t)t); *lo = (uint32_t)acc; *hi = (uint32_t)(acc >> 32); return 1;
		case 0x19: acc = (uint64_t)s * t; *lo = (uint32_t)acc; *hi = (uint32_t)(acc >> 32); return 1;
		case 0x1a:
			if (!t || (s == 0x80000000u && t == 0xffffffffu)) return 0;
			*lo = (uint32_t)((int32_t)s / (int32_t)t); *hi = (uint32_t)((int32_t)s % (int32_t)t); return 1;
		case 0x1b: if (!t) return 0; *lo = s / t; *hi = s % t; return 1;
		case 0x21: v = s + t; break;
		case 0x23: v = s - t; break;
		case 0x24: v = s & t; break;
		case 0x25: v = s | t; break;
		case 0x26: v = s ^ t; break;
		case 0x27: v = ~(s | t); break;
		case 0x2a: v = (int32_t)s < (int32_t)t; break;
		case 0x2b: v = s < t; break;
		default: return 0;
		}
		break;
	case 0x09: dst = rt; v = s + imm; break;
	case 0x0a: dst = rt; v = (int32_t)s < (int32_t)imm; break;
	case 0x0b: dst = rt; v = s < imm; break;
	case 0x0c: dst = rt; v = s & immu; break;
	case 0x0d: dst = rt; v = s | immu; break;
	case 0x0e: dst = rt; v = s ^ immu; break;
	case 0x0f: dst = rt; v = immu << 16; break;
	case 0x1c:
		acc = (uint64_t)*hi << 32 | *lo;
		switch (fn) {
		case 0x02: v = s * t; break;                                           /* mul */
		case 0x00: acc += (uint64_t)((int64_t)(int32_t)s * (int32_t)t); goto setacc;   /* madd */
		case 0x01: acc += (uint64_t)s * t; goto setacc;                        /* maddu */
		case 0x04: acc -= (uint64_t)((int64_t)(int32_t)s * (int32_t)t); goto setacc;   /* msub */
		case 0x05: acc -= (uint64_t)s * t; goto setacc;                        /* msubu */
		case 0x20: v = s ? __builtin_clz(s) : 32; break;                       /* clz */
		case 0x21: v = ~s ? __builtin_clz(~s) : 32; break;                     /* clo */
		default: return 0;
		}
		break;
	setacc:
		*lo = (uint32_t)acc; *hi = (uint32_t)(acc >> 32); return 1;
	case 0x1f:
		if (fn == 0x00) {                                                      /* ext */
			const int size = rd + 1;
			dst = rt; v = (s >> sa) & (size >= 32 ? ~0u : (1u << size) - 1);
		} else if (fn == 0x04) {                                               /* ins */
			const int size = rd - sa + 1;
			const uint32_t m = (size >= 32 ? ~0u : (1u << size) - 1) << sa;
			dst = rt; v = (t & ~m) | ((s << sa) & m);
		} else if (fn == 0x20 && sa == 0x10) v = (uint32_t)(int32_t)(int8_t)t;      /* seb */
		else if (fn == 0x20 && sa == 0x18) v = (uint32_t)(int32_t)(int16_t)t;       /* seh */
		else if (fn == 0x20 && sa == 0x02) v = ((t & 0x00ff00ffu) << 8) | ((t >> 8) & 0x00ff00ffu);   /* wsbh */
		else return 0;
		break;
	default:
		return 0;
	}
	if (dst)
		g[dst] = v;
	return 1;
}

struct ee_res { int ok; uint32_t gpc, gpr[32], nemul, nhoist; };
static uint32_t ee_live[DRY_MAX_WORDS];    /* BSS, touched only in the child */
static uint16_t ee_perm[DRY_MAX_WORDS];
static struct dry_acc ee_acc[DRY_MAX_ACC];

/* child: dry-run the block at hs (guest gpc0), fault at scheduled word F */
static void ee_child(uint32_t *hs, uint32_t gpc0, uint32_t F, int fd)
{
	struct ee_res r;
	memset(&r, 0, sizeof r);
	uint32_t avail = (uint32_t)((ua_jhi - (uintptr_t)hs) / 4), uF, i, n;
	const struct dry_acc *a = NULL;
	if (avail > DRY_MAX_WORDS)
		avail = DRY_MAX_WORDS;
	memcpy(ee_live, hs, avail * 4);
	mipsrec_dry_acc = ee_acc;
	mipsrec_dry_nacc = 0;
	js_perm = ee_perm;
	mipsrec_dry_on = 1;
	n = (uint32_t)mipsrec_dry_run(gpc0, hs);
	mipsrec_dry_on = 0;
	if (n > avail || F >= n || memcmp(hs, ee_live, n * 4) || !js_perm) { r.ok = -1; goto out; }
	uF = ee_perm[F];
	for (int k = 0; k < mipsrec_dry_nacc; k++)
		if (ee_acc[k].hoff == uF) { a = &ee_acc[k]; break; }
	if (!a) { r.ok = -2; goto out; }
	{
		uint32_t g[32], hi = (uint32_t)ee_ctx.mdhi, lo = (uint32_t)ee_ctx.mdlo;
		for (i = 0; i < 32; i++) g[i] = (uint32_t)ee_ctx.gregs[i];
		g[0] = 0;
		/* original order: words scheduled after the access but originally
		 * before it have not run yet -- run them (ascending original index) */
		for (uint32_t want = 0; want < uF; want++)
			for (i = F + 1; i < n; i++)
				if (ee_perm[i] == want) {
					if (!ee_alu(g, &hi, &lo, ee_live[i])) { r.ok = -3; goto out; }
					r.nemul++;
					break;
				}
		/* words hoisted above it already ran: fatal if one overwrote a live guest reg */
		for (i = 0; i < F; i++)
			if (ee_perm[i] > uF) {
				struct js_ins d;
				if (js_decode(ee_live[i], &d) == JS_BAR) { r.ok = -4; goto out; }
				r.nhoist++;
				for (int gr = 1; gr < 32; gr++)
					if (!(a->cmask >> gr & 1) && (a->map[gr] & 0x80) && (d.dst >> (a->map[gr] & 31) & 1)) {
						r.ok = -4; goto out;
					}
			}
		for (int gr = 1; gr < 32; gr++)
			r.gpr[gr] = (a->cmask >> gr & 1) ? a->cval[gr] :
				    (a->map[gr] & 0x80) ? g[a->map[gr] & 31] : psxRegs.GPR.r[gr];
		r.gpc = a->gpc;
		r.ok = 1;
	}
out:
	{ ssize_t w = write(fd, &r, sizeof r); (void)w; }
}

/* guest branch at bpc (word w) taken, given the guest registers */
static int ee_taken(uint32_t w, const uint32_t *gpr)
{
	const int op = w >> 26;
	const int32_t s = (int32_t)gpr[(w >> 21) & 31], t = (int32_t)gpr[(w >> 16) & 31];
	switch (op) {
	case 0x01: return ((w >> 16) & 1) ? s >= 0 : s < 0;
	case 0x04: return s == t;
	case 0x05: return s != t;
	case 0x06: return s <= 0;
	case 0x07: return s > 0;
	}
	return 1;   /* j jal jr jalr */
}

static int ee_guest_branch(uint32_t w)
{
	const int op = w >> 26;
	return (op >= 0x01 && op <= 0x07) || (op == 0 && ((w & 63) == 0x08 || (w & 63) == 0x09));
}

static int ee_exact(void)
{
	const uintptr_t loop = mipsrec_block_ret_addr();
	const uint32_t pc = (uint32_t)ee_ctx.pc;
	if (!loop || !ee_idx || !ee_n) return 0;
	uint32_t lo = 0, hi = ee_n;                /* last block starting <= pc */
	while (hi - lo > 1) {
		const uint32_t mid = (lo + hi) / 2;
		if (ee_idx[mid].host <= pc) lo = mid; else hi = mid;
	}
	if (ee_idx[lo].host > pc) return 0;
	uint32_t *const hs = (uint32_t *)(uintptr_t)ee_idx[lo].host;
	const uint32_t gpc0 = ee_idx[lo].gpc;
	uint32_t F = (pc - ee_idx[lo].host) / 4, w = *(const uint32_t *)(uintptr_t)pc;
	if (ee_is_branch(w)) { F++; w = *(const uint32_t *)(uintptr_t)(pc + 4); }
	const int hop = w >> 26;
	const int ovf = hop == 0x08 || (hop == 0 && ((w & 0x7ff) == 0x20 || (w & 0x7ff) == 0x22));
	if (!ovf && hop != 0x21 && hop != 0x25 && hop != 0x23 && hop != 0x29 && hop != 0x2b) return 0;
	if (F >= DRY_MAX_WORDS) return 0;

	int fd[2];
	if (pipe(fd)) return 0;
	const pid_t pid = fork();
	if (pid == 0) {
		close(fd[0]);
		ee_child(hs, gpc0, F, fd[1]);
		_exit(0);
	}
	close(fd[1]);
	struct ee_res r;
	size_t got = 0;
	while (pid > 0 && got < sizeof r) {
		const ssize_t k = read(fd[0], (char *)&r + got, sizeof r - got);
		if (k > 0) got += k;
		else if (k == 0 || errno != EINTR) break;
	}
	close(fd[0]);
	if (pid > 0)
		while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}
	if (got != sizeof r || r.ok != 1) {
		ee_why[got != sizeof r ? 0 : (-r.ok) & 7]++;
		return 0;
	}

	ee_emul += r.nemul;
	ee_hoist += r.nhoist;
	/* the guest instruction and its address */
	const uint32_t *gp = is_ptr(r.gpc), *pp = r.gpc != gpc0 ? is_ptr(r.gpc - 4) : NULL;
	if (!gp) { ee_why[5]++; return 0; }
	const uint32_t gw = *gp, op = gw >> 26, rs = (gw >> 21) & 31;
	const uint32_t addr = r.gpr[rs] + (uint32_t)(int32_t)(int16_t)gw;
	if (ovf ? !(op == 0x08 || (op == 0 && ((gw & 0x7ff) == 0x20 || (gw & 0x7ff) == 0x22)))
		: (!(addr & ((op == 0x23 || op == 0x2b) ? 3 : 1)) || op != (uint32_t)hop)) { ee_why[6]++; return 0; }
	const int bd = pp && ee_guest_branch(*pp);

	for (int k = 1; k < 32; k++)
		psxRegs.GPR.r[k] = r.gpr[k];
	if (ovf) {   /* as psxADD/psxADDI/psxSUB: Ov, rd/rt unwritten */
		psxRegs.cycle += is_clk((r.gpc - gpc0) / 4 + 1);
		psxRegs.pc = r.gpc;
		const enum R3000Abdt bdt = !bd ? R3000A_BRANCH_NONE_OR_EXCEPTION :
			ee_taken(*pp, r.gpr) ? R3000A_BRANCH_TAKEN : R3000A_BRANCH_NOT_TAKEN;
		psxException((R3000E_Ov << 2) | ((gw & 0x0c000000) << 2), bdt, &psxRegs.CP0);
		ee_ovf_n++;
		ee_ctx.pc = loop;
		ee_ctx.gregs[2] = (greg_t)(int32_t)psxRegs.pc;
		ee_ctx.gregs[3] = 0;
		return 1;
	}
	/* earlier stores of the same series ran but their SMC check comes after
	 * the series: clear for them as the block would have */
	for (uint32_t k = r.gpc - 4; k >= gpc0 && k < r.gpc; k -= 4) {
		const uint32_t *kp = is_ptr(k);
		if (!kp) break;
		const uint32_t kw = *kp, ko = kw >> 26;
		if (!kw) continue;
		if (ko < 0x20 || ko > 0x2e) break;
		if (ko < 0x28 && ((kw >> 16) & 31) == rs) break;   /* load rewrote the base */
		if (ko >= 0x28 && ((kw >> 21) & 31) == rs)
			psxCpu->Clear((r.gpr[rs] + (uint32_t)(int32_t)(int16_t)kw) & ~3u, 1);
	}
	psxRegs.cycle += is_clk((r.gpc - gpc0) / 4 + 1);
	psxRegs.CP0.n.BadVAddr = addr;
	psxRegs.pc = r.gpc;
	const enum R3000Abdt bdt = !bd ? R3000A_BRANCH_NONE_OR_EXCEPTION :
		ee_taken(*pp, r.gpr) ? R3000A_BRANCH_TAKEN : R3000A_BRANCH_NOT_TAKEN;
	psxException(((op >= 0x28 ? R3000E_AdES : R3000E_AdEL) << 2) | ((gw & 0x0c000000) << 2), bdt, &psxRegs.CP0);
	ee_ctx.pc = loop;
	ee_ctx.gregs[2] = (greg_t)(int32_t)psxRegs.pc;   /* v0 = next pc */
	ee_ctx.gregs[3] = 0;                             /* v1 = cycles (charged above) */
	return 1;
}

extern "C" void __attribute__((noreturn, used)) ee_resolve(void)
{
	if (ee_mode == 1 && ee_exact())
		ee_exact_n++;
	else {
		ee_fb_n++;
		if (!ua_complete(&ee_ctx)) {
			fprintf(stderr, "mipsrec: undecodable unaligned JIT access at %08x\n", (uint32_t)ee_ctx.pc);
			abort();
		}
	}
	/* EROSQ_PROF: a running count from normal context (the exit summary is an
	 * atexit handler, which a SIGINT exit (_exit) skips): 1st event, then every 64 */
	if (ee_prof && ((ee_exact_n + ee_fb_n) & 63) == 1)
		fprintf(stderr, "unaligned: running exact %lu fallback %lu overflow %lu\n", ee_exact_n, ee_fb_n, ee_ovf_n);
	ee_trap();
	__builtin_unreachable();
}

static struct sigaction ee_prev_trap;
static void ee_sigtrap(int sig, siginfo_t *si, void *ucv)
{
	ucontext_t *uc = (ucontext_t *)ucv;
	if (ee_pending && (uintptr_t)uc->uc_mcontext.pc == (uintptr_t)ee_trap) {
		uc->uc_mcontext = ee_ctx;
		ee_pending = 0;
		return;
	}
	if ((ee_prev_trap.sa_flags & SA_SIGINFO) && ee_prev_trap.sa_sigaction) { ee_prev_trap.sa_sigaction(sig, si, ucv); return; }
	if (ee_prev_trap.sa_handler == SIG_IGN) return;
	signal(SIGTRAP, SIG_DFL);
}

static int ee_redirect(mcontext_t *mc);
/* SIGFPE in JIT code from a guest ADD/ADDI/SUB (host trapping add/addi/sub,
 * possibly in a host branch's delay slot): raise the guest Ov exception */
static struct sigaction ee_prev_fpe;
static void ee_sigfpe(int sig, siginfo_t *si, void *ucv)
{
	mcontext_t *mc = &((ucontext_t *)ucv)->uc_mcontext;
	const uint32_t pc = (uint32_t)mc->pc;
	if (pc >= ua_jlo && pc < ua_jhi && !(pc & 3)) {
		uint32_t w = *(const uint32_t *)(uintptr_t)pc;
		if (ee_is_branch(w))
			w = *(const uint32_t *)(uintptr_t)(pc + 4);
		const int op = w >> 26;
		if ((op == 0x08 || (op == 0 && ((w & 0x7ff) == 0x20 || (w & 0x7ff) == 0x22))) &&
		    (ee_redirect(mc) || ua_complete(mc)))
			return;
	}
	if ((ee_prev_fpe.sa_flags & SA_SIGINFO) && ee_prev_fpe.sa_sigaction) { ee_prev_fpe.sa_sigaction(sig, si, ucv); return; }
	if (ee_prev_fpe.sa_handler == SIG_IGN) return;
	signal(SIGFPE, SIG_DFL);
}

/* SIGBUS in JIT code: save the context, run ee_resolve below the interrupted sp */
static int ee_redirect(mcontext_t *mc)
{
	if (!ee_mode || ee_pending)
		return 0;
	ee_ctx = *mc;
	ee_pending = 1;
	mc->pc = (uintptr_t)ee_resolve;
	mc->gregs[25] = (greg_t)(uintptr_t)ee_resolve;
	mc->gregs[29] = (mc->gregs[29] - 64) & ~(greg_t)7;
	return 1;
}

static int ua_zero_siaddr;   /* EROSQ_TEST_SIADDR0=1: offline test hook, see ua_sigbus */
static void ua_sigbus(int sig, siginfo_t *si, void *ucv)
{
	/* the device kernel's address-error SIGBUS carries si_addr = 0, qemu-user
	 * fills it in: the hook makes qemu runs see the device's siginfo, so
	 * nothing below may depend on si_addr (offline/siaddr0.sh) */
	if (ua_zero_siaddr)
		si->si_addr = 0;
	ucontext_t *uc = (ucontext_t *)ucv;
	mcontext_t *mc = &uc->uc_mcontext;
	const uint32_t pc = (uint32_t)mc->pc;
	if (pc && !(pc & 3)) {
		const uint32_t w = *(const uint32_t *)(uintptr_t)pc;
		const uint32_t v0 = (uint32_t)mc->gregs[2], t2 = (uint32_t)mc->gregs[10];
		/* no si_addr test: the device kernel's address-error SIGBUS carries
		 * si_addr = 0 (qemu fills it in); t2 is the faulting address here */
		if (w == 0x8d480000 && (v0 & 3) &&                                            /* dispatch slot load, */
#ifdef SIADDR_MUTANT   /* offline mutant: the pre-ff97856 si_addr test (siaddr0.sh must catch it) */
		    (uint32_t)(uintptr_t)si->si_addr == t2 &&
#endif
		    (t2 & 3) == (v0 & 3) && !(pc >= ua_jlo && pc < ua_jhi) &&                    /* misaligned guest pc */
		    cio_redirect(uc, pc))
			return;   /* the stub raises AdEL (mipsrec_cio_run) */
		if (pc >= ua_jlo && pc < ua_jhi) {
			ua_jit++;
			if (ee_redirect(mc))
				return;   /* ee_resolve raises AdEL/AdES or completes it */
		}
		if (!ua_complete(mc)) {
			ua_bad++;
			goto chain;
		}
		if (!(pc >= ua_jlo && pc < ua_jhi)) ua_host++;
		return;
	}
chain:
	if ((ua_prev.sa_flags & SA_SIGINFO) && ua_prev.sa_sigaction) { ua_prev.sa_sigaction(sig, si, ucv); return; }
	if (ua_prev.sa_handler == SIG_IGN) return;
	signal(SIGBUS, SIG_DFL);
}

static void ua_report(void)
{
	fprintf(stderr, "unaligned: host %lu jit %lu undecoded %lu\n", ua_host, ua_jit, ua_bad);
	fprintf(stderr, "ldhaz: %lu blocks interpreted, %lu runs (%lu fall-through)\n", mipsrec_ldhaz_blocks, mipsrec_ldhaz_runs, mipsrec_ldhaz_ft_runs);
	fprintf(stderr, "unaligned: exact %lu (overflow %lu; %lu trapping add/sub compiled) fallback %lu index-peak %u blocks; emulated %lu hoisted %lu (why io %lu cmp %lu acc %lu alu %lu hoist %lu bios %lu chk %lu)\n",
		ee_exact_n, ee_ovf_n, mipsrec_ovf_sites, ee_fb_n, ee_peak, ee_emul, ee_hoist, ee_why[0], ee_why[1], ee_why[2], ee_why[3], ee_why[4], ee_why[5], ee_why[6]);
}

extern "C" void mipsrec_ua_report(void) { ua_report(); }   /* offline harness */

static void ua_install(void)
{
	mipsrec_code_range(&ua_jlo, &ua_jhi);
	struct sigaction sa;
	memset(&sa, 0, sizeof sa);
	sa.sa_sigaction = ua_sigbus;
	sa.sa_flags = SA_SIGINFO;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGBUS, &sa, &ua_prev);
	{
		ua_zero_siaddr = getenv("EROSQ_TEST_SIADDR0") != NULL;
		const char *e = getenv("EROSQ_EE");
		ee_mode = e ? atoi(e) : 1;
		sa.sa_sigaction = ee_sigtrap;
		sigaction(SIGTRAP, &sa, &ee_prev_trap);
		sa.sa_sigaction = ee_sigfpe;
		sigaction(SIGFPE, &sa, &ee_prev_fpe);
	}
	syscall(__NR_sysmips, 7 /* MIPS_FIXADE */, 0, 0);   /* this (emu) thread only; ENOSYS under qemu-user (SIGBUS there anyway) */
	if (getenv("EROSQ_PROF")) {
		ee_prof = 1;
		atexit(ua_report);
	}
}

extern "C" void mipsrec_unaligned_stats(unsigned long *host, unsigned long *jit, unsigned long *bad) { *host = ua_host; *jit = ua_jit; *bad = ua_bad; }

static void jitsched_init(void)
{
	jitsched_inited = 1;
	const char *e = getenv("EROSQ_JITSCHED");
	jitsched_on = e ? atoi(e) : 1;   /* default ON; EROSQ_JITSCHED=0 is the kill-switch */
	jitsched_stat = getenv("EROSQ_JITSCHED_STAT") != NULL;
	blkext_stat = getenv("EROSQ_BLKEXT_STAT") != NULL;
	idleskip_init();
	cio_install();
	ua_install();
	if (jitsched_on > 0)
		fprintf(stderr, "mipsrec: jitsched on\n");   /* stderr: unbuffered, survives SIGTERM */
}

/* Block extents (2026-10-20). mipsrec's Clear() (DMA from CD/GPU/SPU, C-path
 * psxMemWrite stores, HLE memcpy/memset, EXE loads) only zeroes the lookup
 * slots of blocks STARTING inside the range, so a compiled block that starts
 * earlier and runs into the range kept executing the overwritten code. Each
 * compiled RAM block's guest [start, end) is recorded here per 4 KB page it
 * overlaps (compile time only), and mipsrec_blkext_clear() zeroes the slot of
 * every block that starts before a cleared range but overlaps it, then does
 * the normal clear. Cold paths only: emitted code is unchanged.
 *
 * Accepted deviation (documented, not fixed here): guest stores via the inline
 * path (rec_lsu.cpp.h, which zeroes only the written word's own slot) into
 * code of a currently-compiled block -- mid-block words, a block's own first
 * word on a fastpath self-loop, merged branch targets ('b' to the next insn is
 * dropped) -- take effect at the next flush / reset / recClear covering it
 * (BIOS FlushCache / cache isolation -> recClear of all RAM).
 * Real hardware is also stale until the 4 KB I-cache evicts the line. Fixing
 * it exactly costs a check on every guest store; measured offline (GT2 race +
 * boot, Crash boot) no game hit it. hlink.sh coh classifies those fixtures as
 * known-inline-diff. */
#define BX_PAGES (0x200000 >> 12)
struct bx { uint32_t s, e; };                  /* RAM byte offsets [s, e) */
static struct bx *bx_list[BX_PAGES];
static uint32_t bx_n[BX_PAGES], bx_cap[BX_PAGES];
static size_t bx_bytes, bx_peak;
static uintptr_t bx_last_host;
extern "C" void mipsrec_rec_clear(uint32_t addr, uint32_t size);

static void bx_reset_all(void)
{
	for (unsigned p = 0; p < BX_PAGES; p++) {
		free(bx_list[p]);
		bx_list[p] = NULL;
		bx_n[p] = bx_cap[p] = 0;
	}
	bx_bytes = 0;
}

static void bx_remove(unsigned p, uint32_t s)
{
	for (uint32_t i = 0; i < bx_n[p]; i++)
		if (bx_list[p][i].s == s) { bx_list[p][i] = bx_list[p][--bx_n[p]]; return; }
}

static void bx_register(uint32_t s, uint32_t e)
{
	for (unsigned p = s >> 12; p <= (e - 1) >> 12 && p < BX_PAGES; p++) {
		bx_remove(p, s);                             /* recompiled at the same start */
		if (bx_n[p] == bx_cap[p]) {
			uint32_t nc = bx_cap[p] ? bx_cap[p] * 2 : 8;
			struct bx *n = (struct bx *)realloc(bx_list[p], nc * sizeof *n);
			if (!n) return;                          /* out of memory: degrade to start-only clears */
			bx_bytes += (nc - bx_cap[p]) * sizeof *n;
			bx_list[p] = n; bx_cap[p] = nc;
		}
		bx_list[p][bx_n[p]++] = (struct bx){ s, e };
	}
	if (bx_bytes > bx_peak) {
		if (blkext_stat && (bx_bytes >> 12) != (bx_peak >> 12))   /* each new 4 KB step */
			fprintf(stderr, "blkext: peak %zu bytes\n", bx_bytes);
		bx_peak = bx_bytes;
	}
}

/* called by recRecompile (via jitsched_tail) for every block: host entry,
 * guest instruction count; the block's guest start is psxRegs.pc */
static void bx_block(uint32_t *host, uint32_t guest)
{
	/* the code cache restarts at its base on recReset (incl. the memfix cap
	 * flush): a block below the previous one means every old block is gone */
	if ((uintptr_t)host < bx_last_host)
		bx_reset_all();
	bx_last_host = (uintptr_t)host;
	const uint32_t pc = psxRegs.pc;
	ee_index(host, pc);
	if ((pc & 0x1fffffff) >= 0x800000 || !guest || guest > 0x10000)   /* RAM (+mirrors) only */
		return;
	const uint32_t s = pc & 0x1ffffc;
	uint32_t e = s + guest * 4;
	if (gtenf_hi > e)   /* EROSQ_GTENF read code past the block's end */
		e = gtenf_hi;
	gtenf_hi = 0;
	if (e > 0x200000)
		return;
	bx_register(s, e);
}

/* psxCpu->Clear (plugin.c mipsrec_clear): also invalidate blocks that start
 * before [addr, addr + size words) but overlap it */
extern "C" void mipsrec_blkext_clear(uint32_t addr, uint32_t size)
{
	const uint32_t a = addr & 0x1ffffc;
	uint64_t len = (uint64_t)size * 4;
	if (len > 0x200000 - a)
		len = 0x200000 - a;
#ifndef BLKEXT_MUTANT   /* offline mutant: start-only clears (the old behaviour) */
	if (len) {
		const uint32_t e = a + (uint32_t)len;
		for (unsigned p = a >> 12; p <= (e - 1) >> 12; p++)
			for (uint32_t i = 0; i < bx_n[p];) {
				const struct bx x = bx_list[p][i];
				if (x.s < a && x.e > a) {            /* starts before, overlaps */
					mipsrec_rec_clear(x.s, 1);
					for (unsigned q = x.s >> 12; q <= (x.e - 1) >> 12 && q < BX_PAGES; q++)
						bx_remove(q, x.s);
					continue;                        /* slot i now holds another entry */
				}
				i++;
			}
	}
#endif
	mipsrec_rec_clear(addr, size);
	/* blocks starting inside the range are gone now: drop their extents */
	if (len) {
		const uint32_t e = a + (uint32_t)len;
		for (unsigned p = a >> 12; p <= (e - 1) >> 12; p++)
			for (uint32_t i = 0; i < bx_n[p];) {
				const struct bx x = bx_list[p][i];
				if (x.s >= a && x.s < e) {
					for (unsigned q = x.s >> 12; q <= (x.e - 1) >> 12 && q < BX_PAGES; q++)
						bx_remove(q, x.s);
					continue;
				}
				i++;
			}
	}
}

extern "C" void mipsrec_blkext_stats(size_t *bytes, size_t *peak)
{
	*bytes = bx_bytes; *peak = bx_peak;
}

/* called by recRecompile once per emitted block [start, end); also keeps the
 * emitstat block counters (es != 0). Returns 1 when emitstat should dump. */
int jitsched_tail(uint32_t *start, uint32_t *end, int es, unsigned long long *es_blocks,
		  unsigned long long *es_block_bytes, unsigned long long *es_block_guest, uint32_t bytes, uint32_t guest)
{
	if (!jitsched_inited)
		jitsched_init();
	bx_block(start, guest);
	uint32_t *room = start;
	if (cur_idle) {
#ifdef HX_TTY
		const uint32_t h = cur_idle == IS_N ? (uint32_t)(uintptr_t)mipsrec_tty_entry : (uint32_t)(uintptr_t)mipsrec_idle_entry;
#else
		const uint32_t h = (uint32_t)(uintptr_t)mipsrec_idle_entry;
#endif
		room[0] = 0x3c040000 | (cur_pc >> 16);               /* lui  a0, hi(pc) */
		room[1] = 0x03e02821;                                /* move a1, ra */
		room[2] = 0x0c000000 | ((h >> 2) & 0x03ffffff);      /* jal  mipsrec_idle_entry */
		room[3] = 0x34840000 | (cur_pc & 0xffff);            /* ori  a0, a0, lo(pc) <BD> */
		room[4] = 0x0060f821;                                /* move ra, v1 */
		room += 5;
		if (cur_idle < IS_N)
			is_blocks[cur_idle]++;
	}
	cur_idle = cur_words = 0;
	if (js_perm) {   /* exact-exception dry run (child): identity, then the permutation */
		if (end - start > DRY_MAX_WORDS)
			js_perm = NULL;
		else
			for (uint32_t i = 0; i < (uint32_t)(end - start); i++) js_perm[i] = (uint16_t)i;
		js_perm_base = start;
	}
	if (jitsched_on > 0) {
		const int c = jitsched_block(start, end);
		js_blocks++;
		js_changed += c != 0;
		js_regions += c;
		if (jitsched_stat && (js_blocks & 4095) == 0)
			fprintf(stderr, "jitsched: %lu blocks, %lu changed, %lu regions\n", js_blocks, js_changed, js_regions);
	}
	if (!es)
		return 0;
	++*es_blocks;
	*es_block_bytes += bytes;
	*es_block_guest += guest;
	return (*es_blocks & 4095) == 0;
}

/* Offline test hook (hlink / smcstress / divtest): when set, called INSTEAD of
 * gen_interupt() at every horizon (mipsrec_branch_test, plugin.c). NULL in the
 * emulator; only offline harnesses assign it. A pointer rather than
 * -Wl,--wrap=gen_interupt, which cannot redirect a call that -flto resolves
 * inside the merged unit. Lives here (appended .jitsched_text, .bss last) so
 * arming support moves no existing code or data. */
union psxCP0Regs_;
extern "C" void gen_interupt(union psxCP0Regs_ *cp0);
extern "C" void (*mipsrec_irq_test_hook)(union psxCP0Regs_ *cp0);
void (*mipsrec_irq_test_hook)(union psxCP0Regs_ *cp0);
extern "C" void mipsrec_branch_test_hooked(union psxCP0Regs_ *cp0)
{
	if (__builtin_expect(mipsrec_irq_test_hook != NULL, 0))
		mipsrec_irq_test_hook(cp0);
	else
		gen_interupt(cp0);
}

/* offline harness hooks (hexec HX_SCHED, plat_headless) */
extern "C" void mipsrec_jitsched_set(int on) { jitsched_inited = 1; jitsched_on = on; }
extern "C" void mipsrec_jitsched_stats(unsigned long *blocks, unsigned long *changed, unsigned long *regions)
{ *blocks = js_blocks; *changed = js_changed; *regions = js_regions; }

/* A/B pixel gates (2026-10-22, default off), via -Wl,--wrap=drm_present so
 * the shipped frontend code is untouched (only its call target changes) and
 * no new dynamic import appears: every presented frame passes through here.
 * EROSQ_FHASH=N: stderr "FHASH flip=<k> 320x240 crc=<hex>" every N presents
 * (the hash is FNV-1a 32 of the visible 320x240 scanout, i.e. the
 * converter's output); EROSQ_SHOT_FLIP=K: write it to /media/sd/psx/shot.ppm
 * at present K. Weak refs: harness builds have no drm_out.o. */
extern "C" int __real_drm_present(void) __attribute__((weak));
extern "C" uint16_t *drm_pixels(int *stride_px) __attribute__((weak));
extern "C" int __wrap_drm_present(void)
{
	static int init, fh_every, shot_flip;
	static unsigned long flips;
	if (!init) {
		const char *e = getenv("EROSQ_FHASH"), *k = getenv("EROSQ_SHOT_FLIP");
		init = 1;
		fh_every = e ? atoi(e) : 0;
		shot_flip = k ? atoi(k) : 0;
	}
	flips++;
	if ((fh_every > 0 || shot_flip > 0) && drm_pixels) {
		int stride;
		const uint16_t *fb = drm_pixels(&stride);
		if (fb && fh_every > 0 && flips % (unsigned long)fh_every == 0) {
			uint32_t h = 2166136261u;
			for (int y = 0; y < 240; y++)
				for (int x = 0; x < 320; x++) {
					const uint16_t p = fb[y * stride + x];
					h = (h ^ (p & 0xff)) * 16777619u;
					h = (h ^ (p >> 8)) * 16777619u;
				}
			fprintf(stderr, "FHASH flip=%lu 320x240 crc=%08x\n", flips, h);
		}
		if (fb && shot_flip > 0 && (long)flips == shot_flip) {
			FILE *f = fopen("/media/sd/psx/shot.ppm", "wb");
			if (f) {
				fprintf(f, "P6\n320 240\n255\n");
				for (int y = 0; y < 240; y++)
					for (int x = 0; x < 320; x++) {
						const uint16_t p = fb[y * stride + x];
						const unsigned char rgb[3] = { (unsigned char)(((p >> 11) & 0x1f) << 3),
							(unsigned char)(((p >> 5) & 0x3f) << 2), (unsigned char)((p & 0x1f) << 3) };
						fwrite(rgb, 1, 3, f);
					}
				fclose(f);
				fprintf(stderr, "shot: wrote /media/sd/psx/shot.ppm 320x240 at flip %lu\n", flips);
			}
		}
	}
	return __real_drm_present ? __real_drm_present() : 0;
}
